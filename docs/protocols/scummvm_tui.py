#!/usr/bin/env python3
"""
ScummVM MCP TUI — human-friendly terminal interface for ScummVM's MCP server.

Requirements:
    uv venv ~/scummvm-tui-venv
    uv pip install --python ~/scummvm-tui-venv/bin/python textual httpx

Usage:
    ~/scummvm-tui-venv/bin/python scummvm_tui.py [--host 127.0.0.1] [--port 23456]

Commands:
    look [at] <object>        | pick [up] <object>      | talk [to] <object>
    use <object> [on <obj2>]  | open/push/pull <object> | give <obj> [to <obj2>]
    walk to <object>          | walk <x> <y>            | 1–9 / answer <n>
    state / Enter             → refresh game state
"""

from __future__ import annotations

import argparse
import json
import time
from datetime import datetime
from typing import AsyncGenerator

import httpx
from rich.text import Text
from textual import work
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.message import Message
from textual.reactive import reactive
from textual.screen import ModalScreen, Screen
from textual.widgets import (
    Button,
    Footer,
    Header,
    Input,
    Label,
    ListItem,
    ListView,
    RichLog,
    Static,
)

# ── Constants ──────────────────────────────────────────────────────────────────

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 23456
POLL_INTERVAL = 3.0
POLL_INTERVAL_QUESTION = 1.5
REQUEST_TIMEOUT = 5.0
STREAM_TIMEOUT = 60.0

# ── Exceptions ─────────────────────────────────────────────────────────────────

class McpConnectionError(Exception):
    pass

class McpError(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code

class McpSessionExpired(McpConnectionError):
    pass

# ── Textual Messages ───────────────────────────────────────────────────────────

class SseEvent(Message):
    def __init__(self, text: str, actor: str) -> None:
        super().__init__()
        self.text = text
        self.actor = actor

class ActionComplete(Message):
    def __init__(self, result: dict) -> None:
        super().__init__()
        self.result = result

class ActionFailed(Message):
    def __init__(self, error: str) -> None:
        super().__init__()
        self.error = error

# ── McpClient ──────────────────────────────────────────────────────────────────

class McpClient:
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self._url = f"http://{host}:{port}/mcp"
        self._session_id: str | None = None
        self._req_id = 0
        self._client = httpx.AsyncClient(timeout=httpx.Timeout(REQUEST_TIMEOUT))

    def _next_id(self) -> int:
        self._req_id += 1
        return self._req_id

    def _headers(self, extra: dict | None = None) -> dict:
        h: dict = {"Content-Type": "application/json"}
        if self._session_id:
            h["Mcp-Session-Id"] = self._session_id
        if extra:
            h.update(extra)
        return h

    def _extract_result(self, data: dict) -> dict:
        """Pull the inner JSON from a tools/call response."""
        result = data.get("result", {})
        content = result.get("content", [])
        if content and content[0].get("type") == "text":
            return json.loads(content[0]["text"])
        return result

    async def initialize(self) -> None:
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "clientInfo": {"name": "scummvm_tui", "version": "1.0"},
            },
        }
        try:
            resp = await self._client.post(self._url, json=payload, headers=self._headers())
        except (httpx.ConnectError, httpx.TimeoutException) as e:
            raise McpConnectionError(str(e)) from e
        sid = resp.headers.get("Mcp-Session-Id")
        if sid:
            self._session_id = sid
        data = resp.json()
        if "error" in data:
            raise McpError(data["error"]["code"], data["error"]["message"])

    async def call_tool(self, name: str, arguments: dict) -> dict:
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        }
        try:
            resp = await self._client.post(self._url, json=payload, headers=self._headers())
        except (httpx.ConnectError, httpx.TimeoutException) as e:
            raise McpConnectionError(str(e)) from e
        if resp.status_code in (401, 403, 404):
            raise McpSessionExpired(f"HTTP {resp.status_code}")
        data = resp.json()
        if "error" in data:
            raise McpError(data["error"]["code"], data["error"]["message"])
        return self._extract_result(data)

    async def stream_tool(
        self, name: str, arguments: dict
    ) -> AsyncGenerator[tuple[str, dict], None]:
        """Yields (kind, payload) tuples: kind is 'event', 'result', or 'error'."""
        payload = {
            "jsonrpc": "2.0",
            "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        }
        headers = self._headers({"Accept": "text/event-stream"})
        try:
            async with self._client.stream(
                "POST",
                self._url,
                json=payload,
                headers=headers,
                timeout=httpx.Timeout(STREAM_TIMEOUT),
            ) as resp:
                if resp.status_code in (401, 403, 404):
                    raise McpSessionExpired(f"HTTP {resp.status_code}")
                async for line in resp.aiter_lines():
                    if not line.startswith("data: "):
                        continue
                    raw = line[6:].strip()
                    if not raw:
                        continue
                    try:
                        msg = json.loads(raw)
                    except json.JSONDecodeError:
                        continue
                    if "method" in msg:
                        params = msg.get("params", {})
                        yield "event", params
                    elif "result" in msg:
                        yield "result", self._extract_result(msg)
                        return
                    elif "error" in msg:
                        yield "error", msg["error"]
                        return
        except (httpx.ConnectError, httpx.TimeoutException) as e:
            raise McpConnectionError(str(e)) from e

    async def aclose(self) -> None:
        await self._client.aclose()

# ── Command Parser ─────────────────────────────────────────────────────────────

def _fuzzy(name: str, candidates: list[str]) -> str | None:
    nl = name.lower()
    for c in candidates:
        if c.lower() == nl:
            return c
    for c in candidates:
        if c.lower().startswith(nl):
            return c
    for c in candidates:
        if nl in c.lower():
            return c
    return None


def _resolve(raw: str, state: dict | None) -> str:
    if not state or not raw:
        return raw
    pool = [o["name"] for o in state.get("objects", [])] + list(state.get("inventory", []))
    return _fuzzy(raw, pool) or raw


def parse_command(text: str, state: dict | None = None) -> tuple[str, dict] | None:
    """
    Returns (tool, arguments) or None (meaning: just refresh state).
    tool is one of: 'act', 'answer', 'walk'
    """
    text = text.strip()
    if not text or text.lower() == "state":
        return None

    tl = text.lower()
    parts = text.split()

    # answer: bare digit or "answer N"
    if tl.isdigit() and len(tl) == 1:
        return "answer", {"id": int(tl)}
    if tl.startswith("answer "):
        rest = tl[7:].strip()
        if rest.isdigit():
            return "answer", {"id": int(rest)}

    # walk N N  (pixel coords)
    if parts[0].lower() == "walk" and len(parts) == 3:
        try:
            return "walk", {"x": int(parts[1]), "y": int(parts[2])}
        except ValueError:
            pass

    # walk to <object>
    if tl.startswith("walk to "):
        t = _resolve(text[8:].strip(), state)
        return "act", {"verb": "walk_to", "target1": t}

    # use X on Y  /  use X
    if tl.startswith("use "):
        rest = text[4:].strip()
        lower_rest = rest.lower()
        if " on " in lower_rest:
            idx = lower_rest.index(" on ")
            t1 = _resolve(rest[:idx].strip(), state)
            t2 = _resolve(rest[idx + 4:].strip(), state)
            return "act", {"verb": "use", "target1": t1, "target2": t2}
        return "act", {"verb": "use", "target1": _resolve(rest, state)}

    # give X to Y  /  give X
    if tl.startswith("give "):
        rest = text[5:].strip()
        lower_rest = rest.lower()
        if " to " in lower_rest:
            idx = lower_rest.index(" to ")
            t1 = _resolve(rest[:idx].strip(), state)
            t2 = _resolve(rest[idx + 4:].strip(), state)
            return "act", {"verb": "give", "target1": t1, "target2": t2}
        return "act", {"verb": "give", "target1": _resolve(rest, state)}

    # look [at] X
    if tl.startswith("look "):
        rest = text[5:].strip()
        if rest.lower().startswith("at "):
            rest = rest[3:].strip()
        return "act", {"verb": "look_at", "target1": _resolve(rest, state)}

    # pick [up] X
    if tl.startswith("pick "):
        rest = text[5:].strip()
        if rest.lower().startswith("up "):
            rest = rest[3:].strip()
        return "act", {"verb": "pick_up", "target1": _resolve(rest, state)}

    # talk [to] X
    if tl.startswith("talk "):
        rest = text[5:].strip()
        if rest.lower().startswith("to "):
            rest = rest[3:].strip()
        return "act", {"verb": "talk_to", "target1": _resolve(rest, state)}

    # single-word verbs: open, close, push, pull, walk
    _VERB_MAP: dict[str, str] = {
        "open": "open", "close": "close",
        "push": "push", "pull": "pull",
        "walk": "walk_to", "go": "walk_to", "goto": "walk_to",
        "get": "pick_up", "take": "pick_up",
        "l": "look_at", "look": "look_at",
        "talk": "talk_to", "speak": "talk_to",
    }
    if parts[0].lower() in _VERB_MAP and len(parts) >= 2:
        t = _resolve(" ".join(parts[1:]), state)
        return "act", {"verb": _VERB_MAP[parts[0].lower()], "target1": t}

    # fallback: treat the whole thing as "look at X"
    return "act", {"verb": "look_at", "target1": _resolve(text, state)}

# ── MCP Log Panel ──────────────────────────────────────────────────────────────

class McpLogPanel(VerticalScroll):
    """Right panel: prettified MCP request/response traffic."""

    DEFAULT_CSS = """
    McpLogPanel {
        width: 40%;
        border: solid $accent;
        padding: 0 1;
    }
    McpLogPanel .panel-title {
        text-style: bold;
        background: $accent-darken-1;
        color: $text;
        padding: 0 1;
        margin-bottom: 1;
    }
    """

    def compose(self) -> ComposeResult:
        yield Label("MCP Traffic", classes="panel-title")
        yield RichLog(id="mcp-log", highlight=False, markup=False, wrap=True)

    def _log(self) -> RichLog:
        return self.query_one("#mcp-log", RichLog)

    def append_request(self, tool: str, params: dict, ts: str) -> None:
        log = self._log()
        sep = Text("─── REQUEST " + ts + " " + "─" * 10, style="dim")
        log.write(sep)
        header = Text()
        header.append("  POST ", style="cyan bold")
        header.append(tool, style="bold white")
        log.write(header)
        for k, v in params.items():
            row = Text()
            row.append(f"    {k}: ", style="dim")
            row.append(str(v), style="yellow" if k == "verb" else "green")
            log.write(row)

    def append_sse_event(self, text: str, actor: str) -> None:
        t = Text()
        t.append("  ↳ SSE ", style="dim")
        if actor:
            t.append(actor, style="magenta bold")
            t.append(": ", style="dim")
        t.append(text)
        self._log().write(t)

    def append_result(self, result: dict) -> None:
        log = self._log()
        t = Text()
        t.append("✓ RESULT ", style="green bold")
        parts: list[str] = []
        if "room_name" in result:
            parts.append(result["room_name"])
        elif "room" in result:
            parts.append(f"room {result['room']}")
        if "objects" in result:
            parts.append(f"{len(result['objects'])} objects")
        if result.get("inventory_added"):
            added = result["inventory_added"]
            parts.append(f"+{added}" if isinstance(added, str) else f"+{len(added)} items")
        t.append("  ".join(parts), style="dim")
        log.write(t)
        log.write(Text("─" * 32, style="dim"))

    def append_state_result(self, result: dict) -> None:
        t = Text()
        t.append("  state ", style="green")
        room_name = result.get("room_name") or f"room {result.get('room', '?')}"
        t.append(room_name, style="dim")
        n_o = len(result.get("objects", []))
        n_i = len(result.get("inventory", []))
        n_m = len(result.get("messages", []))
        t.append(f"  {n_o}obj {n_i}inv {n_m}msg", style="dim")
        self._log().write(t)

    def append_error(self, message: str) -> None:
        log = self._log()
        t = Text()
        t.append("✗ ERROR ", style="red bold")
        t.append(message, style="red")
        log.write(t)
        log.write(Text("─" * 32, style="dim"))

# ── Game State Panel ───────────────────────────────────────────────────────────

class GameStatePanel(Vertical):
    """Left panel: room, objects, inventory, messages, dialog choices."""

    DEFAULT_CSS = """
    GameStatePanel {
        height: auto;
        padding: 0 1;
    }
    GameStatePanel .panel-title {
        text-style: bold;
        background: $primary-darken-1;
        color: $text;
        padding: 0 1;
        margin-bottom: 1;
    }
    GameStatePanel .section-label {
        text-style: bold dim;
        margin-top: 1;
    }
    #room-info { margin-bottom: 1; }
    #object-list { max-height: 10; border: solid $surface-lighten-2; }
    #inventory-list { max-height: 6; border: solid $surface-lighten-2; }
    #message-log { height: 8; border: solid $surface-lighten-2; margin-top: 1; }
    #question-panel {
        margin-top: 1;
        border: solid $warning;
        background: $warning 15%;
        padding: 0 1;
    }
    #question-panel.hidden { display: none; }
    """

    def compose(self) -> ComposeResult:
        yield Label("Game State", classes="panel-title")
        yield Static("Connecting…", id="room-info")
        yield Label("Objects", classes="section-label")
        yield ListView(id="object-list")
        yield Label("Inventory", classes="section-label")
        yield ListView(id="inventory-list")
        yield Label("Messages", classes="section-label")
        yield RichLog(id="message-log", highlight=False, markup=False, wrap=True)
        yield Static("", id="question-panel", classes="hidden")

    # ── update helpers ───────────────────────────────────────────────────────

    def update_room_info(self, state: dict) -> None:
        room_name = state.get("room_name") or f"Room {state.get('room', '?')}"
        pos = state.get("position", {})
        x, y = pos.get("x", "?"), pos.get("y", "?")
        verbs = state.get("verbs", [])
        t = Text()
        t.append(room_name, style="bold cyan")
        t.append(f"  ({x}, {y})", style="dim")
        if verbs:
            t.append("\nVerbs: ", style="dim")
            t.append(", ".join(verbs), style="yellow")
        self.query_one("#room-info", Static).update(t)

    def update_objects(self, objects: list) -> None:
        lv = self.query_one("#object-list", ListView)
        lv.clear()
        for obj in objects:
            name = obj.get("name", "?")
            verbs = obj.get("compatible_verbs", [])
            t = Text()
            t.append("• ", style="dim")
            t.append(name, style="bold")
            if verbs:
                t.append("  ", style="dim")
                t.append(", ".join(verbs), style="dim yellow")
            if obj.get("pathway"):
                t.append(" ↗", style="dim cyan")
            lv.append(ListItem(Label(t)))

    def update_inventory(self, inventory: list) -> None:
        lv = self.query_one("#inventory-list", ListView)
        lv.clear()
        for item in inventory:
            t = Text()
            t.append("• ", style="dim")
            t.append(str(item), style="bold green")
            lv.append(ListItem(Label(t)))

    def append_messages(self, messages: list, seen: set) -> None:
        log = self.query_one("#message-log", RichLog)
        for msg in messages:
            key = (msg.get("text", ""), msg.get("actor", ""))
            if key in seen:
                continue
            seen.add(key)
            t = Text()
            actor = msg.get("actor", "")
            if actor:
                t.append(actor, style="bold magenta")
                t.append(": ", style="dim")
            t.append(msg.get("text", ""))
            log.write(t)

    def append_live_message(self, text: str, actor: str) -> None:
        log = self.query_one("#message-log", RichLog)
        t = Text()
        if actor:
            t.append(actor, style="bold magenta")
            t.append(": ", style="dim")
        t.append(text)
        log.write(t)

    def update_question(self, question: dict | None) -> None:
        panel = self.query_one("#question-panel", Static)
        if not question:
            panel.update("")
            panel.add_class("hidden")
            return
        choices = question.get("choices", [])
        t = Text()
        t.append("── Choose ──\n", style="bold yellow")
        for c in choices:
            t.append(f"  {c['id']}. ", style="bold cyan")
            t.append(c["label"], style="white")
            t.append("\n")
        panel.update(t)
        panel.remove_class("hidden")

# ── Connection Error Modal ─────────────────────────────────────────────────────

class ConnectionErrorScreen(ModalScreen[bool]):
    BINDINGS = [Binding("escape", "dismiss_false", "Close")]

    DEFAULT_CSS = """
    ConnectionErrorScreen {
        align: center middle;
    }
    #error-dialog {
        width: 64;
        height: auto;
        border: solid $error;
        background: $surface;
        padding: 1 2;
    }
    #error-title {
        text-style: bold;
        color: $error;
        margin-bottom: 1;
    }
    #error-hint {
        color: $text-muted;
        margin-top: 1;
        margin-bottom: 1;
        text-style: italic;
    }
    #error-buttons { height: 3; }
    #error-buttons Button { margin-right: 1; }
    """

    def __init__(self, host: str, port: int, message: str) -> None:
        super().__init__()
        self._host = host
        self._port = port
        self._message = message

    def compose(self) -> ComposeResult:
        with Vertical(id="error-dialog"):
            yield Label("Cannot connect to ScummVM MCP", id="error-title")
            yield Static(f"  {self._host}:{self._port}\n\n  {self._message}")
            yield Static(
                "Make sure ScummVM is running with mcp=true in scummvm.ini",
                id="error-hint",
            )
            with Horizontal(id="error-buttons"):
                yield Button("Retry", id="retry-btn", variant="primary")
                yield Button("Quit", id="quit-btn", variant="error")

    def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "retry-btn":
            self.dismiss(True)
        else:
            self.dismiss(False)

    def action_dismiss_false(self) -> None:
        self.dismiss(False)

# ── Main Screen ────────────────────────────────────────────────────────────────

class MainScreen(Screen):
    BINDINGS = [
        Binding("ctrl+r", "refresh", "Refresh"),
        Binding("escape", "clear_input", "Clear input"),
    ]

    DEFAULT_CSS = """
    MainScreen {
        layers: base overlay;
    }
    #main-layout {
        height: 1fr;
    }
    #state-scroll {
        width: 60%;
        border: solid $primary;
    }
    #bottom-bar {
        height: 3;
        dock: bottom;
        background: $surface;
        border-top: solid $primary-darken-2;
    }
    #status {
        width: 12;
        content-align: center middle;
        background: $success-darken-1;
        color: $text;
        text-style: bold;
    }
    #status.streaming {
        background: $warning-darken-1;
    }
    #status.error {
        background: $error-darken-1;
    }
    #cmd-input { width: 1fr; }
    """

    is_streaming: reactive[bool] = reactive(False)

    def __init__(self, mcp: McpClient) -> None:
        super().__init__()
        self.mcp = mcp
        self._state: dict | None = None
        self._seen_messages: set = set()
        self._last_room: int | None = None
        self._last_refresh = 0.0
        self._had_question = False
        self._poll_timer = None

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal(id="main-layout"):
            with VerticalScroll(id="state-scroll"):
                yield GameStatePanel(id="game-panel")
            yield McpLogPanel(id="mcp-panel")
        with Horizontal(id="bottom-bar"):
            yield Static("READY", id="status")
            yield Input(
                placeholder="look at sword  /  talk to captain  /  1  /  walk 320 140",
                id="cmd-input",
            )
        yield Footer()

    def on_mount(self) -> None:
        self._poll_timer = self.set_interval(POLL_INTERVAL, self._auto_poll)
        self.run_worker(self._init_and_refresh(), exclusive=False, name="init")

    # ── Init & state refresh ─────────────────────────────────────────────────

    async def _init_and_refresh(self) -> None:
        try:
            await self.mcp.initialize()
        except McpConnectionError as e:
            def _on_dismiss(retry: bool | None) -> None:
                if retry:
                    self.run_worker(self._init_and_refresh(), exclusive=False, name="init")
                else:
                    self.app.exit()
            self.app.push_screen(
                ConnectionErrorScreen(self.mcp.host, self.mcp.port, str(e)),
                _on_dismiss,
            )
            return
        await self._do_refresh(log=True)

    async def _do_refresh(self, log: bool = False) -> None:
        ts = datetime.now().strftime("%H:%M:%S")
        mcp_log = self.query_one(McpLogPanel)
        if log:
            mcp_log.append_request("state", {}, ts)
        try:
            state = await self.mcp.call_tool("state", {})
        except McpSessionExpired:
            try:
                await self.mcp.initialize()
                state = await self.mcp.call_tool("state", {})
            except McpConnectionError as e:
                mcp_log.append_error(str(e))
                self._set_status_error()
                return
        except McpConnectionError as e:
            if log:
                mcp_log.append_error(str(e))
            self._set_status_error()
            return
        except McpError as e:
            if log:
                mcp_log.append_error(str(e))
            return

        self._last_refresh = time.monotonic()
        if log:
            mcp_log.append_state_result(state)
        self._apply_state(state)

    def _apply_state(self, state: dict) -> None:
        self._state = state
        panel = self.query_one(GameStatePanel)

        new_room = state.get("room")
        if new_room != self._last_room:
            self._seen_messages.clear()
            self._last_room = new_room

        panel.update_room_info(state)
        panel.update_objects(state.get("objects", []))
        panel.update_inventory(state.get("inventory", []))
        panel.append_messages(state.get("messages", []), self._seen_messages)
        panel.update_question(state.get("question"))

        has_q = state.get("question") is not None
        if has_q != self._had_question:
            self._had_question = has_q
            if self._poll_timer:
                self._poll_timer.stop()
            self._poll_timer = self.set_interval(
                POLL_INTERVAL_QUESTION if has_q else POLL_INTERVAL,
                self._auto_poll,
            )

    async def _auto_poll(self) -> None:
        if self.is_streaming:
            return
        if time.monotonic() - self._last_refresh < 2.5:
            return
        await self._do_refresh(log=False)

    # ── Status helpers ───────────────────────────────────────────────────────

    def _set_status_error(self) -> None:
        s = self.query_one("#status", Static)
        s.update("ERROR")
        s.remove_class("streaming")
        s.add_class("error")

    def _set_status_ready(self) -> None:
        s = self.query_one("#status", Static)
        s.update("READY")
        s.remove_class("streaming", "error")
        if not self.query_one("#cmd-input", Input).disabled:
            self.query_one("#cmd-input", Input).focus()

    # ── Reactive watch ───────────────────────────────────────────────────────

    def watch_is_streaming(self, streaming: bool) -> None:
        inp = self.query_one("#cmd-input", Input)
        status = self.query_one("#status", Static)
        inp.disabled = streaming
        if streaming:
            status.update("STREAM")
            status.remove_class("error")
            status.add_class("streaming")
        else:
            self._set_status_ready()

    # ── Input handling ───────────────────────────────────────────────────────

    def on_input_submitted(self, event: Input.Submitted) -> None:
        text = event.value.strip()
        event.input.clear()
        if self.is_streaming:
            return
        parsed = parse_command(text, self._state)
        if parsed is None:
            self.run_worker(self._do_refresh(log=True), exclusive=False, name="refresh")
            return
        tool, params = parsed
        ts = datetime.now().strftime("%H:%M:%S")
        self.query_one(McpLogPanel).append_request(tool, params, ts)
        if tool in ("act", "answer", "walk"):
            self._run_action(tool, params)
        else:
            self.run_worker(self._do_refresh(log=True), exclusive=False, name="refresh")

    # ── Streaming worker ─────────────────────────────────────────────────────

    @work(exclusive=True, name="action")
    async def _run_action(self, tool: str, params: dict) -> None:
        self.is_streaming = True
        try:
            await self._stream(tool, params)
        finally:
            self.is_streaming = False

    async def _stream(self, tool: str, params: dict, retried: bool = False) -> None:
        try:
            async for kind, payload in self.mcp.stream_tool(tool, params):
                if kind == "event":
                    self.post_message(SseEvent(
                        payload.get("text", ""),
                        payload.get("actor", ""),
                    ))
                elif kind == "result":
                    self.post_message(ActionComplete(payload))
                    return
                elif kind == "error":
                    self.post_message(ActionFailed(payload.get("message", "Unknown error")))
                    return
        except McpSessionExpired:
            if retried:
                self.post_message(ActionFailed("Session expired; reconnect failed"))
                return
            try:
                await self.mcp.initialize()
            except McpConnectionError as e:
                self.post_message(ActionFailed(str(e)))
                return
            await self._stream(tool, params, retried=True)
        except McpConnectionError as e:
            self.post_message(ActionFailed(str(e)))

    # ── Message handlers ─────────────────────────────────────────────────────

    def on_sse_event(self, msg: SseEvent) -> None:
        self.query_one(McpLogPanel).append_sse_event(msg.text, msg.actor)
        self.query_one(GameStatePanel).append_live_message(msg.text, msg.actor)
        # add to seen so the follow-up state refresh doesn't duplicate it
        self._seen_messages.add((msg.text, msg.actor))

    def on_action_complete(self, msg: ActionComplete) -> None:
        self.query_one(McpLogPanel).append_result(msg.result)
        self.run_worker(self._do_refresh(log=False), exclusive=False, name="post-action")

    def on_action_failed(self, msg: ActionFailed) -> None:
        self.query_one(McpLogPanel).append_error(msg.error)

    # ── Key bindings ─────────────────────────────────────────────────────────

    def action_refresh(self) -> None:
        if not self.is_streaming:
            self.run_worker(self._do_refresh(log=True), exclusive=False, name="refresh")

    def action_clear_input(self) -> None:
        self.query_one("#cmd-input", Input).clear()

# ── App ────────────────────────────────────────────────────────────────────────

class ScummVMApp(App):
    TITLE = "ScummVM MCP"
    SUB_TITLE = "Interactive game controller"
    BINDINGS = [Binding("ctrl+c", "quit", "Quit", priority=True)]

    DEFAULT_CSS = """
    Header { background: $primary-darken-2; }
    """

    def __init__(self, host: str, port: int) -> None:
        super().__init__()
        self._mcp = McpClient(host, port)

    def on_mount(self) -> None:
        self.push_screen(MainScreen(self._mcp))

    async def on_unmount(self) -> None:
        await self._mcp.aclose()

# ── Entry point ────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Terminal UI for the ScummVM MCP server",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help="MCP server host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="MCP server port")
    args = parser.parse_args()
    ScummVMApp(args.host, args.port).run()


if __name__ == "__main__":
    main()
