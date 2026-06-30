"""One benchmark cell: wire backend + proxy + recorder + harness for a RunSpec."""

import os
import tempfile
import threading
from dataclasses import dataclass

from .backend import Backend, RealBackend
from .goals import get_goal_set
from .harness import HarnessRunner, RunContext, make_harness
from .models import GameSpec, RunResult, RunSpec
from .ports import reserve_pair
from .proxy import BenchProxy, ProxyServer
from .recorder import LimitConfig, Recorder

HOST = "127.0.0.1"


@dataclass
class SessionConfig:
    """The static inputs a session needs to launch a real ScummVM."""

    scummvm_binary: str
    games: dict[str, GameSpec]
    save_folder: str | None = None
    # Optional overrides for the coding harnesses' config directories. When
    # unset each harness defaults to an isolated subdir of the run's agent_dir.
    pi_config_dir: str | None = None
    claude_config_dir: str | None = None


def build_real_backend(
    spec: RunSpec,
    scummvm_port: int,
    backend_dir: str,
    config: SessionConfig,
) -> RealBackend:
    """Construct a :class:`RealBackend` for ``spec`` from the session config."""
    game = config.games.get(spec.game_id)
    if game is None:
        raise KeyError(f"no game registered for {spec.game_id!r}")
    save_slot = spec.save_slot if spec.save_slot is not None else game.save_slot
    save_path = None
    if config.save_folder:
        save_path = os.path.join(config.save_folder, spec.game_id)
    return RealBackend(
        game_id=spec.game_id,
        game_path=game.game_path,
        scummvm_binary=config.scummvm_binary,
        scummvm_port=scummvm_port,
        session_dir=backend_dir,
        save_slot=save_slot,
        save_path=save_path,
        ini_template=game.ini_template,
    )


def _make_session_dirs(game_id: str) -> tuple[str, str]:
    """Create a fresh root with separate ``(agent_dir, backend_dir)``.

    They are kept apart so the agent cannot read the rendered ini (which leaks
    the ScummVM port) or the engine logs from its own working directory.
    """
    root = tempfile.mkdtemp(prefix=f"bench_{game_id}_")
    agent_dir = os.path.join(root, "agent")
    backend_dir = os.path.join(root, "backend")
    os.makedirs(agent_dir, exist_ok=True)
    os.makedirs(backend_dir, exist_ok=True)
    return agent_dir, backend_dir


def _build_context(
    spec: RunSpec,
    *,
    bench_port: int,
    scummvm_port: int,
    agent_dir: str,
    backend_dir: str,
    stop_event: threading.Event,
    proxy: BenchProxy,
    config: SessionConfig | None,
) -> RunContext:
    """Assemble the :class:`RunContext` the harness drives, pulling the game data
    paths and config-dir overrides from ``config`` when present."""
    game = config.games.get(spec.game_id) if config else None
    return RunContext(
        spec=spec,
        bench_port=bench_port,
        agent_dir=agent_dir,
        stop_event=stop_event,
        proxy=proxy,
        backend_dir=backend_dir,
        scummvm_port=scummvm_port,
        game_path=game.game_path if game else None,
        save_folder=config.save_folder if config else None,
        pi_config_dir=config.pi_config_dir if config else None,
        claude_config_dir=config.claude_config_dir if config else None,
    )


def _find_transcript(agent_dir: str) -> str | None:
    """Return the agent's transcript file in ``agent_dir``, if one was written."""
    for name in ("pi.jsonl", "claude.jsonl"):
        path = os.path.join(agent_dir, name)
        if os.path.exists(path):
            return path
    return None


class BenchSession:
    """Runs a single ``RunSpec`` and returns its ``RunResult``."""

    def execute(
        self,
        spec: RunSpec,
        *,
        backend: Backend | None = None,
        harness: HarnessRunner | None = None,
        config: SessionConfig | None = None,
        serve: bool = True,
    ) -> RunResult:
        bench_port, scummvm_port = reserve_pair()
        agent_dir, backend_dir = _make_session_dirs(spec.game_id)
        goal_set = get_goal_set(spec.game_id, spec.save_slot)
        recorder = Recorder(goal_set, LimitConfig(spec.max_calls, spec.time_limit_s))
        stop_event = threading.Event()

        if backend is None:
            if config is None:
                raise ValueError("config is required when no backend is injected")
            backend = build_real_backend(spec, scummvm_port, backend_dir, config)

        proxy = BenchProxy(
            backend, recorder, goal_set, on_stop=lambda _d: stop_event.set()
        )
        runner = harness or make_harness(spec.harness)
        ctx = _build_context(
            spec,
            bench_port=bench_port,
            scummvm_port=scummvm_port,
            agent_dir=agent_dir,
            backend_dir=backend_dir,
            stop_event=stop_event,
            proxy=proxy,
            config=config,
        )
        error = self._serve_and_run(backend, proxy, runner, ctx, bench_port, serve)

        result = recorder.result(spec, error=error)
        result.transcript_path = _find_transcript(agent_dir)
        return result

    def _serve_and_run(
        self,
        backend: Backend,
        proxy: BenchProxy,
        runner: HarnessRunner,
        ctx: RunContext,
        bench_port: int,
        serve: bool,
    ) -> str | None:
        """Start the backend + proxy, run the harness, and always tear down.

        Returns the harness error (or any startup/run exception rendered as a
        string), or ``None`` on success."""
        server: ProxyServer | None = None
        try:
            backend.start()
            proxy.prime()
            if serve:
                server = ProxyServer(proxy, HOST, bench_port)
                server.start()
            return runner.run(ctx)
        except Exception as exc:  # noqa: BLE001 - surface as a run error
            return f"{type(exc).__name__}: {exc}"
        finally:
            if server is not None:
                server.stop()
            try:
                backend.stop()
            except Exception:  # noqa: BLE001
                pass


def run_spec(spec: RunSpec, config: SessionConfig) -> RunResult:
    """Top-level, picklable entry point for worker pools."""
    return BenchSession().execute(spec, config=config)
