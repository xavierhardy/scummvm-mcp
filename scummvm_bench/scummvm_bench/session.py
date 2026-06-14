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


def build_real_backend(
    spec: RunSpec,
    scummvm_port: int,
    session_dir: str,
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
        session_dir=session_dir,
        save_slot=save_slot,
        save_path=save_path,
        ini_template=game.ini_template,
    )


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
        session_dir = tempfile.mkdtemp(prefix=f"bench_{spec.game_id}_")
        goal_set = get_goal_set(spec.game_id, spec.save_slot)
        recorder = Recorder(goal_set, LimitConfig(spec.max_calls, spec.time_limit_s))
        stop_event = threading.Event()

        if backend is None:
            if config is None:
                raise ValueError("config is required when no backend is injected")
            backend = build_real_backend(spec, scummvm_port, session_dir, config)

        proxy = BenchProxy(
            backend, recorder, goal_set, on_stop=lambda _d: stop_event.set()
        )
        runner = harness or make_harness(spec.harness)
        server: ProxyServer | None = None
        error: str | None = None
        try:
            backend.start()
            proxy.prime()
            if serve:
                server = ProxyServer(proxy, HOST, bench_port)
                server.start()
            ctx = RunContext(
                spec=spec,
                bench_port=bench_port,
                session_dir=session_dir,
                stop_event=stop_event,
                proxy=proxy,
            )
            error = runner.run(ctx)
        except Exception as exc:  # noqa: BLE001 - surface as a run error
            error = f"{type(exc).__name__}: {exc}"
        finally:
            if server is not None:
                server.stop()
            try:
                backend.stop()
            except Exception:  # noqa: BLE001
                pass

        result = recorder.result(spec, error=error)
        transcript = os.path.join(session_dir, "pi.jsonl")
        if os.path.exists(transcript):
            result.transcript_path = transcript
        return result


def run_spec(spec: RunSpec, config: SessionConfig) -> RunResult:
    """Top-level, picklable entry point for worker pools."""
    return BenchSession().execute(spec, config=config)
