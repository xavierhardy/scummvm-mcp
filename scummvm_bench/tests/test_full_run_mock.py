"""The headline benchmark: a mock harness that reaches 100% of all goals.

Drives the real FastMCP proxy + recorder + goal engine through a scripted
backend, asserting exact tool-call counts and per-goal completion. Elapsed time
is only checked as a strictly-positive float (its magnitude is allowed to vary).
"""

from mock_harness import EXPECTED_CALLS, MockHarness, monkey_backend

from scummvm_bench.goals import get_goal_set
from scummvm_bench.models import RunSpec
from scummvm_bench.session import BenchSession


def _run() -> tuple[RunSpec, object]:
    spec = RunSpec("none", "mock", "mock-model", "monkey-ega-demo", 1)
    result = BenchSession().execute(
        spec, backend=monkey_backend(), harness=MockHarness(), serve=False
    )
    return spec, result


def test_full_mock_run_reaches_all_goals() -> None:
    _spec, result = _run()
    goal_set = get_goal_set("monkey-ega-demo", 1)

    assert result.call_count == EXPECTED_CALLS
    assert result.reached_count == goal_set.total()
    assert all(g.reached for g in result.goals)
    assert result.score_pct == 100.0


def test_full_mock_run_stops_on_stopping_goal() -> None:
    _spec, result = _run()
    assert result.stopped_by_goal is True
    assert result.stopped_by_limit is None
    assert result.error is None

    tell = result.goals_by_id["tell_troll_phrase"]
    assert tell.stopping is True
    assert tell.reached is True
    # The stopping goal latches on the very last recorded call.
    assert tell.reached_at_call == result.call_count


def test_full_mock_run_elapsed_is_positive_float() -> None:
    _spec, result = _run()
    assert isinstance(result.elapsed_s, float)
    assert result.elapsed_s > 0.0


def test_full_mock_run_records_no_failures() -> None:
    _spec, result = _run()
    assert result.failure_count == 0
    assert all(call.ok for call in result.calls)


def test_full_mock_run_per_goal_booleans() -> None:
    _spec, result = _run()
    goal_set = get_goal_set("monkey-ega-demo", 1)
    for goal_id in goal_set.goals:
        assert result.goals_by_id[goal_id].reached is True, goal_id


def test_full_mock_run_call_count_matches_sequence_length() -> None:
    _spec, result = _run()
    # 29 goals, reached by the 42-call walkthrough captured from the real demo.
    assert result.call_count == 42
    assert result.total_goals == 29
