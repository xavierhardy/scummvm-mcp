"""The recorder: the single chokepoint every proxied tool call passes through.

It owns timing (the clock starts on the *first* call), latches goals (reached
once stays reached), enforces optional call-count / time limits, and produces the
final :class:`RunResult` with a percentage score.
"""

import time
from dataclasses import dataclass

from .goals.engine import GoalEvent, GoalSet
from .models import (
    LIMIT_MAX_CALLS,
    LIMIT_TIME,
    GoalResult,
    RunResult,
    RunSpec,
    ToolCall,
)


@dataclass(frozen=True)
class LimitConfig:
    """Optional budgets for a run (both may be ``None``)."""

    max_calls: int | None = None
    time_limit_s: float | None = None


@dataclass(frozen=True)
class Decision:
    """Whether the run should stop after the call just recorded, and why."""

    stop: bool
    reason: str | None  # "goal" | "max_calls" | "time_limit" | None


class Recorder:
    """Records calls, latches goals, enforces limits, and scores a run."""

    def __init__(self, goal_set: GoalSet, limits: LimitConfig) -> None:
        self.goal_set = goal_set
        self.limits = limits
        self._t0: float | None = None
        self._seq = 0
        self._calls: list[ToolCall] = []
        self._reached: dict[str, int] = {}
        self._stop_reason: str | None = None

    def begin(self) -> None:
        """Start the monotonic clock lazily, on the first call only."""
        if self._t0 is None:
            self._t0 = time.monotonic()

    def elapsed(self) -> float:
        """Seconds since the first call, or ``0.0`` if no call has happened."""
        if self._t0 is None:
            return 0.0
        return time.monotonic() - self._t0

    @property
    def call_count(self) -> int:
        return self._seq

    def record(self, event: GoalEvent, failure: str | None) -> Decision:
        """Record one call, latch any newly satisfied goals, decide on stopping."""
        self.begin()
        self._seq += 1
        seq = self._seq
        t_offset = self.elapsed()
        self._calls.append(
            ToolCall(
                seq=seq,
                tool=event.tool,
                args=dict(event.args),
                result=event.result,
                ok=failure is None,
                failure=failure,
                t_offset_s=t_offset,
            )
        )

        newly_stopping = False
        for goal_id, goal in self.goal_set.goals.items():
            if goal_id in self._reached:
                continue
            if self._safe_predicate(goal.predicate, event):
                self._reached[goal_id] = seq
                if goal.stopping:
                    newly_stopping = True

        decision = self._decide(seq, t_offset, newly_stopping)
        if decision.stop and self._stop_reason is None:
            self._stop_reason = decision.reason
        return decision

    def _decide(self, seq: int, t_offset: float, newly_stopping: bool) -> Decision:
        if newly_stopping:
            return Decision(True, "goal")
        if self.limits.max_calls is not None and seq >= self.limits.max_calls:
            return Decision(True, LIMIT_MAX_CALLS)
        if (
            self.limits.time_limit_s is not None
            and t_offset >= self.limits.time_limit_s
        ):
            return Decision(True, LIMIT_TIME)
        return Decision(False, None)

    @staticmethod
    def _safe_predicate(predicate, event: GoalEvent) -> bool:
        """A faulty predicate must never crash a run."""
        try:
            return bool(predicate(event))
        except Exception:
            return False

    def result(self, spec: RunSpec, error: str | None = None) -> RunResult:
        """Assemble the final :class:`RunResult` for ``spec``."""
        goals = [
            GoalResult(
                goal_id=goal_id,
                label=goal.label,
                reached=goal_id in self._reached,
                stopping=goal.stopping,
                reached_at_call=self._reached.get(goal_id),
            )
            for goal_id, goal in self.goal_set.goals.items()
        ]
        total = len(goals)
        reached = sum(1 for g in goals if g.reached)
        score = 100.0 * reached / total if total else 0.0
        stopped_by_goal = self.goal_set.stopping_goal_id in self._reached
        stopped_by_limit = (
            self._stop_reason
            if self._stop_reason in (LIMIT_MAX_CALLS, LIMIT_TIME)
            else None
        )
        return RunResult(
            spec=spec,
            goals=goals,
            calls=list(self._calls),
            call_count=self._seq,
            elapsed_s=self.elapsed(),
            score_pct=score,
            stopped_by_goal=stopped_by_goal,
            stopped_by_limit=stopped_by_limit,
            error=error,
        )
