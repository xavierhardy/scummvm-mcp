#!/usr/bin/env python3
"""
MCP integration test utilities: client, ScummVM launcher, shared helpers.
"""
import json
import os
import subprocess
import tempfile
import time
from typing import Optional, Dict, Any

import httpx


class McpClient:
    """Synchronous MCP client over HTTP/1.1 with SSE streaming support."""

    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self._url = f"http://{host}:{port}/mcp"
        self._session_id: Optional[str] = None
        self._req_id = 0
        self._client = httpx.Client(timeout=httpx.Timeout(30.0))

    def _next_id(self) -> int:
        self._req_id += 1
        return self._req_id

    def _headers(self, extra: Optional[Dict[str, str]] = None) -> Dict[str, str]:
        h: Dict[str, str] = {"Content-Type": "application/json"}
        if self._session_id:
            h["Mcp-Session-Id"] = self._session_id
        if extra:
            h.update(extra)
        return h

    def _extract_result(self, data: Dict[str, Any]) -> Dict[str, Any]:
        """Pull the inner JSON from a tools/call response."""
        result = data.get("result", {})
        content = result.get("content", [])
        if content and content[0].get("type") == "text":
            return json.loads(content[0]["text"])
        return result

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

    def state(self) -> Dict[str, Any]:
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
        self, verb: str, target1: Optional[str] = None, target2: Optional[str] = None
    ) -> Dict[str, Any]:
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
            if resp.status_code >= 400:
                raise RuntimeError(f"Act error: HTTP {resp.status_code}")
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
                if "result" in msg:
                    return self._extract_result(msg)
                elif "error" in msg:
                    raise RuntimeError(f"Act error: {msg['error']}")
        raise RuntimeError("Act stream ended without result")

    def answer(self, choice_id: int) -> Dict[str, Any]:
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
            if resp.status_code >= 400:
                raise RuntimeError(f"Answer error: HTTP {resp.status_code}")
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
                if "result" in msg:
                    return self._extract_result(msg)
                elif "error" in msg:
                    raise RuntimeError(f"Answer error: {msg['error']}")
        raise RuntimeError("Answer stream ended without result")

    def close(self) -> None:
        """Close the client."""
        self._client.close()


def wait_for_mcp(
    host: str = "127.0.0.1", port: int = 23456, timeout: float = 30.0
) -> McpClient:
    """Poll until MCP server is ready, then return initialized client."""
    start = time.time()
    last_error = None
    while time.time() - start < timeout:
        try:
            client = McpClient(host, port)
            client.initialize()
            return client
        except Exception as e:
            last_error = e
            time.sleep(0.5)
    raise TimeoutError(
        f"MCP server at {host}:{port} did not respond within {timeout}s: {last_error}"
    )


def launch_scummvm(
    game_id: str, game_path: str, port: int = 23456, scummvm_binary: str = "./scummvm"
) -> subprocess.Popen:
    """Launch ScummVM headlessly with MCP enabled for the given game."""
    # Create temporary scummvm.ini
    tmpdir = tempfile.mkdtemp(prefix=f"scummvm_{game_id}_")
    ini_path = os.path.join(tmpdir, "scummvm.ini")
    ini_content = f"""[scummvm]
save_path=/tmp

[{game_id}]
path={game_path}
mcp=true
mcp_port={port}
"""
    with open(ini_path, "w") as f:
        f.write(ini_content)

    # Launch with no video/audio
    env = os.environ.copy()
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"

    proc = subprocess.Popen(
        [scummvm_binary, "-c", ini_path, "--no-gui", game_id],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return proc


GAME_PATHS = {
    "monkey": os.environ.get("MONKEY_DEMO_PATH", "/home/pi/games/MonkeyDemo"),
    "maniac": os.environ.get("MANIAC_C64_PATH", "/home/pi/games/ManiacC64"),
    "atlantis": os.environ.get("ATLANTIS_DEMO_PATH", "/home/pi/games/Indy4Demo"),
}


def require_game_path(game_id: str) -> None:
    """Skip test if game files are not found."""
    import pytest

    path = GAME_PATHS.get(game_id)
    if not path or not os.path.isdir(path):
        pytest.skip(f"Game files not found at {path}")
