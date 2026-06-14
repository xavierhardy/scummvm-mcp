"""Worker pool for running the benchmark matrix sequentially or in parallel.

``workers == 1`` runs sequentially with no executor. Otherwise the ``work_type``
selects the parallelism backend: threads (default), processes (true isolation;
the mapped function and its arguments must be picklable), or asyncio (blocking
work offloaded to a thread per task, bounded by a semaphore).
"""

import asyncio
from collections.abc import Callable
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from dataclasses import dataclass

from .models import RunResult, RunSpec

WORK_TYPES = ("async", "thread", "process")

RunFn = Callable[[RunSpec], RunResult]


@dataclass(frozen=True)
class PoolConfig:
    """How to execute the matrix."""

    workers: int = 1
    work_type: str = "thread"

    def __post_init__(self) -> None:
        if self.work_type not in WORK_TYPES:
            raise ValueError(
                f"work_type must be one of {WORK_TYPES}, got {self.work_type!r}"
            )


class WorkerPool:
    """Maps a run function over a list of specs per the pool configuration."""

    def __init__(self, config: PoolConfig) -> None:
        self.config = config

    def map(self, fn: RunFn, specs: list[RunSpec]) -> list[RunResult]:
        if self.config.workers <= 1 or len(specs) <= 1:
            return [fn(spec) for spec in specs]
        if self.config.work_type == "process":
            with ProcessPoolExecutor(max_workers=self.config.workers) as executor:
                return list(executor.map(fn, specs))
        if self.config.work_type == "async":
            return asyncio.run(self._map_async(fn, specs))
        with ThreadPoolExecutor(max_workers=self.config.workers) as executor:
            return list(executor.map(fn, specs))

    async def _map_async(self, fn: RunFn, specs: list[RunSpec]) -> list[RunResult]:
        semaphore = asyncio.Semaphore(self.config.workers)

        async def one(spec: RunSpec) -> RunResult:
            async with semaphore:
                return await asyncio.to_thread(fn, spec)

        return list(await asyncio.gather(*(one(spec) for spec in specs)))
