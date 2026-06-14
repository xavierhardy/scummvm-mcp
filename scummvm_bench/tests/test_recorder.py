"""Recorder timing, limit enforcement, and permanent goal latching."""

from scummvm_bench.goals.engine import Goal, GoalEvent, GoalSet, on_call
from scummvm_bench.models import RunSpec
from scummvm_bench.recorder import LimitConfig, Recorder

SPEC = RunSpec("none", None, None, "game", None)


def small_goalset() -> GoalSet:
    return GoalSet(
        "game",
        None,
        {
            "walked": Goal("walked", "Walked", on_call("walk"), kind="call"),
            "done": Goal("done", "Done", on_call("done"), stopping=True, kind="call"),
        },
    )


def _event(tool: str) -> GoalEvent:
    return GoalEvent(tool=tool, args={}, result={}, state_before={}, ok=True)


def test_timer_starts_on_first_call() -> None:
    rec = Recorder(small_goalset(), LimitConfig())
    assert rec.elapsed() == 0.0
    rec.record(_event("look"), None)
    assert rec.elapsed() > 0.0


def test_goal_latches_permanently() -> None:
    rec = Recorder(small_goalset(), LimitConfig())
    rec.record(_event("walk"), None)  # latches "walked" at call 1
    rec.record(_event("look"), None)  # unrelated; must not un-latch
    result = rec.result(SPEC)
    walked = result.goals_by_id["walked"]
    assert walked.reached is True
    assert walked.reached_at_call == 1


def test_max_calls_limit_stops_after_capping_call() -> None:
    rec = Recorder(small_goalset(), LimitConfig(max_calls=2))
    assert rec.record(_event("look"), None).stop is False
    decision = rec.record(_event("look"), None)
    assert decision.stop is True
    assert decision.reason == "max_calls"
    result = rec.result(SPEC)
    assert result.call_count == 2  # the capping call is still recorded
    assert result.stopped_by_limit == "max_calls"
    assert result.stopped_by_goal is False


def test_time_limit_stops() -> None:
    rec = Recorder(small_goalset(), LimitConfig(time_limit_s=0.0))
    decision = rec.record(_event("look"), None)
    assert decision.stop is True
    assert decision.reason == "time_limit"
    assert rec.result(SPEC).stopped_by_limit == "time_limit"


def test_stopping_goal_takes_precedence_over_limits() -> None:
    rec = Recorder(small_goalset(), LimitConfig(max_calls=1))
    decision = rec.record(_event("done"), None)
    assert decision.stop is True
    assert decision.reason == "goal"
    result = rec.result(SPEC)
    assert result.stopped_by_goal is True
    assert result.stopped_by_limit is None


def test_failures_are_recorded() -> None:
    rec = Recorder(small_goalset(), LimitConfig())
    rec.record(GoalEvent("act", {"verb": "x"}, None, {}, False), "invalid_request")
    result = rec.result(SPEC)
    assert result.failure_count == 1
    last = result.calls[-1]
    assert last.ok is False
    assert last.failure == "invalid_request"


def test_score_is_percentage_of_reached_goals() -> None:
    rec = Recorder(small_goalset(), LimitConfig())
    rec.record(_event("walk"), None)  # 1 of 2 goals
    result = rec.result(SPEC)
    assert result.score_pct == 50.0


def test_goal_times_latches_on_nth_occurrence() -> None:
    goal_set = GoalSet(
        "game",
        None,
        {
            "first": Goal("first", "First", on_call("walk"), kind="call", times=1),
            "third": Goal("third", "Third", on_call("walk"), kind="call", times=3),
            "done": Goal("done", "Done", on_call("done"), stopping=True),
        },
    )
    rec = Recorder(goal_set, LimitConfig())
    rec.record(_event("walk"), None)  # 1st
    result = rec.result(SPEC)
    assert result.goals_by_id["first"].reached is True
    assert result.goals_by_id["third"].reached is False

    rec.record(_event("walk"), None)  # 2nd
    rec.record(_event("walk"), None)  # 3rd
    result = rec.result(SPEC)
    assert result.goals_by_id["third"].reached is True
    assert result.goals_by_id["third"].reached_at_call == 3


def test_no_calls_means_zero_elapsed() -> None:
    rec = Recorder(small_goalset(), LimitConfig())
    result = rec.result(SPEC)
    assert result.elapsed_s == 0.0
    assert result.call_count == 0
    assert result.score_pct == 0.0
