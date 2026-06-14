"""Session backend resolution and error handling."""

import pytest

from scummvm_bench.harness.base import NoneHarness
from scummvm_bench.models import GameSpec, RunSpec
from scummvm_bench.session import BenchSession, SessionConfig, build_real_backend


def _config() -> SessionConfig:
    return SessionConfig(
        scummvm_binary="scummvm-bin",
        games={"monkey-ega-demo": GameSpec("monkey-ega-demo", "/games/m", save_slot=1)},
        save_folder="/saves",
    )


def test_build_real_backend_uses_config_defaults() -> None:
    spec = RunSpec("none", None, None, "monkey-ega-demo", None)
    backend = build_real_backend(spec, 23999, "/tmp/sess", _config())
    assert backend.scummvm_port == 23999
    assert backend.game_path == "/games/m"
    assert backend.save_slot == 1  # filled from config
    assert backend.save_path == "/saves/monkey-ega-demo"


def test_build_real_backend_spec_slot_overrides_config() -> None:
    spec = RunSpec("none", None, None, "monkey-ega-demo", 4)
    backend = build_real_backend(spec, 1, "/tmp/sess", _config())
    assert backend.save_slot == 4


def test_build_real_backend_unknown_game() -> None:
    spec = RunSpec("none", None, None, "nope", 1)
    with pytest.raises(KeyError):
        build_real_backend(spec, 1, "/tmp/sess", _config())


def test_execute_requires_config_without_backend() -> None:
    spec = RunSpec("none", None, None, "monkey-ega-demo", 1)
    with pytest.raises(ValueError):
        BenchSession().execute(spec)


def test_execute_records_backend_start_error() -> None:
    class FailingBackend:
        def start(self) -> None:
            raise RuntimeError("boom")

        def stop(self) -> None: ...

        def state(self) -> dict:
            return {}

        def call(self, tool: str, args: dict) -> dict:
            return {}

    spec = RunSpec("none", None, None, "monkey-ega-demo", 1)
    result = BenchSession().execute(
        spec, backend=FailingBackend(), harness=NoneHarness(0.1), serve=False
    )
    assert result.error is not None
    assert "boom" in result.error
    assert result.call_count == 0
