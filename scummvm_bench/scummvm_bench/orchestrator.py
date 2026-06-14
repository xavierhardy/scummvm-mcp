"""Build the run matrix (model -> harness -> game -> save) and execute it.

The nesting puts the model outermost so the ``--local`` flow can bracket all of a
model's runs inside a single LM Studio download/load/unload/delete lifecycle.
"""

import functools
from collections.abc import Callable
from dataclasses import dataclass, field

from .lmstudio import LmStudioManager
from .models import LocalModelSpec, RunResult, RunSpec
from .pool import PoolConfig, WorkerPool
from .session import SessionConfig, run_spec

RunFn = Callable[[RunSpec], RunResult]


@dataclass
class MatrixSelection:
    """The user's selection that expands into the run matrix."""

    harnesses: list[str]
    models: list[tuple[str | None, str | None]]
    games: list[tuple[str, int | None]]
    max_calls: int | None = None
    time_limit_s: float | None = None
    local: bool = False


def build_matrix(selection: MatrixSelection) -> list[RunSpec]:
    """Expand a selection into ``RunSpec``s, model outermost, save-state innermost."""
    models = selection.models or [(None, None)]
    specs: list[RunSpec] = []
    for provider, model in models:
        for harness in selection.harnesses:
            for game_id, save_slot in selection.games:
                specs.append(
                    RunSpec(
                        harness=harness,
                        provider=provider,
                        model=model,
                        game_id=game_id,
                        save_slot=save_slot,
                        max_calls=selection.max_calls,
                        time_limit_s=selection.time_limit_s,
                        local=selection.local,
                    )
                )
    return specs


def _group_by_model(
    specs: list[RunSpec],
) -> list[tuple[tuple[str | None, str | None], list[RunSpec]]]:
    groups: dict[tuple[str | None, str | None], list[RunSpec]] = {}
    for spec in specs:
        groups.setdefault((spec.provider, spec.model), []).append(spec)
    return list(groups.items())


@dataclass
class Orchestrator:
    """Executes a list of specs through a worker pool, per-model for ``--local``."""

    session_config: SessionConfig
    pool_config: PoolConfig = field(default_factory=PoolConfig)
    local: bool = False
    local_models: dict[tuple[str | None, str | None], LocalModelSpec] = field(
        default_factory=dict
    )
    lms_bin: str = "lms"
    models_json: str | None = None
    run_fn: RunFn | None = None

    def run(self, specs: list[RunSpec]) -> list[RunResult]:
        pool = WorkerPool(self.pool_config)
        fn = self.run_fn or functools.partial(run_spec, config=self.session_config)
        results: list[RunResult] = []
        for (provider, model), group in _group_by_model(specs):
            local_model = self.local_models.get((provider, model))
            if self.local and local_model is not None:
                with LmStudioManager(
                    local_model, lms_bin=self.lms_bin, models_json=self.models_json
                ):
                    results.extend(pool.map(fn, group))
            else:
                results.extend(pool.map(fn, group))
        return results
