"""A vendored, self-contained MCP client + ScummVM launcher.

This is a trimmed copy of the proven ``test/mcp/utils.py`` client, kept inside
``scummvm_bench`` so the package has no dependency on the test tree. It speaks
ScummVM's Streamable-HTTP MCP wire protocol (JSON-RPC 2.0 over HTTP, with SSE for
streaming tools) and exposes one generic :meth:`McpClient.call_tool` that
consolidates streaming responses into a single result dict.
"""

import json
import os
import subprocess
import time

import httpx

DEFAULT_HOST = "127.0.0.1"
DEFAULT_TIMEOUT_S = 30.0
CONNECT_TIMEOUT_S = 30.0

# JSON-RPC error codes that mean "the request was bad" (vs. a server fault).
INVALID_REQUEST_CODES = frozenset({-32600, -32601, -32602, -32700})


class McpError(RuntimeError):
    """A JSON-RPC error returned by the MCP server."""

    def __init__(self, message: str, code: int | None = None) -> None:
        super().__init__(message)
        self.code = code


class McpDecodeError(RuntimeError):
    """The server returned a body that could not be decoded as expected."""


class McpClient:
    """Synchronous MCP client over HTTP/1.1 with SSE streaming support."""

    def __init__(
        self,
        host: str = DEFAULT_HOST,
        port: int = 23456,
        timeout: float = DEFAULT_TIMEOUT_S,
    ) -> None:
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
        headers = {"Content-Type": "application/json"}
        if self._session_id:
            headers["Mcp-Session-Id"] = self._session_id
        if extra:
            headers.update(extra)
        return headers

    @staticmethod
    def _extract_result(data: dict[str, object]) -> dict[str, object]:
        result = data.get("result")
        if not isinstance(result, dict):
            return {}
        content = result.get("content")
        if isinstance(content, list) and content:
            first = content[0]
            if isinstance(first, dict) and first.get("type") == "text":
                text = first.get("text")
                if isinstance(text, str):
                    decoded = json.loads(text)
                    if isinstance(decoded, dict):
                        return {str(key): val for key, val in decoded.items()}
                    return {}
        return {str(key): val for key, val in result.items()}

    def _decode_stream(self, resp: httpx.Response, tool: str) -> dict[str, object]:
        if resp.status_code >= 400:
            raise McpError(f"{tool}: HTTP {resp.status_code}")
        notes: list[str] = []
        for line in resp.iter_lines():
            raw = line[6:].strip() if line.startswith("data: ") else line.strip()
            if not raw or raw == ": keepalive":
                continue
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise McpDecodeError(f"{tool}: bad SSE JSON: {raw!r}") from exc
            if not isinstance(msg, dict):
                continue
            # Loom plays distaff notes as streamed notifications (type "note");
            # collect them so callers can learn a draft and replay it.
            params = msg.get("params")
            if isinstance(params, dict) and params.get("type") == "note":
                text = params.get("text")
                if isinstance(text, str) and text:
                    notes.append(text)
            if "result" in msg:
                result = self._extract_result(msg)
                if notes and isinstance(result, dict) and "notes" not in result:
                    result = {**result, "notes": notes}
                return result
            if "error" in msg:
                raise self._error_from(msg["error"], tool)
        raise McpDecodeError(f"{tool}: stream ended without result")

    @staticmethod
    def _error_from(error: object, tool: str) -> McpError:
        if isinstance(error, dict):
            message = error.get("message")
            code = error.get("code")
            return McpError(
                f"{tool}: {message}" if message else f"{tool}: {error}",
                code if isinstance(code, int) else None,
            )
        return McpError(f"{tool}: {error}")

    def initialize(self) -> None:
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "clientInfo": {"name": "scummvm_bench", "version": "1.0"},
            },
        }
        resp = self._client.post(self._url, json=payload, headers=self._headers())
        session_id = resp.headers.get("Mcp-Session-Id")
        if session_id:
            self._session_id = session_id
        data = resp.json()
        if isinstance(data, dict) and "error" in data:
            raise self._error_from(data["error"], "initialize")

    def call_tool(self, name: str, arguments: dict[str, object]) -> dict[str, object]:
        """Invoke any MCP tool, returning the consolidated result dict.

        Handles both synchronous (``application/json``) and streaming
        (``text/event-stream``) responses transparently.
        """
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        }
        headers = self._headers({"Accept": "application/json, text/event-stream"})
        with self._client.stream(
            "POST", self._url, json=payload, headers=headers
        ) as resp:
            ctype = resp.headers.get("content-type", "")
            if "text/event-stream" in ctype:
                return self._decode_stream(resp, name)
            body = b"".join(resp.iter_bytes())
            if resp.status_code >= 400:
                raise McpError(f"{name}: HTTP {resp.status_code}")
            try:
                data = json.loads(body)
            except json.JSONDecodeError as exc:
                raise McpDecodeError(f"{name}: bad JSON body") from exc
            if not isinstance(data, dict):
                raise McpDecodeError(f"{name}: non-object response")
            if "error" in data:
                raise self._error_from(data["error"], name)
            return self._extract_result(data)

    def state(self) -> dict[str, object]:
        return self.call_tool("state", {})

    def close(self) -> None:
        self._client.close()


def wait_for_mcp(
    host: str,
    port: int,
    connect_timeout: float = CONNECT_TIMEOUT_S,
    timeout: float = DEFAULT_TIMEOUT_S,
) -> McpClient:
    """Poll until the MCP server answers ``initialize``, then return the client."""
    start = time.time()
    last_error: Exception | None = None
    while time.time() - start < connect_timeout:
        try:
            client = McpClient(host=host, port=port, timeout=timeout)
            client.initialize()
            return client
        except Exception as exc:  # noqa: BLE001 - server not up yet
            last_error = exc
            time.sleep(0.5)
    raise TimeoutError(
        f"MCP server at {host}:{port} did not respond within {connect_timeout}s: "
        f"{last_error}"
    )


def launch_scummvm(
    scummvm_binary: str,
    ini_path: str,
    game_id: str,
    save_slot: int | None,
    save_path: str | None,
    stdout_path: str,
    stderr_path: str,
    talkspeed: int = 1200,
) -> subprocess.Popen[bytes]:
    """Launch ScummVM headlessly with MCP enabled for ``game_id``."""
    args = [scummvm_binary, "-c", ini_path]
    if save_slot is not None:
        args.append(f"--save-slot={save_slot}")
    if save_path is not None:
        args.append(f"--savepath={save_path}")
    args.append(f"--talkspeed={talkspeed}")
    args.append(game_id)

    env = os.environ.copy()
    env["SDL_AUDIODRIVER"] = "dummy"

    stdout_file = open(stdout_path, "w")
    stderr_file = open(stderr_path, "w")
    stdout_file.write(f"Command: {' '.join(args)}\n{'=' * 72}\n")
    stdout_file.flush()

    proc = subprocess.Popen(args, env=env, stdout=stdout_file, stderr=stderr_file)
    # The child holds its own dup'd fds; the parent copies can be closed now.
    stdout_file.close()
    stderr_file.close()
    return proc
