"""Per-game scripted walkthroughs for the mock and real full-run bench tests.

Each :class:`Walkthrough` carries the exact call sequence (replayed verbatim
against either a scripted ``MockBackend`` or a live ScummVM), the scripted
backend responses that mirror the real game, and where the game data lives. The
sequences were all reconciled against live captures of the demos.

Monkey Island has its own dedicated tests (``mock_harness.py`` +
``test_full_run_*_monkey.py``); this registry covers the additional games.
"""

import asyncio
import copy
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from fastmcp import Client

from scummvm_bench.backend import MockBackend, ScriptStep
from scummvm_bench.game_paths import game_path as configured_game_path
from scummvm_bench.harness.base import HarnessRunner, RunContext

# walkthroughs -> tests -> scummvm_bench -> scummvm repo root
REPO = Path(__file__).resolve().parents[3]

Call = tuple[str, dict[str, object]]


@dataclass
class Walkthrough:
    """A scripted run of one game for the mock and real bench tests."""

    game_id: str
    save_slot: int
    initial_room: int
    expected_goals: int
    calls: list[Call]
    steps: list[ScriptStep]
    settle_targets: tuple[object, ...] = field(default_factory=tuple)
    initial_inventory: list[str] = field(default_factory=list)
    # Games with non-deterministic dialog drive the live game with a bespoke
    # navigating harness; the mock side always replays the fixed ``calls``.
    real_harness_factory: "Callable[[Walkthrough], HarnessRunner] | None" = None
    dynamic_real: bool = False

    def make_real_harness(self) -> HarnessRunner:
        if self.real_harness_factory is not None:
            return self.real_harness_factory(self)
        return RealWalkthroughHarness(self)

    def backend(self) -> MockBackend:
        """A fresh scripted backend (step consumption state reset)."""
        return MockBackend(
            copy.deepcopy(self.steps),
            initial_room=self.initial_room,
            initial_inventory=list(self.initial_inventory),
        )

    @property
    def expected_calls(self) -> int:
        return len(self.calls)

    def game_path(self) -> str:
        """The machine's data folder for this game, or "" when unconfigured.

        Comes from the non-committed ``game_paths.local.toml`` (or the game's
        env var); the real-run tests skip when it is "" or does not exist.
        """
        return configured_game_path(self.game_id)

    def save_file(self) -> Path:
        return (
            REPO
            / "test"
            / "mcp"
            / "save_slots"
            / self.game_id
            / f"{self.game_id}.s{self.save_slot:02d}"
        )


def _door(name: str, old: int = 0, new: int = 8) -> dict[str, object]:
    return {"objects_changed": [{"name": name, "old_state": old, "new_state": new}]}


def _msgs(*pairs: tuple[str, str]) -> dict[str, object]:
    return {"messages": [{"actor": a, "text": t} for a, t in pairs]}


def _inv(*items: str) -> dict[str, object]:
    return {"inventory_added": list(items)}


def _pickup(name: str) -> "ScriptStep":
    """A pick_up step whose target name equals the inventory item it yields."""
    return ScriptStep("act", {"verb": "pick_up", "target1": name}, _inv(name))


# ---------------------------------------------------------------------------
# Real-ScummVM driver (shared by every game's real test)
# ---------------------------------------------------------------------------


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


class RealWalkthroughHarness:
    """Replays a :class:`Walkthrough` against the live game via the proxy."""

    def __init__(self, walkthrough: Walkthrough) -> None:
        self.walkthrough = walkthrough

    def _settle(self, tool: str, args: dict, result) -> float:
        if args.get("target1") in self.walkthrough.settle_targets:
            return 1.6
        data = getattr(result, "data", None)
        if isinstance(data, dict) and data.get("room_changed") is not None:
            return 1.3
        if tool == "walk":
            return 0.6
        return 0.3

    def run(self, ctx: RunContext) -> str | None:
        async def go() -> None:
            async with Client(ctx.proxy.app) as client:
                for tool, args in self.walkthrough.calls:
                    result = await _call_with_retry(client, tool, args)
                    await asyncio.sleep(self._settle(tool, args, result))

        asyncio.run(go())
        return None
