"""Real (non-mock) full-run for every registered game — opt-in via --run-real.

Each launches a real headless ScummVM and drives the captured walkthrough to
100% of the game's goals. A game is skipped individually if its data/save is
absent; point the per-game env var (e.g. ``DIG_DEMO_PATH``) at the data folder.
"""

import os

import pytest
from walkthroughs import REPO, WALKTHROUGHS

from scummvm_bench.goals import get_goal_set
from scummvm_bench.models import GameSpec, RunSpec
from scummvm_bench.session import BenchSession, SessionConfig

SCUMMVM_BIN = REPO / "scummvm"
SAVE_FOLDER = REPO / "test" / "mcp" / "save_slots"

pytestmark = pytest.mark.real


@pytest.mark.parametrize("game_id", sorted(WALKTHROUGHS))
def test_real_full_run(game_id: str) -> None:
    wt = WALKTHROUGHS[game_id]
    if not os.access(SCUMMVM_BIN, os.X_OK):
        pytest.skip(f"scummvm binary missing: {SCUMMVM_BIN}")
    if not os.path.isdir(wt.game_path()):
        pytest.skip(f"{game_id} game data missing: {wt.game_path()}")
    if not wt.save_file().is_file():
        pytest.skip(f"{game_id} save missing: {wt.save_file()}")

    config = SessionConfig(
        scummvm_binary=str(SCUMMVM_BIN),
        games={
            wt.game_id: GameSpec(wt.game_id, wt.game_path(), save_slot=wt.save_slot)
        },
        save_folder=str(SAVE_FOLDER),
    )
    spec = RunSpec("none", None, None, wt.game_id, wt.save_slot)
    result = BenchSession().execute(
        spec, harness=wt.make_real_harness(), config=config, serve=False
    )
    goal_set = get_goal_set(wt.game_id, wt.save_slot)

    assert result.error is None, result.error
    missing = [g.goal_id for g in result.goals if not g.reached]
    assert not missing, f"{game_id} unreached: {missing}"
    assert result.reached_count == goal_set.total()
    assert result.score_pct == 100.0
    assert result.stopped_by_goal is True
    # Real runs retry transient "not accepting input" errors and some call-based
    # goals latch on a rejected call, so floor-check the count (the mock test
    # asserts the exact deterministic call count).
    assert result.call_count >= wt.expected_goals
    assert isinstance(result.elapsed_s, float)
    assert result.elapsed_s > 0.0
