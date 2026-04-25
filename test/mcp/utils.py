#!/usr/bin/env python3
"""
MCP integration test utilities: client, ScummVM launcher, shared helpers.
"""

import json
import os
import subprocess
import tempfile
import time

from typing import Any

import httpx


MCP_HOST = "127.0.0.1"
MCP_PORT = 23456
MCP_TIMEOUT_SECS = 10.0
MCP_CONNECT_TIMEOUT_SECS = 30.0
MCP_TOOLS = ("state", "act", "answer", "walk")


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
        self._client = httpx.Client(timeout=httpx.Timeout(timeout))

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
                raise RuntimeError(f"Failed to decode JSON (error: {exc}): '{raw}'")

            if "result" in msg:
                return self._extract_result(msg)
            elif "error" in msg:
                if "message" in msg["error"]:
                    if "code" in msg["error"]:
                        raise RuntimeError(
                            f"{tool} error: {msg['error']['message']} (code: {msg['error']['code']})"
                        )
                    else:
                        raise RuntimeError(f"{tool} error: {msg['error']['message']}")
                else:
                    raise RuntimeError(f"{tool} error: {msg['error']}")
        raise RuntimeError("{tool stream ended without result")

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

    def state(self) -> dict[str, Any]:
        """Get current game state (sync call)."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": "state", "arguments": {}},
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        data = resp.json()

        if "error" in data:
            raise RuntimeError(f"State error: {data['error']}")
        return self._extract_result(data)

    def act(
        self,
        verb: str,
        target1: str | int | None = None,
        target2: str | int | None = None,
    ) -> dict[str, Any]:
        """Execute a verb on a target (streaming call)."""
        arguments = {"verb": verb}
        if target1 is not None:
            arguments["target1"] = target1
        if target2 is not None:
            arguments["target2"] = target2

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
        f"MCP server at {host}:{port} did not respond within {connect_timeout}s: {last_error}"
    )


def launch_scummvm(
    game_id: str, game_path: str, port: int = 23456, scummvm_binary: str = "./scummvm"
) -> subprocess.Popen:
    """Launch ScummVM headlessly with MCP enabled for the given game."""
    # Create temporary scummvm.ini
    with open(os.path.join("ini_files", f"scummvm_{game_id}.ini")) as ini_file:
        content = ini_file.read() % {"game_path": game_path, "mcp_port": port}

    tmpdir = tempfile.mkdtemp(prefix=f"scummvm_{game_id}_")
    ini_path = os.path.join(tmpdir, "scummvm.ini")
    save_path = os.path.join(os.path.dirname(__file__), f"save_slots/{game_id}")

    with open(ini_path, "w") as f:
        f.write(content)

    # Launch with no video/audio
    env = os.environ.copy()
    # env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"

    if game_id == "atlantis":
        args = [
            scummvm_binary,
            "-c",
            ini_path,
            game_id,
        ]
    else:
        args = [
            scummvm_binary,
            "-c",
            ini_path,
            "--save-slot=1",
            f"--savepath={save_path}",
            "--talkspeed=1200",
            game_id,
        ]

    proc = subprocess.Popen(
        # [scummvm_binary, "-c", ini_path, "--no-gui", game_id],
        args,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return proc


GAME_PATHS = {
    "monkey-ega-demo": os.environ.get("MONKEY_DEMO_PATH", "/home/pi/games/MonkeyDemo"),
    "maniac-c64": os.environ.get("MANIAC_C64_PATH", "/home/pi/games/ManiacC64"),
    "atlantis": os.environ.get("ATLANTIS_DEMO_PATH", "/home/pi/games/Indy4Demo"),
}


def require_game_path(game_id: str) -> None:
    """Skip test if game files are not found."""
    import pytest

    path = GAME_PATHS.get(game_id)
    if not path or not os.path.isdir(path):
        pytest.skip(f"Game files not found at {path}")
