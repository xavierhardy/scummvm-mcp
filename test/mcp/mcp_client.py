#!/usr/bin/env python3
"""MCP client + connection helpers for the integration tests.

A synchronous client speaking ScummVM's Streamable-HTTP MCP wire protocol
(JSON-RPC 2.0 over HTTP, with SSE for streaming tools), plus the per-worker port
allocator and a readiness poll. Imported via ``utils`` for back-compat.
"""

import json
import os
import time
from typing import Any

import httpx
import jsonschema

MCP_HOST = "127.0.0.1"
MCP_PORT = 23456
# Per-request HTTP timeout. Streaming tools (act/answer/walk) hold the SSE
# connection open for the duration of a cutscene; under heavy parallelism
# (--dist=loadgroup spins up many ScummVM instances at once) long cutscenes —
# e.g. The Dig's ~1-minute leave-scene argument — stream their lines slowly, so
# the gap between SSE events can exceed a short read timeout. Keep this generous
# so contention only makes tests slower, never spuriously timed-out.
MCP_TIMEOUT_SECS = 60.0
MCP_CONNECT_TIMEOUT_SECS = 30.0
MCP_TOOLS = ("state", "act", "answer", "walk", "skip")

# Base for per-(worker, fixture) MCP ports (see get_mcp_port).
MCP_PORT_BASE = 23400


def _wire_tool_name(display: str) -> str:
    """ "ChooseKids" -> "choose_kids": the streaming helpers name tools for their
    error messages, but schemas are keyed by the name on the wire."""
    out = ""
    for i, ch in enumerate(display):
        if ch.isupper() and i:
            out += "_"
        out += ch.lower()
    return out


def get_mcp_port(fixture_index: int) -> int:
    """Return a unique MCP port for this xdist worker and fixture.

    Under ``--dist=loadgroup`` tests are distributed per-test across workers and
    most game fixtures are function-scoped, so several ScummVM instances of the
    same or different games can be alive at once. Each xdist worker (``gw0``,
    ``gw1``, …) gets its own 100-port band, and each fixture a distinct slot
    within that band, so ports never collide — even when a worker holds an idle
    session fixture (FT/Atlantis) alongside an active function fixture.

    ``fixture_index`` must be a stable integer in 0..99, unique per fixture.
    Outside xdist (no ``PYTEST_XDIST_WORKER``) the worker index is 0.
    """
    worker = os.environ.get("PYTEST_XDIST_WORKER", "gw0")
    digits = "".join(ch for ch in worker if ch.isdigit())
    worker_index = int(digits) if digits else 0
    return MCP_PORT_BASE + worker_index * 100 + fixture_index


class McpClient:
    """Synchronous MCP client over HTTP/1.1 with SSE streaming support."""

    def __init__(
        self,
        host: str = MCP_HOST,
        port: int = MCP_PORT,
        timeout: float = MCP_TIMEOUT_SECS,
    ):
        self.host = host
        self.port = port
        self._url = f"http://{host}:{port}/mcp"
        self._session_id: str | None = None
        self._req_id = 0
        self._schemas: dict[str, Any] | None = None
        self._client = httpx.Client(timeout=httpx.Timeout(timeout))
        # Filled by the fixtures with the launched instance's screenshot folder.
        self.screenshot_path: str | None = None

    def _next_id(self) -> int:
        self._req_id += 1
        return self._req_id

    def _headers(self, extra: dict[str, str] | None = None) -> dict[str, str]:
        h: dict[str, str] = {"Content-Type": "application/json"}
        if self._session_id:
            h["Mcp-Session-Id"] = self._session_id
        if extra:
            h.update(extra)
        return h

    def _extract_result(self, data: dict[str, Any]) -> dict[str, Any]:
        """Pull the inner JSON from a tools/call response."""
        result = data.get("result", {})
        content = result.get("content", [])
        if content and content[0].get("type") == "text":
            return json.loads(content[0]["text"])
        return result

    def _output_schemas(self) -> dict[str, Any]:
        """tool name -> advertised outputSchema, fetched once per session."""
        if self._schemas is None:
            payload = {
                "jsonrpc": "2.0",
                "id": self._next_id(),
                "method": "tools/list",
                "params": {},
            }
            resp = self._client.post(self._url, json=payload, headers=self._headers())
            tools = resp.json().get("result", {}).get("tools", [])
            self._schemas = {
                t["name"]: t["outputSchema"] for t in tools if t.get("outputSchema")
            }
        return self._schemas

    def _check_schema(self, tool: str, result: Any) -> None:
        """Fail the test when a tool result breaks its own advertised schema.

        The schemas are closed (``additionalProperties: false``), so a strict
        MCP client rejects the whole call over one undeclared field — which is
        exactly the failure mode this guards against. Tools that advertise no
        output schema (the debug tools) are left alone.
        """
        if not isinstance(result, dict):
            return
        # A rejected call answers with an error envelope instead of the tool's
        # own result; that is the protocol talking, not the tool, so it is not
        # what the output schema describes.
        if set(result) == {"error"}:
            return
        schema = self._output_schemas().get(tool)
        if schema is None:
            return
        errors = sorted(
            jsonschema.Draft202012Validator(schema).iter_errors(result),
            key=lambda e: list(e.path),
        )
        if errors:
            details = "; ".join(f"at {list(e.path)}: {e.message}" for e in errors)
            raise AssertionError(f"{tool} result violates its outputSchema — {details}")

    def _decode_stream_response(self, resp: httpx.Response, tool: str):
        if resp.status_code >= 400:
            raise RuntimeError(f"Act error: HTTP {resp.status_code}")
        for line in resp.iter_lines():
            if line.startswith("data: "):
                raw = line[6:].strip()
            else:
                raw = line.strip()

            if not raw or raw == ": keepalive":
                continue

            try:
                msg = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    f"Failed to decode JSON (error: {exc}): '{raw}'"
                ) from exc

            if "result" in msg:
                result = self._extract_result(msg)
                # Callers pass a display name for error messages ("ChooseKids");
                # the schema is registered under the wire name.
                self._check_schema(_wire_tool_name(tool), result)
                return result
            elif "error" in msg:
                if "message" in msg["error"]:
                    if "code" in msg["error"]:
                        raise RuntimeError(
                            f"{tool} error: {msg['error']['message']} "
                            f"(code: {msg['error']['code']})"
                        )
                    else:
                        raise RuntimeError(f"{tool} error: {msg['error']['message']}")
                else:
                    raise RuntimeError(f"{tool} error: {msg['error']}")
        raise RuntimeError(f"{tool} stream ended without result")

    def initialize(self) -> None:
        """Initialize MCP session."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "clientInfo": {"name": "test_client", "version": "1.0"},
            },
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        sid = resp.headers.get("Mcp-Session-Id")
        if sid:
            self._session_id = sid
        data = resp.json()
        if "error" in data:
            raise RuntimeError(f"Initialize error: {data['error']}")

    def list_tools(self) -> list[dict[str, Any]]:
        """Return the advertised tools: name, description and both schemas."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/list",
            "params": {},
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        data = resp.json()
        if "error" in data:
            raise RuntimeError(f"tools/list error: {data['error']}")
        return data.get("result", {}).get("tools", [])

    def call_tool_raw(
        self, name: str, arguments: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        """Call a tool and return the whole MCP result envelope.

        ``call_tool`` unwraps the structured result, which is what a test
        usually wants; this keeps ``content`` too, for the tools that answer
        with something other than data (``screenshot``'s image block).
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}},
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        data = resp.json()
        if "error" in data:
            raise RuntimeError(f"{name} error: {data['error']}")
        return data.get("result", {})

    def call_tool(
        self, name: str, arguments: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        """Call any non-streaming tool by name (e.g. a game-specific ``debug``).

        The dedicated wrappers (``state``, ``act``, ...) are the ergonomic path;
        this is the escape hatch for tools without one, such as sword1's
        section-flagged ``debug``.
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}},
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        data = resp.json()

        if "error" in data:
            raise RuntimeError(f"{name} error: {data['error']}")
        result = self._extract_result(data)
        self._check_schema(name, result)
        return result

    def state(self) -> dict[str, Any]:
        """Get current game state (sync call)."""
        return self.call_tool("state")

    def debug(self) -> dict[str, Any]:
        """Get current game state (sync call)."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "debug", "arguments": {}},
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        data = resp.json()

        if "error" in data:
            raise RuntimeError(f"Debug error: {data['error']}")
        return self._extract_result(data)

    def set_talk_speed(self, speed: int = 255) -> dict[str, Any]:
        """Force the text/talk speed at runtime (sync call, gated by mcp_debug).

        ``speed`` is on the 0..255 scale used by the --talkspeed option
        (0 = slowest, 255 = fastest/instant text). Needed for titles whose boot
        script overrides the configured talkspeed (e.g. the Fate of Atlantis
        demo), where the startup setting never takes.
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {
                "name": "set_talk_speed",
                "arguments": {"speed": speed},
            },
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        data = resp.json()
        if "error" in data:
            raise RuntimeError(f"SetTalkSpeed error: {data['error']}")
        return self._extract_result(data)

    def skip(self) -> dict[str, Any]:
        """Skip (equivalent to Escape)."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "skip", "arguments": {}},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="Skip")

    def act(
        self,
        verb: str,
        target1: str | int | None = None,
        target2: str | int | None = None,
        x: int | None = None,
        y: int | None = None,
    ) -> dict[str, Any]:
        """Execute a verb on a target, or on a point when the game takes one."""
        arguments: dict[str, Any] = {"verb": verb}
        if target1 is not None:
            arguments["target1"] = target1
        if target2 is not None:
            arguments["target2"] = target2
        if x is not None:
            arguments["x"] = x
        if y is not None:
            arguments["y"] = y

        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "act", "arguments": arguments},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="Act")

    def answer(self, choice_id: int) -> dict[str, Any]:
        """Select a dialog choice (streaming call)."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "answer", "arguments": {"id": choice_id}},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="Answer")

    def walk(self, x: int, y: int) -> dict[str, Any]:
        """Select position to walk to (streaming call)."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "walk", "arguments": {"x": x, "y": y}},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="Walk")

    def choose_kids(self, kids: list[str], skip_intro: bool = True) -> dict[str, Any]:
        """Pick the three heroes on the Maniac Mansion title screen (streaming).

        Only valid while state()['kid_selection_pending'] is set. Name the two
        kids joining Dave (naming Dave as the third is accepted); the call
        presses START and, unless skip_intro is False, escapes through the intro
        so it returns with the player in control. The team comes back in
        result['kids'].
        """
        arguments: dict[str, Any] = {"kids": kids}
        if not skip_intro:
            arguments["skip_intro"] = False
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "choose_kids", "arguments": arguments},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="ChooseKids")

    def switch_character(self, name: str) -> dict[str, Any]:
        """Switch the controlled kid in Maniac Mansion (streaming call).

        Valid names are listed in state()['available_characters']; the active
        one is state()['controlling'].
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "switch_character", "arguments": {"name": name}},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="SwitchCharacter")

    def dial(self, number: str) -> dict[str, Any]:
        """Dial a number on the Maniac Mansion phone dial pad (streaming call).

        Only valid while the dial pad is on screen — use the phone first via
        act('use', 'phone') and wait for the dial pad room. Keys are digits
        0-9 plus '*' and '#'.
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "dial", "arguments": {"number": number}},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="Dial")

    def ride_bike(self) -> dict[str, Any]:
        """Play the Full Throttle motorcycle minigame (streaming call).

        Only valid in Full Throttle once Ben has his bike keys and is at the bike
        (bar front, room 6). Mounts the bike, rides onto the highway, and
        auto-plays the Rottwheeler fight (steering + punching) until the section
        resolves at the mechanic's shack. Takes no arguments. Blocks until done.
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "ride_bike", "arguments": {}},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="RideBike")

    def play_note(self, note) -> dict[str, Any]:
        """Play one or more notes on the Loom distaff (streaming call).

        Pass a single note ('c'..'C') for a one-note play, or a list/tuple
        like ['e', 'c', 'e', 'd'] to send a full sequence in one call. The
        engine plays them one after another, throttled so each note finishes
        before the next is pressed.
        """
        if isinstance(note, (list, tuple)):
            arguments = {"notes": list(note)}
        else:
            arguments = {"note": note}
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "play_note", "arguments": arguments},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            return self._decode_stream_response(resp=resp, tool="PlayNote")

    def call_capturing(
        self, name: str, arguments: dict
    ) -> tuple[list[str], list[dict], dict | None]:
        """Invoke any MCP tool, capturing every notification.

        Returns (notes, messages, result):
          - notes:    text of each notification with type=='note' (Loom note plays)
          - messages: every other notification (dialog, system, etc.)
          - result:   inner JSON of the final tool result, or None on error
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        notes: list[str] = []
        messages: list[dict] = []
        result: dict | None = None
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            for line in resp.iter_lines():
                if not line.startswith("data: "):
                    continue
                raw = line[6:].strip()
                if not raw:
                    continue
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    continue
                if "params" in msg:
                    p = msg["params"]
                    if p.get("type") == "note":
                        text = p.get("text") or ""
                        if text:
                            notes.append(text)
                    else:
                        messages.append(p)
                elif "result" in msg:
                    result = self._extract_result(msg)
                    break
                elif "error" in msg:
                    return notes, messages, None
        return notes, messages, result

    def close(self) -> None:
        """Close the client."""
        self._client.close()


def wait_for_mcp(
    host: str = MCP_HOST,
    port: int = MCP_PORT,
    connect_timeout: float = MCP_CONNECT_TIMEOUT_SECS,
    timeout: float = MCP_TIMEOUT_SECS,
) -> McpClient:
    """Poll until MCP server is ready, then return initialized client."""
    start = time.time()
    last_error = None
    while time.time() - start < connect_timeout:
        try:
            client = McpClient(host=host, port=port, timeout=timeout)
            client.initialize()
            return client
        except Exception as e:
            last_error = e
            time.sleep(0.5)
    raise TimeoutError(
        f"MCP server at {host}:{port} did not respond "
        f"within {connect_timeout}s: {last_error}"
    )
