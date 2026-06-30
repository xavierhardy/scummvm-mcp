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

    def list_tools(self) -> list[dict[str, object]]: ...

    def stop(self) -> None: ...


def _prop(type_name: str, desc: str) -> dict[str, object]:
    return {"type": type_name, "description": desc}


def _target(desc: str) -> dict[str, object]:
    return {"oneOf": [{"type": "string"}, {"type": "integer"}], "description": desc}


def _tool(
    name: str, description: str, properties: dict[str, object], required: list[str]
) -> dict[str, object]:
    return {
        "name": name,
        "description": description,
        "inputSchema": {
            "type": "object",
            "properties": properties,
            "required": required,
            "additionalProperties": False,
        },
    }


# Canonical gameplay tool surface. Used by ``MockBackend.list_tools()`` and as the
# proxy's fallback when a live server's ``tools/list`` cannot be read. The real
# backend derives its surface from the running server instead, so the proxy never
# drifts from what the engine actually exposes (see ``proxy.BenchProxy``).
DEFAULT_GAMEPLAY_TOOLS: list[dict[str, object]] = [
    _tool("state", "Get the current game state.", {}, []),
    _tool(
        "act",
        "Execute a verb on up to two targets (e.g. walk_to / pick_up / use).",
        {
            "verb": _prop("string", "The verb to perform."),
            "target1": _target("First target (object/actor name or id)."),
            "target2": _target("Second target (for two-object verbs)."),
        },
        ["verb"],
    ),
    _tool(
        "answer",
        "Select a dialog choice by its 1-based id.",
        {"id": _prop("integer", "The 1-based dialog choice id.")},
        ["id"],
    ),
    _tool(
        "walk",
        "Walk to a pixel coordinate in the current room.",
        {"x": _prop("integer", "X pixel."), "y": _prop("integer", "Y pixel.")},
        ["x", "y"],
    ),
    _tool("skip", "Skip the current cutscene / advance text.", {}, []),
    _tool(
        "play_note",
        "Play one note or a sequence of notes (Loom distaff).",
        {
            "note": _prop("string", "A single note (c d e f g a b C)."),
            "notes": {
                "type": "array",
                "items": {"type": "string"},
                "description": "A sequence of notes to play in order.",
            },
        },
        [],
    ),
    _tool(
        "switch_character",
        "Switch the controlled character (Maniac Mansion).",
        {"name": _prop("string", "The character to switch to.")},
        ["name"],
    ),
    _tool(
        "dial",
        "Dial a number on the phone dial pad (Maniac Mansion).",
        {"number": _prop("string", "The number to dial.")},
        ["number"],
    ),
    _tool(
        "shoot_cannon",
        "Aim and fire the cannon at a coordinate (Curse of Monkey Island).",
        {"x": _prop("integer", "Aim X."), "y": _prop("integer", "Aim Y.")},
        ["x", "y"],
    ),
    _tool(
        "ride_bike",
        "Play the Full Throttle motorcycle minigame (auto-plays the highway fight).",
        {},
        [],
    ),
    _tool(
        "keystroke",
        "Send a raw keypress (numpad 1-9 drives the Indy3 boxing fight).",
        {"key": _prop("string", "The key to press.")},
        ["key"],
    ),
]


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

        # The Fate of Atlantis demo's boot script overrides the configured
        # talkspeed (it sets VAR_CHARINC outside room 0, so the engine's
        # room-0-only user override is skipped). Force the max text speed at
        # runtime via the mcp_debug-gated set_talk_speed tool so lingering
        # subtitles don't stall the walkthrough.
        if self.game_id == "atlantis":
            self._client.call_tool("set_talk_speed", {"speed": 255})

    def state(self) -> dict[str, object]:
        return self._guard(lambda: self._require_client().state())

    def call(self, tool: str, args: dict[str, object]) -> dict[str, object]:
        clean = {k: v for k, v in args.items() if v is not None}
        return self._guard(lambda: self._require_client().call_tool(tool, clean))

    def list_tools(self) -> list[dict[str, object]]:
        try:
            return self._require_client().list_tools()
        except McpError as exc:
            raise BackendError(str(exc)) from exc
        except McpDecodeError as exc:
            raise BackendInvalidResponse(str(exc)) from exc
        except (httpx.HTTPError, OSError) as exc:
            raise BackendError(str(exc)) from exc

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

    def list_tools(self) -> list[dict[str, object]]:
        return [copy.deepcopy(t) for t in DEFAULT_GAMEPLAY_TOOLS]

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
