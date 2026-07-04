"""The FastMCP proxy the harness connects to.

Every tool call is forwarded to the backend, classified (ok / invalid_request /
invalid_response / backend_error), recorded, and scored against the goal set. The
proxy keeps a small cached view of the game state so goal predicates can inspect
the room the player was in *before* a call without an extra round-trip per call.
"""

import copy
import keyword
import threading
from collections.abc import Callable

from fastmcp import FastMCP
from fastmcp.server.middleware import Middleware, MiddlewareContext
from fastmcp.tools.function_tool import FunctionTool

from .backend import (
    DEFAULT_GAMEPLAY_TOOLS,
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

# Debug tools (gated by ``mcp_debug`` in the engine) that drive the cursor or
# read engine internals directly. Exposing them would let the agent bypass the
# semantic tool surface the benchmark scores, so they are filtered out of the
# proxy even when a debug-enabled game advertises them. ``keystroke`` is *not*
# here: it is the legitimate input for the Indy3 boxing minigame.
DENIED_TOOLS = frozenset(
    {"debug", "save_state", "set_talk_speed", "mouse_move", "mouse_click", "screenshot"}
)


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
        # Built from whatever tools the backend can advertise right now (the
        # fallback set if it is not started yet); prime() rebuilds it once the
        # backend is live so the surface matches the running engine exactly.
        self.app = self._build_app(self._discover_tools())

    # -- lifecycle ---------------------------------------------------------

    def prime(self) -> None:
        """Seed the cached state and lock in the tool surface, post-backend-start."""
        try:
            self._state_cache = self.backend.state()
        except Exception:  # noqa: BLE001 - state read is best-effort
            self._state_cache = {}
        self.app = self._build_app(self._discover_tools())

    def _discover_tools(self) -> list[dict[str, object]]:
        """Tools to expose: the backend's advertised set minus the denylist.

        Falls back to the canonical gameplay set if the backend cannot be asked
        (e.g. not started yet, or a transport hiccup) so the proxy always has a
        usable surface."""
        try:
            advertised = self.backend.list_tools()
        except Exception:  # noqa: BLE001 - fall back to the default surface
            advertised = []
        if not advertised:
            advertised = DEFAULT_GAMEPLAY_TOOLS
        return [
            t
            for t in advertised
            if isinstance(t.get("name"), str) and t["name"] not in DENIED_TOOLS
        ]

    # -- dispatch ----------------------------------------------------------

    def dispatch(self, tool: str, args: dict[str, object]) -> dict[str, object]:
        """Forward one tool call, record it, evaluate goals, maybe stop."""
        state_before = copy.deepcopy(self._state_cache)

        # A call-based stopping goal that is already satisfied by this call's
        # tool/args plus the pre-call state ends the run *on receipt* — before
        # forwarding. This is essential for the COMI escape: firing the
        # unrestrained cannon starts a closing video the engine never settles out
        # of, so forwarding it would hang; instead we stop the moment the cannon
        # act arrives. Result-based stopping goals never match here (no result
        # yet), so they still forward and settle as usual.
        pre_event = GoalEvent(
            tool=tool, args=args, result=None, state_before=state_before, ok=True
        )
        if self.recorder.stopping_goal_pending_on(pre_event):
            decision = self.recorder.record(pre_event, None)
            if decision.stop:
                self.on_stop(decision)
            return {}

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
            self._update_cache(tool, args, result)

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

    def _update_cache(
        self, tool: str, args: dict[str, object], result: dict[str, object]
    ) -> None:
        if tool == "state":
            # A state read replaces the cache but must not drop the act history
            # accumulated from earlier world actions.
            acts = self._state_cache.get("acts")
            self._state_cache = result
            if isinstance(acts, list):
                self._state_cache["acts"] = acts
            return
        updated = dict(self._state_cache)
        room_changed = result.get("room_changed")
        if isinstance(room_changed, int):
            updated["room"] = {"id": room_changed}
        # Track inventory across actions so state-guard predicates (in_inventory)
        # are accurate between explicit state() reads. Mirrors how the engine
        # reports inventory_added/removed in each action result.
        added = result.get("inventory_added")
        removed = result.get("inventory_removed")
        if isinstance(added, list) or isinstance(removed, list):
            inv = [i for i in updated.get("inventory", []) if isinstance(i, str)]
            for item in removed if isinstance(removed, list) else []:
                if isinstance(item, str) and item in inv:
                    inv.remove(item)
            for item in added if isinstance(added, list) else []:
                if isinstance(item, str) and item not in inv:
                    inv.append(item)
            updated["inventory"] = inv
        # Record each successful act in the snapshot so state-guard predicates
        # (after_act) can gate a later action on an earlier one — e.g. firing the
        # COMI cannon only counts once the restraint rope's cut act has already
        # happened, a "cut state" the engine does not otherwise surface. Only
        # world-changing tools are logged (state reads are not acts).
        if tool != "state":
            prior_acts = updated.get("acts")
            acts = (
                [a for a in prior_acts if isinstance(a, dict)]
                if isinstance(prior_acts, list)
                else []
            )
            acts.append(
                {
                    "verb": args.get("verb"),
                    "target1": args.get("target1"),
                    "target2": args.get("target2"),
                    "tool": tool,
                }
            )
            updated["acts"] = acts
        self._state_cache = updated

    # -- FastMCP wiring ----------------------------------------------------

    def _build_app(self, tools: list[dict[str, object]]) -> FastMCP:
        """Build a FastMCP app that mirrors ``tools``, forwarding each to dispatch.

        Every tool is registered with the server's own input schema, so the
        surface the agent sees can never drift from what the engine exposes."""
        app: FastMCP = FastMCP("scummvm-bench")
        app.add_middleware(_RecordingMiddleware(self))
        for spec in tools:
            name = str(spec["name"])
            schema = _input_schema(spec)
            description = spec.get("description")
            forwarder = _make_forwarder(name, schema, self.dispatch)
            tool = FunctionTool.from_function(
                forwarder,
                name=name,
                description=str(description) if isinstance(description, str) else None,
            )
            # Advertise the engine's exact schema rather than the one inferred
            # from the generated forwarder's (untyped) signature.
            tool.parameters = schema
            app.add_tool(tool)
        return app


def _input_schema(spec: dict[str, object]) -> dict[str, object]:
    """The tool's input schema, defaulting to an empty object when absent."""
    schema = spec.get("inputSchema")
    if isinstance(schema, dict):
        typed = {str(key): val for key, val in schema.items()}
        if isinstance(typed.get("properties"), dict):
            return typed
    return {"type": "object", "properties": {}, "additionalProperties": False}


def _make_forwarder(
    name: str,
    schema: dict[str, object],
    dispatch: Callable[[str, dict[str, object]], dict[str, object]],
) -> Callable[..., dict[str, object]]:
    """Build a function whose signature matches ``schema`` and forwards to dispatch.

    FastMCP infers a tool's callable signature (it rejects ``**kwargs``), so we
    synthesise one parameter per schema property — required ones positional, the
    rest defaulting to ``None`` — and pass them straight through to dispatch."""
    props = schema.get("properties")
    prop_names = [str(n) for n in props] if isinstance(props, dict) else []
    names = [n for n in prop_names if n.isidentifier() and not keyword.iskeyword(n)]
    required = schema.get("required")
    required_set = {str(r) for r in required} if isinstance(required, list) else set()

    params = ", ".join(n if n in required_set else f"{n}=None" for n in names)
    payload = ", ".join(f"{n!r}: {n}" for n in names)
    source = f"def _forward({params}):\n    return _dispatch({name!r}, {{{payload}}})\n"
    namespace: dict[str, object] = {"_dispatch": dispatch}
    exec(source, namespace)  # noqa: S102 - schema is server-controlled, names validated
    forwarder = namespace["_forward"]
    forwarder.__name__ = name  # type: ignore[attr-defined]
    return forwarder  # type: ignore[return-value]


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
