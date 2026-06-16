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
    expected_goals=10,
    game_path_env="MANIAC_C64_PATH",
    game_path_default=str(GAMES_DIR / "ManiacMansionDemo/Games/ManiacMansion"),
    calls=[
        ("state", {}),  # 1 state_outside
        ("act", {"verb": "walk_to", "target1": "front_door"}),  # 2 approach
        ("act", {"verb": "pull", "target1": "door mat"}),  # 3 pull_door_mat
        ("act", {"verb": "pick_up", "target1": "key"}),  # 4 pick_up_key
        ("act", {"verb": "use", "target1": "key", "target2": "front_door"}),  # 5
        ("act", {"verb": "walk_to", "target1": "front_door"}),  # 6 enter_mansion (->10)
        # Tour the whole ground floor; interior doors are addressed by object id.
        ("act", {"verb": "open", "target1": 35}),  # kitchen door
        ("act", {"verb": "walk_to", "target1": 35}),  # 7 reach_kitchen (->7)
        ("act", {"verb": "open", "target1": 49}),  # dining door
        ("act", {"verb": "walk_to", "target1": 49}),  # 8 reach_dining_room (->37)
        ("act", {"verb": "open", "target1": 65}),  # pantry door
        ("act", {"verb": "walk_to", "target1": 65}),  # 9 reach_pantry (->36)
        ("act", {"verb": "walk_to", "target1": 91}),  # back to dining (->37)
        ("act", {"verb": "walk_to", "target1": 62}),  # back to kitchen (->7)
        ("act", {"verb": "walk_to", "target1": 48}),  # back to hall (->10)
        ("act", {"verb": "open", "target1": 37}),  # living-room door
        ("act", {"verb": "walk_to", "target1": 37}),  # 10 reach_living_room (->3)
        ("act", {"verb": "open", "target1": 93}),  # library door
        ("act", {"verb": "walk_to", "target1": 93}),  # 11 reach_library (->5) STOP
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
        # Opening interior doors has no observable change; the walk-through enters.
        ScriptStep("act", {"verb": "open", "target1": 35}, {}),
        ScriptStep("act", {"verb": "open", "target1": 49}, {}),
        ScriptStep("act", {"verb": "open", "target1": 65}, {}),
        ScriptStep("act", {"verb": "open", "target1": 37}, {}),
        ScriptStep("act", {"verb": "open", "target1": 93}, {}),
        ScriptStep("act", {"verb": "walk_to", "target1": 35}, {"room_changed": 7}),
        ScriptStep("act", {"verb": "walk_to", "target1": 49}, {"room_changed": 37}),
        ScriptStep("act", {"verb": "walk_to", "target1": 65}, {"room_changed": 36}),
        ScriptStep("act", {"verb": "walk_to", "target1": 91}, {"room_changed": 37}),
        ScriptStep("act", {"verb": "walk_to", "target1": 62}, {"room_changed": 7}),
        ScriptStep("act", {"verb": "walk_to", "target1": 48}, {"room_changed": 10}),
        ScriptStep("act", {"verb": "walk_to", "target1": 37}, {"room_changed": 3}),
        ScriptStep("act", {"verb": "walk_to", "target1": 93}, {"room_changed": 5}),
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
                    # Calling him a failure rattles Wally into dropping his hook.
                    await call("answer", {"id": fail})
                    break
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

            # Escape phase. NOTE: best-effort — the cut-rope / backfire-escape
            # chain is not yet reconciled against a live capture, so the cannon
            # coordinates and the resulting room may need tuning before a real
            # run reaches the stopping goal.
            if stop.is_set():
                return
            await call("act", {"verb": "pick_up", "target1": "plastic_hook"})
            await call("act", {"verb": "pick_up", "target1": "ramrod"})
            # Fire the cannon to sink the four skeleton boats.
            for _ in range(8):
                if stop.is_set():
                    return
                await call("shoot_cannon", {"x": 200, "y": 300})
                await asyncio.sleep(0.4)
            await call(
                "act", {"verb": "use", "target1": "ramrod", "target2": "plastic_hook"}
            )
            await call("act", {"verb": "use", "target1": "gaff", "target2": "debris"})
            await call(
                "act",
                {
                    "verb": "use",
                    "target1": "cutlass",
                    "target2": "cannon_restraint_rope",
                },
            )
            # Fire again — with nothing to hold it down the cannon backfires.
            for _ in range(6):
                if stop.is_set():
                    return
                await call("shoot_cannon", {"x": 320, "y": 215})
                await asyncio.sleep(0.6)


COMI = Walkthrough(
    game_id="comi-demo",
    save_slot=1,
    initial_room=3,
    expected_goals=12,
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
        ("answer", {"id": 5}),  # provoke_failure (Wally drops the hook)
        ("act", {"verb": "pick_up", "target1": "plastic_hook"}),  # get_plastic_hook
        ("act", {"verb": "pick_up", "target1": "ramrod"}),  # get_ramrod
        ("shoot_cannon", {"x": 200, "y": 300}),  # fire_at_boats
        (
            "act",
            {"verb": "use", "target1": "ramrod", "target2": "plastic_hook"},
        ),  # gaff
        ("act", {"verb": "use", "target1": "gaff", "target2": "debris"}),  # cutlass
        # cut_restraint_rope
        (
            "act",
            {"verb": "use", "target1": "cutlass", "target2": "cannon_restraint_rope"},
        ),
        ("shoot_cannon", {"x": 320, "y": 215}),  # escape_to_treasure_room (STOP)
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
            {
                **_msgs(
                    ("guybrush", "You're a failure as a pirate."),
                    ("pirate", "One more peep out of you and I'll do ya in!"),
                ),
                # The outburst makes Wally drop his plastic hook to the floor.
                "objects_changed": [
                    {"name": "plastic_hook", "old_state": 0, "new_state": 1}
                ],
            },
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "plastic_hook"},
            {"inventory_added": ["plastic_hook"]},
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "ramrod"},
            {"inventory_added": ["ramrod"]},
        ),
        ScriptStep(
            "shoot_cannon",
            {},
            {
                "objects_changed": [
                    {"name": "skeleton_boat", "old_state": 0, "new_state": 1}
                ],
                **_msgs(("guybrush", "Got one!")),
            },
        ),
        ScriptStep(
            "act",
            {"verb": "use", "target1": "ramrod", "target2": "plastic_hook"},
            {
                "inventory_added": ["gaff"],
                "inventory_removed": ["ramrod", "plastic_hook"],
            },
        ),
        ScriptStep(
            "act",
            {"verb": "use", "target1": "gaff", "target2": "debris"},
            {
                "inventory_added": ["cutlass", "skeleton_arm"],
                "objects_changed": [{"name": "debris", "old_state": 0, "new_state": 1}],
            },
        ),
        ScriptStep(
            "act",
            {"verb": "use", "target1": "cutlass", "target2": "cannon_restraint_rope"},
            {
                "objects_changed": [
                    {"name": "cannon_restraint_rope", "old_state": 0, "new_state": 1}
                ],
                **_msgs(("guybrush", "Snip.")),
            },
        ),
        # Second shot: nothing restrains the cannon, so it backfires Guybrush
        # through the door into the treasure room (room 4).
        ScriptStep(
            "shoot_cannon",
            {},
            {"room_changed": 4, **_msgs(("guybrush", "Yaaaah!"))},
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
        (
            "act",
            {"verb": "use", "target1": "max_the_object", "target2": 4},
        ),  # talk_to_cat
        ("answer", {"id": 1}),  # ask_cat_commissioner
        ("act", {"verb": "use", "target1": "beat_up_desoto"}),  # use_desoto (->10)
    ],
    steps=[
        ScriptStep("act", {"verb": "use", "target1": 62}, {"room_changed": 8}),
        ScriptStep("act", {"verb": "use", "target1": 82}, {"room_changed": 9}),
        ScriptStep(
            "walk",
            {"x": 50, "y": 180},
            _msgs(
                ("sam", "So, you want a piece of me, huh? Well, take a piece of this!"),
            ),
        ),
        ScriptStep("act", {"verb": "pick_up", "target1": "max"}, {}),
        ScriptStep(
            "act",
            {"verb": "use", "target1": "max_the_object", "target2": 4},
            {
                **_msgs(("max", "Hey there, lil' fella.")),
                "question": {
                    "choices": [
                        {"id": 1, "label": "question"},
                        {"id": 2, "label": "exclamation"},
                        {"id": 3, "label": "tease"},
                        {"id": 4, "label": "bye"},
                    ]
                },
            },
        ),
        ScriptStep(
            "answer",
            {"id": 1},
            _msgs(
                ("sam", "we've got a message from the Commissioner to collect."),
                ("kitten", "I swallowed your orders for safekeeping,"),
            ),
        ),
        ScriptStep(
            "act", {"verb": "use", "target1": "beat_up_desoto"}, {"room_changed": 10}
        ),
    ],
)


# ---------------------------------------------------------------------------
# The Dig demo (save slot 1) — start -> hub -> wreck -> pull wire -> back -> dig
# ---------------------------------------------------------------------------


class DigRealHarness:
    """Drives The Dig demo's full run.

    Two arrival cutscenes lock input for ~30-45s: reaching the wreck exterior
    (the team reacts to the ship) and returning to the starting area after the
    wire is pulled (a small hole opens). The cutscene-gated steps are retried
    patiently until the room actually flips / the dig is accepted, which is more
    patience than the generic harness's fixed retry budget allows.
    """

    def __init__(self, walkthrough: "Walkthrough") -> None:
        self.walkthrough = walkthrough

    def run(self, ctx: RunContext) -> str | None:
        asyncio.run(self._play(ctx))
        return None

    async def _play(self, ctx: RunContext) -> None:
        async with Client(ctx.proxy.app) as client:

            async def act(args: dict) -> dict:
                res = await client.call_tool("act", args)
                return res.data if isinstance(res.data, dict) else {}

            async def room() -> object:
                res = await client.call_tool("state", {})
                s = res.data if isinstance(res.data, dict) else {}
                r = s.get("room") if isinstance(s, dict) else None
                return r.get("id") if isinstance(r, dict) else None

            async def go(obj: int, target: int, tries: int = 60) -> bool:
                """Click obj until the room flips to target. State reads are not
                input-gated, so polling works even mid-cutscene; clicks that land
                during a cutscene simply error and are ignored until it ends."""
                for _ in range(tries):
                    if await room() == target:
                        return True
                    await act({"verb": "interact", "target1": obj})
                    await asyncio.sleep(1.5)
                return await room() == target

            async def act_ok(args: dict, tries: int = 60) -> bool:
                for _ in range(tries):
                    if not (await act(args)).get("error"):
                        return True
                    await asyncio.sleep(1.5)
                return False

            stop = ctx.stop_event
            await client.call_tool("state", {})  # state_start (room 15)
            await go(53, 16)  # leave_to_hub
            await go(67, 18)  # reach_wreck (arrival cutscene follows)
            await go(81, 19)  # enter_wreck (waits out the cutscene)
            await act_ok({"verb": "interact", "target1": 85})  # pull_wire
            await go(84, 18)  # leave_wreck
            await go(80, 16)  # back_to_hub
            await go(66, 15)  # back_to_start (return cutscene; hole opens)
            if stop.is_set():
                return
            # The hole only becomes diggable once the return cutscene ends.
            await act_ok({"verb": "use item", "target1": "trowel", "target2": 54})


DIG = Walkthrough(
    game_id="dig-demo",
    save_slot=1,
    initial_room=15,
    expected_goals=9,
    game_path_env="DIG_DEMO_PATH",
    game_path_default=str(GAMES_DIR / "Dig"),
    initial_inventory=["look_at", "trowel"],
    dynamic_real=True,
    real_harness_factory=lambda wt: DigRealHarness(wt),
    calls=[
        ("state", {}),  # state_start (room 15)
        ("act", {"verb": "interact", "target1": 53}),  # leave_to_hub (->16)
        ("act", {"verb": "interact", "target1": 67}),  # reach_wreck (->18)
        ("act", {"verb": "interact", "target1": 81}),  # enter_wreck (->19)
        ("act", {"verb": "interact", "target1": 85}),  # pull_wire
        ("act", {"verb": "interact", "target1": 84}),  # leave_wreck (->18)
        ("act", {"verb": "interact", "target1": 80}),  # back_to_hub (->16)
        ("act", {"verb": "interact", "target1": 66}),  # back_to_start (->15)
        ("act", {"verb": "use item", "target1": "trowel", "target2": 54}),  # dig_hole
    ],
    steps=[
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 53},
            {"room_changed": 16, **_msgs(("brink", "Where are you going, Low?"))},
        ),
        ScriptStep("act", {"verb": "interact", "target1": 67}, {"room_changed": 18}),
        ScriptStep("act", {"verb": "interact", "target1": 81}, {"room_changed": 19}),
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 85},
            {
                **_msgs(
                    ("low", "Watch out. I'm going to pull this wire down."),
                    ("brink", "Careful, Low."),
                ),
                "objects_changed": [
                    {"name": "hanging wire", "old_state": 0, "new_state": 1}
                ],
            },
        ),
        ScriptStep("act", {"verb": "interact", "target1": 84}, {"room_changed": 18}),
        ScriptStep("act", {"verb": "interact", "target1": 80}, {"room_changed": 16}),
        ScriptStep("act", {"verb": "interact", "target1": 66}, {"room_changed": 15}),
        ScriptStep("act", {"verb": "use item", "target1": "trowel", "target2": 54}, {}),
    ],
)


# ---------------------------------------------------------------------------
# Loom segment of Passport to Adventure (save slot 2) — clearing -> dark tent
# ---------------------------------------------------------------------------


class LoomRealHarness:
    """Drives Loom's pathway navigation: Bobbin's walk is interrupted at
    intermediate stand points, so each pathway needs several ``interact`` clicks
    before the room changes. Retry each transition until the room actually
    flips, then poll state in the new room so the call-based goals latch."""

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

            async def room() -> object:
                s = await call("state", {})
                r = s.get("room") if isinstance(s, dict) else None
                return r.get("id") if isinstance(r, dict) else None

            async def go_pathway(pid: int, target: int, tries: int = 8) -> bool:
                for _ in range(tries):
                    if await room() == target:
                        return True
                    await call("act", {"verb": "interact", "target1": pid})
                    await asyncio.sleep(2.2)
                return await room() == target

            stop = ctx.stop_event
            await call("state", {})  # state_clearing (room 36)
            await call("act", {"verb": "interact", "target1": 461})  # fell_last_leaf
            await asyncio.sleep(1.5)
            await go_pathway(460, 39)  # reach_village
            await call("state", {})  # state_village (room 39)
            if stop.is_set():
                return
            await go_pathway(510, 41)  # reach_crossroads
            await call("state", {})  # state_crossroads (room 41)
            await go_pathway(539, 38)  # reach_dark_clearing (STOP)


LOOM = Walkthrough(
    game_id="pass",
    save_slot=2,
    initial_room=36,
    expected_goals=7,
    game_path_env="PASS_PATH",
    game_path_default=str(GAMES_DIR / "pass"),
    dynamic_real=True,
    real_harness_factory=lambda wt: LoomRealHarness(wt),
    calls=[
        ("state", {}),  # state_clearing (room 36)
        ("act", {"verb": "interact", "target1": 461}),  # fell_last_leaf
        ("act", {"verb": "interact", "target1": 460}),  # reach_village (->39)
        ("state", {}),  # state_village (room 39)
        ("act", {"verb": "interact", "target1": 510}),  # reach_crossroads (->41)
        ("state", {}),  # state_crossroads (room 41)
        ("act", {"verb": "interact", "target1": 539}),  # reach_dark_clearing (->38)
    ],
    steps=[
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 461},
            _msgs(("bobbin", "leaf"), ("bobbin", "Last leaf of the year.")),
        ),
        ScriptStep("act", {"verb": "interact", "target1": 460}, {"room_changed": 39}),
        ScriptStep("act", {"verb": "interact", "target1": 510}, {"room_changed": 41}),
        ScriptStep("act", {"verb": "interact", "target1": 539}, {"room_changed": 38}),
    ],
)


# ---------------------------------------------------------------------------
# Indy3 segment of Passport to Adventure (save slot 3) — the boxing gym fight
# ---------------------------------------------------------------------------


class IndyRealHarness:
    """Drives the Indy3 boxing gym: walk to the locker room to open the spar
    dialog, accept the match, then throw high punches (numpad 9) until the
    three-punch combo goal stops the run. Whether a punch lands is up to the
    coach's blocking, so it just keeps swinging."""

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

            stop = ctx.stop_event
            await state()  # state_gym (room 25)
            # Walk to the locker room until the coach's spar dialog opens.
            for _ in range(10):
                await call("act", {"verb": "walk to", "target1": "locker_room"})
                await asyncio.sleep(1.0)
                if (await state()).get("question"):
                    break
            await call("answer", {"id": 1})  # accept_go_easy + fight_begins
            for _ in range(10):  # let the fight HUD come up
                if (await state()).get("fight"):
                    break
                await asyncio.sleep(0.8)
            for _ in range(6):  # throw_high_punch + land_three_punch_combo (STOP)
                if stop.is_set():
                    return
                await call("keystroke", {"key": "9"})
                await asyncio.sleep(1.5)


INDY3 = Walkthrough(
    game_id="pass",
    save_slot=3,
    initial_room=25,
    expected_goals=7,
    game_path_env="PASS_PATH",
    game_path_default=str(GAMES_DIR / "pass"),
    dynamic_real=True,
    real_harness_factory=lambda wt: IndyRealHarness(wt),
    calls=[
        ("state", {}),  # state_gym (room 25)
        ("act", {"verb": "walk to", "target1": "locker_room"}),  # approach + spar
        ("answer", {"id": 1}),  # accept_go_easy + fight_begins
        ("keystroke", {"key": "9"}),  # throw_high_punch
        ("keystroke", {"key": "9"}),
        ("keystroke", {"key": "9"}),  # land_three_punch_combo (STOP)
    ],
    steps=[
        ScriptStep(
            "act",
            {"verb": "walk to", "target1": "locker_room"},
            {
                "question": {
                    "choices": [
                        {"id": 1, "label": "Go easy on me. I'm a bit out of shape!"},
                        {"id": 2, "label": "Let's have a good workout."},
                        {
                            "id": 3,
                            "label": "Let me have it with everything you've got!",
                        },
                        {"id": 4, "label": "I think I'll pass for now."},
                        {"id": 5, "label": "I'd like to learn how to box."},
                    ]
                },
                **_msgs(
                    ("coach", "Hi, Dr. Jones. How would you like me to spar with you?")
                ),
            },
        ),
        ScriptStep(
            "answer",
            {"id": 1},
            _msgs(
                ("indy", "Go easy on me. I'm a bit out of shape!"),
                ("hud", "Indiana Jones' Health"),
                ("hud", "Boxing Coach's Health"),
            ),
        ),
        ScriptStep("keystroke", {"key": "9"}, {}),
    ],
)


WALKTHROUGHS: dict[str, Walkthrough] = {
    "maniac-c64": MANIAC,
    "comi-demo": COMI,
    "samnmax": SAMNMAX,
    "dig-demo": DIG,
    "loom": LOOM,
    "indy3": INDY3,
}
