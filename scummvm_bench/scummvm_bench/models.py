"""Shared, picklable data structures used across the bench.

Only plain dataclasses live here so that ``RunSpec`` / ``RunResult`` can cross a
``ProcessPoolExecutor`` boundary unchanged. Predicates and live objects never
appear here.
"""

from dataclasses import dataclass, field

# Allowed values for ``ToolCall.failure`` (None means the call succeeded).
FAILURE_INVALID_REQUEST = "invalid_request"
FAILURE_INVALID_RESPONSE = "invalid_response"
FAILURE_BACKEND_ERROR = "backend_error"

# Allowed values for ``RunResult.stopped_by_limit``.
LIMIT_MAX_CALLS = "max_calls"
LIMIT_TIME = "time_limit"


@dataclass(frozen=True)
class RunSpec:
    """One cell of the (model x harness x game x save-state) matrix."""

    harness: str
    provider: str | None
    model: str | None
    game_id: str
    save_slot: int | None
    max_calls: int | None = None
    time_limit_s: float | None = None
    local: bool = False

    @property
    def label(self) -> str:
        save = "-" if self.save_slot is None else str(self.save_slot)
        model = self.model or "-"
        return f"{self.harness}/{model}/{self.game_id}/{save}"


@dataclass
class ToolCall:
    """A single MCP tool call as seen by the proxy."""

    seq: int
    tool: str
    args: dict[str, object]
    result: dict[str, object] | None
    ok: bool
    failure: str | None
    t_offset_s: float


@dataclass
class GoalResult:
    """The outcome of one goal for a run."""

    goal_id: str
    label: str
    reached: bool
    stopping: bool
    reached_at_call: int | None


@dataclass
class RunResult:
    """The complete result of running one ``RunSpec``."""

    spec: RunSpec
    goals: list[GoalResult]
    calls: list[ToolCall]
    call_count: int
    elapsed_s: float
    score_pct: float
    stopped_by_goal: bool
    stopped_by_limit: str | None
    error: str | None = None
    transcript_path: str | None = None

    @property
    def total_goals(self) -> int:
        return len(self.goals)

    @property
    def reached_count(self) -> int:
        return sum(1 for g in self.goals if g.reached)

    @property
    def goals_by_id(self) -> dict[str, GoalResult]:
        return {g.goal_id: g for g in self.goals}

    @property
    def failure_count(self) -> int:
        return sum(1 for c in self.calls if not c.ok)


@dataclass
class GameSpec:
    """Static description of a game/save the bench knows how to launch."""

    game_id: str
    game_path: str
    save_slot: int | None = None
    ini_template: str | None = None


@dataclass
class LocalModelSpec:
    """An LM Studio model the bench can download/load/unload/delete."""

    key: str
    provider: str = "local"
    model: str = ""
    context_length: int | None = None
    gpu: str | None = None
    ttl: int | None = None
    base_url: str = "http://localhost:1234/v1"
    api_key: str = "lm-studio"
    extra_load_args: list[str] = field(default_factory=list)
