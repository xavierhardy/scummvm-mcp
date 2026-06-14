"""Mock full-run for every registered game walkthrough — each reaches 100%."""

import pytest
from mock_harness import MockHarness
from walkthroughs import WALKTHROUGHS

from scummvm_bench.goals import get_goal_set
from scummvm_bench.models import RunSpec
from scummvm_bench.session import BenchSession


@pytest.mark.parametrize("game_id", sorted(WALKTHROUGHS))
def test_mock_full_run(game_id: str) -> None:
    wt = WALKTHROUGHS[game_id]
    spec = RunSpec("none", "mock", "mock", wt.game_id, wt.save_slot)
    result = BenchSession().execute(
        spec, backend=wt.backend(), harness=MockHarness(wt.calls), serve=False
    )
    goal_set = get_goal_set(wt.game_id, wt.save_slot)

    missing = [g.goal_id for g in result.goals if not g.reached]
    assert not missing, f"{game_id} unreached: {missing}"
    assert goal_set.total() == wt.expected_goals
    assert result.reached_count == goal_set.total()
    assert result.score_pct == 100.0
    assert result.call_count == wt.expected_calls
    assert result.stopped_by_goal is True
    assert result.failure_count == 0
    assert isinstance(result.elapsed_s, float)
    assert result.elapsed_s > 0.0
