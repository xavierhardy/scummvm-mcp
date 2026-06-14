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
import os
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from fastmcp import Client

from scummvm_bench.backend import MockBackend, ScriptStep
from scummvm_bench.harness.base import HarnessRunner, RunContext

REPO = Path(__file__).resolve().parents[2]  # tests -> scummvm_bench -> scummvm
GAMES_DIR = REPO.parent.parent / "games"  # .../llm/scummvm/games

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
    game_path_env: str
    game_path_default: str
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
        return os.environ.get(self.game_path_env, self.game_path_default)

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


# ---------------------------------------------------------------------------
# Maniac Mansion C64 demo (save slot 1) — get into the mansion
# ---------------------------------------------------------------------------

MANIAC = Walkthrough(
    game_id="maniac-c64",
    save_slot=1,
    initial_room=1,
    expected_goals=6,
    game_path_env="MANIAC_C64_PATH",
    game_path_default=str(GAMES_DIR / "ManiacMansionDemo/Games/ManiacMansion"),
    calls=[
        ("state", {}),  # 1 state_outside
        ("act", {"verb": "walk_to", "target1": "front_door"}),  # 2 approach
        ("act", {"verb": "pull", "target1": "door mat"}),  # 3 pull_door_mat
        ("act", {"verb": "pick_up", "target1": "key"}),  # 4 pick_up_key
        ("act", {"verb": "use", "target1": "key", "target2": "front_door"}),  # 5
        ("act", {"verb": "walk_to", "target1": "front_door"}),  # 6 enter_mansion (->10)
        ("state", {}),  # 7 state_inside (STOPPING)
    ],
    steps=[
        # first walk_to positions the kid; the second (after unlocking) enters.
        ScriptStep("act", {"verb": "walk_to", "target1": "front_door"}, {}),
        ScriptStep(
            "act", {"verb": "walk_to", "target1": "front_door"}, {"room_changed": 10}
        ),
        ScriptStep(
            "act", {"verb": "pull", "target1": "door mat"}, _door("door mat", 0, 8)
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "key"},
            {
                "inventory_added": ["key"],
                **_door("key", 0, 10),
            },
        ),
        ScriptStep(
            "act",
            {
                "verb": "use",
                "target1": "key",
                "target2": "front_door",
            },
            _door("front door", 4, 8),
        ),
    ],
)


# ---------------------------------------------------------------------------
# Curse of Monkey Island demo (save slot 1) — the cannon-room dialog
# ---------------------------------------------------------------------------


def _msg_has(data: object, needle: str) -> bool:
    if not isinstance(data, dict):
        return False
    return any(
        needle.lower() in (m.get("text", "").lower())
        for m in data.get("messages", [])
        if isinstance(m, dict)
    )


class ComiRealHarness:
    """Navigates COMI's non-deterministic pirate dialog to reach every goal."""

    PIRATE = {"verb": "talk_to", "target1": "small_pirate"}

    def __init__(self, walkthrough: "Walkthrough") -> None:
        self.walkthrough = walkthrough

    def run(self, ctx: RunContext) -> str | None:
        asyncio.run(self._play(ctx))
        return None

    async def _play(self, ctx: RunContext) -> None:
        async with Client(ctx.proxy.app) as client:

            async def call(tool: str, args: dict, tries: int = 14) -> dict:
                data: dict = {}
                for _ in range(tries):
                    res = await client.call_tool(tool, args)
                    data = res.data if isinstance(res.data, dict) else {}
                    if data.get("error"):
                        await asyncio.sleep(0.8)
                        continue
                    return data
                return data

            async def question() -> dict | None:
                state = await call("state", {})
                q = state.get("question")
                return q if isinstance(q, dict) else None

            def pick(q: dict, *kw: str) -> int | None:
                for choice in q["choices"]:
                    if any(k in choice["label"].lower() for k in kw):
                        return choice["id"]
                return None

            async def ensure_dialog() -> dict | None:
                q = await question()
                if not q:
                    await call("act", self.PIRATE)
                    q = await question()
                return q

            stop = ctx.stop_event
            await call("state", {})  # state_cannon_scene
            await call("act", self.PIRATE)  # talk_to_pirate
            # Needle the pirate about his features until he reveals he is Wally.
            for _ in range(16):
                if stop.is_set():
                    return
                q = await ensure_dialog()
                if not q:
                    break
                cid = (
                    pick(q, "beard", "eyepatch", "hook", "tough")
                    or q["choices"][0]["id"]
                )
                if _msg_has(await call("answer", {"id": cid}), "wally"):
                    break
            # Take the leaflet, then exhaust sales topics until the insult appears.
            for _ in range(18):
                if stop.is_set():
                    return
                q = await ensure_dialog()
                if not q:
                    break
                lit = pick(q, "literature")
                if lit is not None:
                    await call("answer", {"id": lit})
                    continue
                fail = pick(q, "failure")
                if fail is not None:
                    # Calling him a failure makes the pirate threaten ("...do ya in!").
                    await call("answer", {"id": fail})
                    return
                sid = (
                    pick(
                        q,
                        "seminar",
                        "lecture",
                        "audio",
                        "more",
                        "set me free",
                        "snap out",
                    )
                    or q["choices"][0]["id"]
                )
                await call("answer", {"id": sid})


COMI = Walkthrough(
    game_id="comi-demo",
    save_slot=1,
    initial_room=3,
    expected_goals=6,
    game_path_env="COMI_DEMO_PATH",
    game_path_default=str(GAMES_DIR / "COMIDEMO"),
    initial_inventory=["helium_balloons"],
    dynamic_real=True,
    real_harness_factory=lambda wt: ComiRealHarness(wt),
    calls=[
        ("state", {}),  # state_cannon_scene
        ("act", {"verb": "talk_to", "target1": "small_pirate"}),  # talk_to_pirate
        ("answer", {"id": 2}),  # recognize_wally
        ("answer", {"id": 4}),  # get_leaflet
        ("answer", {"id": 5}),  # call_pirate_failure + pirate_threatens (STOP)
    ],
    steps=[
        ScriptStep(
            "act",
            {"verb": "talk_to", "target1": "small_pirate"},
            {
                **_msgs(("pirate", "Stand yer distance!")),
                "question": {"choices": [{"id": 1, "label": "who are you?"}]},
            },
        ),
        ScriptStep(
            "answer",
            {"id": 2},
            {
                **_msgs(
                    ("pirate", "Wait a minute! You're not a pirate!"),
                    ("pirate", "Wally!"),
                    ("guybrush", "It's Guybrush Threepwood!"),
                ),
                "question": {"choices": [{"id": 4, "label": "literature"}]},
            },
        ),
        ScriptStep(
            "answer",
            {"id": 4},
            {
                "inventory_added": ["pirate_literature"],
                **_msgs(
                    ("pirate", "This leaflet explains the basic philosophy I follow.")
                ),
                "question": {"choices": [{"id": 5, "label": "failure"}]},
            },
        ),
        ScriptStep(
            "answer",
            {"id": 5},
            _msgs(
                ("guybrush", "You're a failure as a pirate."),
                ("pirate", "Shut yer trap, ya yellow-bellied blowfish!"),
                ("pirate", "One more peep out of you and I'll do ya in!"),
            ),
        ),
    ],
)


# ---------------------------------------------------------------------------
# Sam & Max Hit the Road demo (save slot 1) — office -> street -> DeSoto
# ---------------------------------------------------------------------------


class SamnmaxRealHarness:
    """Drives Sam & Max: the room transitions and the Max-on-cat step both need
    nudging/retries, so navigate the live game dynamically."""

    def __init__(self, walkthrough: "Walkthrough") -> None:
        self.walkthrough = walkthrough

    def run(self, ctx: RunContext) -> str | None:
        asyncio.run(self._play(ctx))
        return None

    async def _play(self, ctx: RunContext) -> None:
        async with Client(ctx.proxy.app) as client:

            async def call(tool: str, args: dict, tries: int = 12) -> dict:
                data: dict = {}
                for _ in range(tries):
                    res = await client.call_tool(tool, args)
                    data = res.data if isinstance(res.data, dict) else {}
                    if data.get("error"):
                        await asyncio.sleep(0.8)
                        continue
                    return data
                return data

            async def state() -> dict:
                s = await call("state", {})
                return s if isinstance(s, dict) else {}

            async def room() -> object:
                r = (await state()).get("room")
                return r.get("id") if isinstance(r, dict) else None

            async def wait_room(target: int, tries: int = 20) -> bool:
                for _ in range(tries):
                    if await room() == target:
                        return True
                    await asyncio.sleep(0.6)
                return False

            stop = ctx.stop_event
            await state()  # state_office (room 7)
            await call("act", {"verb": "use", "target1": 62})  # -> staircase 8
            await wait_room(8)
            await state()  # state_staircase
            # The brawl cutscene fires from the bottom-left, which is also the exit.
            for _ in range(6):
                await call("walk", {"x": 50, "y": 180})  # trigger_cutscene
                await asyncio.sleep(1.0)
                await call("act", {"verb": "use", "target1": 82})  # -> street 9
                if await wait_room(9, tries=6):
                    break
            await state()  # state_street
            # Hold Max and use him on the kitten until its topic dialog opens.
            for _ in range(10):
                if stop.is_set():
                    return
                await call("act", {"verb": "pick_up", "target1": "max"})
                await asyncio.sleep(0.8)
                data = await call(
                    "act", {"verb": "use", "target1": "max_the_object", "target2": 4}
                )
                if data.get("question") or (await state()).get("question"):
                    break
                await asyncio.sleep(1.2)
            q = (await state()).get("question")
            if isinstance(q, dict):
                qid = next(
                    (c["id"] for c in q["choices"] if c["label"] == "question"),
                    q["choices"][0]["id"],
                )
                await call("answer", {"id": qid})  # -> Commissioner reveal
            await call("act", {"verb": "use", "target1": "beat_up_desoto"})  # drive off
            await wait_room(10)


SAMNMAX = Walkthrough(
    game_id="samnmax",
    save_slot=1,
    initial_room=7,
    expected_goals=9,
    game_path_env="SAMNMAX_DEMO_PATH",
    game_path_default=str(GAMES_DIR / "samnmax-dos-demo-en"),
    initial_inventory=["max_the_object"],
    dynamic_real=True,
    real_harness_factory=lambda wt: SamnmaxRealHarness(wt),
    calls=[
        ("state", {}),  # state_office
        ("act", {"verb": "use", "target1": 62}),  # reach_staircase (->8)
        ("state", {}),  # state_staircase
        ("walk", {"x": 50, "y": 180}),  # trigger_cutscene
        ("act", {"verb": "use", "target1": 82}),  # reach_street (->9)
        ("state", {}),  # state_street
        ("act", {"verb": "pick_up", "target1": "max"}),
        ("act", {"verb": "use", "target1": "max_the_object", "target2": 4}),  # talk_to_cat
        ("answer", {"id": 1}),  # ask_cat_commissioner
        ("act", {"verb": "use", "target1": "beat_up_desoto"}),  # use_desoto (->10)
    ],
    steps=[
        ScriptStep("act", {"verb": "use", "target1": 62}, {"room_changed": 8}),
        ScriptStep("act", {"verb": "use", "target1": 82}, {"room_changed": 9}),
        ScriptStep("walk", {"x": 50, "y": 180}, _msgs(
            ("sam", "So, you want a piece of me, huh? Well, take a piece of this!"),
        )),
        ScriptStep("act", {"verb": "pick_up", "target1": "max"}, {}),
        ScriptStep("act", {"verb": "use", "target1": "max_the_object", "target2": 4}, {
            **_msgs(("max", "Hey there, lil' fella.")),
            "question": {
                "choices": [
                    {"id": 1, "label": "question"},
                    {"id": 2, "label": "exclamation"},
                    {"id": 3, "label": "tease"},
                    {"id": 4, "label": "bye"},
                ]
            },
        }),
        ScriptStep("answer", {"id": 1}, _msgs(
            ("sam", "we've got a message from the Commissioner to collect."),
            ("kitten", "I swallowed your orders for safekeeping,"),
        )),
        ScriptStep("act", {"verb": "use", "target1": "beat_up_desoto"}, {"room_changed": 10}),
    ],
)


WALKTHROUGHS: dict[str, Walkthrough] = {
    MANIAC.game_id: MANIAC,
    COMI.game_id: COMI,
    SAMNMAX.game_id: SAMNMAX,
}
