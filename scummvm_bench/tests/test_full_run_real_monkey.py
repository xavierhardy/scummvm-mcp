"""Real (non-mock) full bench run against the actual ScummVM Monkey demo.

This launches a real headless ScummVM with the Monkey Island 1 demo and drives
the captured walkthrough through the bench proxy, reaching 100% of the goals.
It is skipped unless the built binary, the game data, and the save slot are all
present (resolve the game path with the ``MONKEY_DEMO_PATH`` env var). The driver
is a deterministic scripted harness — no LLM — so the result is reproducible;
room transitions need a moment to settle, so it retries on transient
"not accepting input" errors like the existing ``test/mcp`` walkthrough does.
"""

import asyncio
import os
from pathlib import Path

import pytest
from fastmcp import Client
from mock_harness import MONKEY_CALLS

from scummvm_bench.goals import get_goal_set
from scummvm_bench.harness.base import RunContext
from scummvm_bench.models import GameSpec, RunSpec
from scummvm_bench.session import BenchSession, SessionConfig

_REPO = Path(__file__).resolve().parents[2]  # tests -> scummvm_bench -> scummvm
SCUMMVM_BIN = _REPO / "scummvm"
GAME_PATH = os.environ.get("MONKEY_DEMO_PATH", str(_REPO.parent / "monkey"))
SAVE_FOLDER = _REPO / "test" / "mcp" / "save_slots"
SAVE_FILE = SAVE_FOLDER / "monkey-ega-demo" / "monkey-ega-demo.s01"

PLANK_TARGET = 307
EXPECTED_OK_CALLS = len(MONKEY_CALLS)  # one successful call per walkthrough step

_missing = []
if not os.access(SCUMMVM_BIN, os.X_OK):
    _missing.append(f"binary {SCUMMVM_BIN}")
if not os.path.isdir(GAME_PATH):
    _missing.append(f"game data {GAME_PATH}")
if not os.path.isfile(SAVE_FILE):
    _missing.append(f"save {SAVE_FILE}")

pytestmark = [
    pytest.mark.real,
    pytest.mark.skipif(
        bool(_missing),
        reason="real Monkey demo unavailable: " + ", ".join(_missing),
    ),
]


async def _call_with_retry(client: Client, tool: str, args: dict, tries: int = 18):
    """Call a proxy tool, retrying while the backend reports a transient error."""
    result = None
    for _ in range(tries):
        result = await client.call_tool(tool, args)
        data = result.data
        if isinstance(data, dict) and data.get("error"):
            await asyncio.sleep(1.0)
            continue
        return result
    return result


def _settle_seconds(tool: str, args: dict, result) -> float:
    if args.get("target1") == PLANK_TARGET:
        return 1.6  # let each plank-bounce script finish
    data = getattr(result, "data", None)
    if isinstance(data, dict) and data.get("room_changed") is not None:
        return 1.3  # room transitions need to settle before the next input
    if tool == "walk":
        return 0.6
    return 0.3


class RealMonkeyWalkthrough:
    """Replays the captured walkthrough against the live game via the proxy."""

    def run(self, ctx: RunContext) -> str | None:
        async def go() -> None:
            async with Client(ctx.proxy.app) as client:
                for tool, args in MONKEY_CALLS:
                    result = await _call_with_retry(client, tool, args)
                    await asyncio.sleep(_settle_seconds(tool, args, result))

        asyncio.run(go())
        return None


def _config() -> SessionConfig:
    return SessionConfig(
        scummvm_binary=str(SCUMMVM_BIN),
        games={"monkey-ega-demo": GameSpec("monkey-ega-demo", GAME_PATH, save_slot=1)},
        save_folder=str(SAVE_FOLDER),
    )


def test_real_monkey_full_run_reaches_all_goals() -> None:
    spec = RunSpec("none", None, None, "monkey-ega-demo", 1)
    result = BenchSession().execute(
        spec, harness=RealMonkeyWalkthrough(), config=_config(), serve=False
    )
    goal_set = get_goal_set("monkey-ega-demo", 1)

    assert result.error is None, result.error
    missing = [g.goal_id for g in result.goals if not g.reached]
    assert not missing, f"unreached goals: {missing}"
    assert result.reached_count == goal_set.total()
    assert result.score_pct == 100.0
    assert result.stopped_by_goal is True
    assert result.goals_by_id["tell_troll_phrase"].reached is True

    # one successful call per walkthrough step (retries add failed calls on top)
    ok_calls = sum(1 for c in result.calls if c.ok)
    assert ok_calls == EXPECTED_OK_CALLS
    assert result.call_count >= EXPECTED_OK_CALLS

    # time is recorded and strictly positive (magnitude may vary)
    assert isinstance(result.elapsed_s, float)
    assert result.elapsed_s > 0.0
