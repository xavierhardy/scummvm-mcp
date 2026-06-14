"""The backend abstraction the proxy forwards tool calls to.

``RealBackend`` drives an actual headless ScummVM instance via the vendored
:class:`McpClient`. ``MockBackend`` replays a scripted state machine so the whole
bench can be exercised deterministically with no binary, ports, or game files.
"""

import copy
import os
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Protocol, runtime_checkable

import httpx

from .ini import default_template_path, render_ini
from .mcp_client import (
    INVALID_REQUEST_CODES,
    McpClient,
    McpDecodeError,
    McpError,
    launch_scummvm,
    wait_for_mcp,
)


class BackendInvalidRequest(Exception):
    """The agent's request was rejected (bad args / unknown verb)."""


class BackendInvalidResponse(Exception):
    """The server returned something undecodable / malformed."""


class BackendError(Exception):
    """A transport or process fault unrelated to request validity."""


@runtime_checkable
class Backend(Protocol):
    """Everything the proxy needs from a ScummVM (real or mock)."""

    def start(self) -> None: ...

    def state(self) -> dict[str, object]: ...

    def call(self, tool: str, args: dict[str, object]) -> dict[str, object]: ...

    def stop(self) -> None: ...


# ---------------------------------------------------------------------------
# Real backend
# ---------------------------------------------------------------------------


class RealBackend:
    """Forwards calls to a headless ScummVM instance over MCP."""

    def __init__(
        self,
        game_id: str,
        game_path: str,
        scummvm_binary: str,
        scummvm_port: int,
        session_dir: str,
        save_slot: int | None = None,
        save_path: str | None = None,
        ini_template: str | None = None,
    ) -> None:
        self.game_id = game_id
        self.game_path = game_path
        self.scummvm_binary = scummvm_binary
        self.scummvm_port = scummvm_port
        self.session_dir = session_dir
        self.save_slot = save_slot
        self.save_path = save_path
        self.ini_template = ini_template or default_template_path(game_id)
        self._client: McpClient | None = None
        self._proc: object | None = None

    def start(self) -> None:
        ini_path = os.path.join(self.session_dir, "scummvm.ini")
        logfile = os.path.join(self.session_dir, "scummvm.engine.log")
        render_ini(
            self.ini_template,
            self.game_path,
            self.scummvm_port,
            logfile,
            ini_path,
        )
        proc = launch_scummvm(
            scummvm_binary=self.scummvm_binary,
            ini_path=ini_path,
            game_id=self.game_id,
            save_slot=self.save_slot,
            save_path=self.save_path,
            stdout_path=os.path.join(self.session_dir, "scummvm.stdout.log"),
            stderr_path=os.path.join(self.session_dir, "scummvm.stderr.log"),
        )
        self._proc = proc
        self._client = wait_for_mcp("127.0.0.1", self.scummvm_port)

    def state(self) -> dict[str, object]:
        return self._guard(lambda: self._require_client().state())

    def call(self, tool: str, args: dict[str, object]) -> dict[str, object]:
        clean = {k: v for k, v in args.items() if v is not None}
        return self._guard(lambda: self._require_client().call_tool(tool, clean))

    def stop(self) -> None:
        if self._client is not None:
            try:
                self._client.close()
            except Exception:  # noqa: BLE001
                pass
        proc = self._proc
        if proc is not None:
            for method, kwargs in (("kill", {}), ("wait", {"timeout": 5})):
                try:
                    getattr(proc, method)(**kwargs)
                except Exception:  # noqa: BLE001
                    pass

    def _require_client(self) -> McpClient:
        if self._client is None:
            raise BackendError("backend not started")
        return self._client

    @staticmethod
    def _guard(fn: Callable[[], dict[str, object]]) -> dict[str, object]:
        try:
            return fn()
        except McpError as exc:
            if exc.code in INVALID_REQUEST_CODES:
                raise BackendInvalidRequest(str(exc)) from exc
            raise BackendError(str(exc)) from exc
        except McpDecodeError as exc:
            raise BackendInvalidResponse(str(exc)) from exc
        except (httpx.HTTPError, OSError) as exc:
            raise BackendError(str(exc)) from exc


# ---------------------------------------------------------------------------
# Mock backend
# ---------------------------------------------------------------------------


@dataclass
class ScriptStep:
    """A scripted response: when ``tool`` is called with args matching ``match``
    (a subset, compared case-insensitively), return a copy of ``result``.

    Steps sharing the same ``(tool, match)`` are consumed in declaration order,
    so a verb that produces different outcomes on successive calls (e.g. walking
    through the same ``door`` into different rooms) can be scripted faithfully.
    Once every matching step is consumed, the last one is replayed.
    """

    tool: str
    match: dict[str, object]
    result: dict[str, object] = field(default_factory=dict)
    used: bool = field(default=False, compare=False)


def _norm(value: object) -> str:
    return str(value).strip().rstrip("@").lower()


class MockBackend:
    """Replays scripted responses and tracks room/inventory deterministically."""

    def __init__(
        self,
        steps: list[ScriptStep],
        initial_room: int,
        initial_inventory: list[str] | None = None,
    ) -> None:
        self._steps = steps
        self._room = initial_room
        self._inventory = list(initial_inventory or [])

    def start(self) -> None:
        return None

    def stop(self) -> None:
        return None

    def state(self) -> dict[str, object]:
        return {
            "room": {"id": self._room},
            "position": {"x": 0, "y": 0},
            "inventory": list(self._inventory),
            "objects": [],
            "messages": [],
        }

    def call(self, tool: str, args: dict[str, object]) -> dict[str, object]:
        if tool == "state":
            return self.state()
        step = self._match(tool, args)
        if step is None:
            raise BackendInvalidRequest(
                f"no scripted response for tool={tool!r} args={args!r}"
            )
        step.used = True
        result = copy.deepcopy(step.result)
        self._apply(result)
        return result

    def _match(self, tool: str, args: dict[str, object]) -> ScriptStep | None:
        fallback: ScriptStep | None = None
        for step in self._steps:
            if step.tool != tool:
                continue
            if not all(
                _norm(args.get(key)) == _norm(value)
                for key, value in step.match.items()
            ):
                continue
            fallback = step
            if not step.used:
                return step
        return fallback

    def _apply(self, result: dict[str, object]) -> None:
        room_changed = result.get("room_changed")
        if isinstance(room_changed, int):
            self._room = room_changed
        added = result.get("inventory_added")
        if isinstance(added, list):
            for item in added:
                if isinstance(item, str):
                    self._inventory.append(item)
        removed = result.get("inventory_removed")
        if isinstance(removed, list):
            for item in removed:
                if isinstance(item, str) and item in self._inventory:
                    self._inventory.remove(item)
