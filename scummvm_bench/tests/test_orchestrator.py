"""Worker pool execution and the orchestrator matrix/local grouping."""

import scummvm_bench.orchestrator as orch_mod
from scummvm_bench.models import LocalModelSpec, RunResult, RunSpec
from scummvm_bench.orchestrator import MatrixSelection, Orchestrator, build_matrix
from scummvm_bench.pool import PoolConfig, WorkerPool
from scummvm_bench.session import SessionConfig


def dummy_run(spec: RunSpec) -> RunResult:
    """Top-level (picklable) run function returning a trivial result."""
    return RunResult(
        spec=spec,
        goals=[],
        calls=[],
        call_count=0,
        elapsed_s=0.001,
        score_pct=0.0,
        stopped_by_goal=False,
        stopped_by_limit=None,
    )


def _specs(n: int) -> list[RunSpec]:
    return [RunSpec("none", None, None, f"g{i}", None) for i in range(n)]


def test_build_matrix_empty_models_uses_none_pair() -> None:
    sel = MatrixSelection(["none"], [], [("g", 1)])
    specs = build_matrix(sel)
    assert len(specs) == 1
    assert specs[0].provider is None and specs[0].model is None


def test_pool_sequential() -> None:
    pool = WorkerPool(PoolConfig(workers=1, work_type="thread"))
    results = pool.map(dummy_run, _specs(3))
    assert [r.spec.game_id for r in results] == ["g0", "g1", "g2"]


def test_pool_threads_preserve_order() -> None:
    pool = WorkerPool(PoolConfig(workers=3, work_type="thread"))
    results = pool.map(dummy_run, _specs(5))
    assert [r.spec.game_id for r in results] == [f"g{i}" for i in range(5)]


def test_pool_async() -> None:
    pool = WorkerPool(PoolConfig(workers=2, work_type="async"))
    results = pool.map(dummy_run, _specs(4))
    assert [r.spec.game_id for r in results] == [f"g{i}" for i in range(4)]


def test_pool_process() -> None:
    pool = WorkerPool(PoolConfig(workers=2, work_type="process"))
    results = pool.map(dummy_run, _specs(3))
    assert {r.spec.game_id for r in results} == {"g0", "g1", "g2"}


def test_pool_invalid_work_type() -> None:
    import pytest

    with pytest.raises(ValueError):
        PoolConfig(work_type="nonsense")


def test_orchestrator_runs_all_specs() -> None:
    orch = Orchestrator(
        session_config=SessionConfig("scummvm", {}),
        pool_config=PoolConfig(1, "thread"),
        run_fn=dummy_run,
    )
    specs = build_matrix(MatrixSelection(["none"], [], [("g", 1), ("h", 2)]))
    results = orch.run(specs)
    assert len(results) == 2


def test_orchestrator_local_brackets_each_model(monkeypatch) -> None:
    entered: list[str] = []

    class FakeManager:
        def __init__(self, model, **kwargs) -> None:
            self.model = model

        def __enter__(self):
            entered.append(self.model.model)
            return self

        def __exit__(self, *exc) -> None:
            return None

    monkeypatch.setattr(orch_mod, "LmStudioManager", FakeManager)

    sel = MatrixSelection(
        harnesses=["pi"],
        models=[("local", "m1"), ("local", "m2")],
        games=[("g", 1)],
        local=True,
    )
    specs = build_matrix(sel)
    orch = Orchestrator(
        session_config=SessionConfig("scummvm", {}),
        pool_config=PoolConfig(1, "thread"),
        local=True,
        local_models={
            ("local", "m1"): LocalModelSpec(key="k1", provider="local", model="m1"),
            ("local", "m2"): LocalModelSpec(key="k2", provider="local", model="m2"),
        },
        run_fn=dummy_run,
    )
    results = orch.run(specs)
    assert len(results) == 2
    assert entered == ["m1", "m2"]
