"""The FastMCP proxy the harness connects to.

Every tool call is forwarded to the backend, classified (ok / invalid_request /
invalid_response / backend_error), recorded, and scored against the goal set. The
proxy keeps a small cached view of the game state so goal predicates can inspect
the room the player was in *before* a call without an extra round-trip per call.
"""

import copy
import threading
from collections.abc import Callable

from fastmcp import FastMCP
from fastmcp.server.middleware import Middleware, MiddlewareContext

from .backend import (
    Backend,
    BackendError,
    BackendInvalidRequest,
    BackendInvalidResponse,
)
from .goals.engine import GoalEvent, GoalSet
from .models import (
    FAILURE_BACKEND_ERROR,
    FAILURE_INVALID_REQUEST,
    FAILURE_INVALID_RESPONSE,
)
from .recorder import Decision, Recorder

StopCallback = Callable[[Decision], None]


class BenchProxy:
    """Wires a backend + recorder + goal set behind a FastMCP server."""

    def __init__(
        self,
        backend: Backend,
        recorder: Recorder,
        goal_set: GoalSet,
        on_stop: StopCallback | None = None,
    ) -> None:
        self.backend = backend
        self.recorder = recorder
        self.goal_set = goal_set
        self.on_stop = on_stop or (lambda _decision: None)
        self._state_cache: dict[str, object] = {}
        self.app = self._build_app()

    # -- lifecycle ---------------------------------------------------------

    def prime(self) -> None:
        """Seed the cached state once, after the backend has started."""
        try:
            self._state_cache = self.backend.state()
        except Exception:  # noqa: BLE001 - state read is best-effort
            self._state_cache = {}

    # -- dispatch ----------------------------------------------------------

    def dispatch(self, tool: str, args: dict[str, object]) -> dict[str, object]:
        """Forward one tool call, record it, evaluate goals, maybe stop."""
        state_before = copy.deepcopy(self._state_cache)
        result: dict[str, object] | None = None
        failure: str | None = None
        try:
            result = self.backend.call(tool, args)
        except BackendInvalidRequest:
            failure = FAILURE_INVALID_REQUEST
        except BackendInvalidResponse:
            failure = FAILURE_INVALID_RESPONSE
        except BackendError:
            failure = FAILURE_BACKEND_ERROR

        if failure is None and result is not None:
            self._update_cache(tool, result)

        event = GoalEvent(
            tool=tool,
            args=args,
            result=result if failure is None else None,
            state_before=state_before,
            ok=failure is None,
        )
        decision = self.recorder.record(event, failure)
        if decision.stop:
            self.on_stop(decision)

        if failure is not None:
            return {"error": failure}
        return result if result is not None else {}

    def record_invalid_request(self, tool: str, args: dict[str, object]) -> None:
        """Record a schema/validation rejection caught by the middleware."""
        event = GoalEvent(
            tool=tool,
            args=args,
            result=None,
            state_before=copy.deepcopy(self._state_cache),
            ok=False,
        )
        self.recorder.record(event, FAILURE_INVALID_REQUEST)

    def _update_cache(self, tool: str, result: dict[str, object]) -> None:
        if tool == "state":
            self._state_cache = result
            return
        room_changed = result.get("room_changed")
        if isinstance(room_changed, int):
            updated = dict(self._state_cache)
            updated["room"] = {"id": room_changed}
            self._state_cache = updated

    # -- FastMCP wiring ----------------------------------------------------

    def _build_app(self) -> FastMCP:
        app: FastMCP = FastMCP("scummvm-bench")
        app.add_middleware(_RecordingMiddleware(self))
        dispatch = self.dispatch

        @app.tool
        def state() -> dict[str, object]:
            """Get the current game state (room, position, inventory, objects)."""
            return dispatch("state", {})

        @app.tool
        def act(
            verb: str,
            target1: str | int | None = None,
            target2: str | int | None = None,
        ) -> dict[str, object]:
            """Execute a verb on up to two targets (e.g. walk_to / pick_up / use)."""
            return dispatch(
                "act", {"verb": verb, "target1": target1, "target2": target2}
            )

        @app.tool
        def answer(id: int) -> dict[str, object]:
            """Select a dialog choice by its 1-based id."""
            return dispatch("answer", {"id": id})

        @app.tool
        def walk(x: int, y: int) -> dict[str, object]:
            """Walk to a pixel coordinate in the current room."""
            return dispatch("walk", {"x": x, "y": y})

        @app.tool
        def skip() -> dict[str, object]:
            """Skip the current cutscene / advance text."""
            return dispatch("skip", {})

        @app.tool
        def play_note(
            note: str | None = None,
            notes: list[str] | None = None,
        ) -> dict[str, object]:
            """Play one note or a sequence of notes (Loom distaff)."""
            return dispatch("play_note", {"note": note, "notes": notes})

        @app.tool
        def switch_character(name: str) -> dict[str, object]:
            """Switch the controlled character (Maniac Mansion)."""
            return dispatch("switch_character", {"name": name})

        @app.tool
        def dial(number: str) -> dict[str, object]:
            """Dial a number on the phone dial pad (Maniac Mansion)."""
            return dispatch("dial", {"number": number})

        @app.tool
        def shoot_cannon(x: int, y: int) -> dict[str, object]:
            """Aim and fire the cannon at a coordinate (Curse of Monkey Island)."""
            return dispatch("shoot_cannon", {"x": x, "y": y})

        @app.tool
        def keystroke(key: str) -> dict[str, object]:
            """Send a raw keypress (numpad 1-9 drives the Indy3 boxing fight)."""
            return dispatch("keystroke", {"key": key})

        return app


class _RecordingMiddleware(Middleware):
    """Records schema/validation rejections as ``invalid_request`` calls."""

    def __init__(self, proxy: BenchProxy) -> None:
        self._proxy = proxy

    async def on_call_tool(self, context: MiddlewareContext, call_next):
        try:
            return await call_next(context)
        except Exception:
            tool, args = _tool_and_args(context)
            self._proxy.record_invalid_request(tool, args)
            raise


def _tool_and_args(context: MiddlewareContext) -> tuple[str, dict[str, object]]:
    message = getattr(context, "message", None)
    tool = getattr(message, "name", None) or "?"
    args = getattr(message, "arguments", None)
    return str(tool), dict(args) if isinstance(args, dict) else {}


class ProxyServer:
    """Serves a :class:`BenchProxy` over Streamable-HTTP in a background thread."""

    def __init__(self, proxy: BenchProxy, host: str, port: int) -> None:
        self.proxy = proxy
        self.host = host
        self.port = port
        self._server: object | None = None
        self._thread: threading.Thread | None = None

    def start(self) -> None:
        import uvicorn

        config = uvicorn.Config(
            self.proxy.app.http_app(),
            host=self.host,
            port=self.port,
            log_level="warning",
        )
        server = uvicorn.Server(config)
        # Signal handlers can only be installed on the main thread; disable them
        # so the server can run inside a worker thread.
        setattr(server, "install_signal_handlers", lambda: None)  # noqa: B010
        self._server = server
        self._thread = threading.Thread(target=server.run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        server = self._server
        if server is not None:
            setattr(server, "should_exit", True)  # noqa: B010
        thread = self._thread
        if thread is not None:
            thread.join(timeout=5)
