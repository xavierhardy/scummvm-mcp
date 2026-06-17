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
            # Talk to the kitten (the cat courier) until its topic dialog opens.
            for _ in range(10):
                if stop.is_set():
                    return
                data = await call("act", {"verb": "talk_to", "target1": 4})
                if data.get("question") or (await state()).get("question"):
                    break
                await asyncio.sleep(1.2)
            # Ask the "question" topic — the kitten admits it swallowed the orders.
            q = (await state()).get("question")
            if isinstance(q, dict):
                qid = next(
                    (c["id"] for c in q["choices"] if c["label"] == "question"),
                    q["choices"][0]["id"],
                )
                await call("answer", {"id": qid})  # -> Commissioner reveal
            # Use Max on the kitten to shake the swallowed orders back up: this
            # adds the carnival tickets to the inventory.
            for _ in range(10):
                if stop.is_set():
                    return
                data = await call("act", {"verb": "use", "target1": "max", "target2": 4})
                inv = (await state()).get("inventory") or []
                if "carnival_tickets" in inv or "carnival_tickets" in (
                    data.get("inventory_added") or []
                ):
                    break
                await asyncio.sleep(1.2)
            # Board the DeSoto, which drives off (room 10). The use-Max cutscene
            # just played, so retry the boarding until the room actually flips.
            for _ in range(8):
                if stop.is_set():
                    return
                await call("act", {"verb": "use", "target1": "beat_up_desoto"})
                if await wait_room(10, tries=8):
                    break
                await asyncio.sleep(1.0)


SAMNMAX = Walkthrough(
    game_id="samnmax",
    save_slot=1,
    initial_room=7,
    expected_goals=10,
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
        ("act", {"verb": "talk_to", "target1": 4}),  # talk_to_cat
        ("answer", {"id": 1}),  # ask_cat_commissioner
        (
            "act",
            {"verb": "use", "target1": "max", "target2": 4},
        ),  # use_max_on_cat (carnival_tickets)
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
        ScriptStep(
            "act",
            {"verb": "talk_to", "target1": 4},
            {
                **_msgs(
                    ("max", "Hey there, lil' fella."),
                    ("kitten", "You talkin' to me?"),
                ),
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
            "act",
            {"verb": "use", "target1": "max", "target2": 4},
            {
                **_msgs(
                    ("max", "Ooh, that gives me an idea!"),
                    (
                        "sam",
                        "According to these orders, something bizarre is "
                        "happening at the carnival.",
                    ),
                ),
                "inventory_added": ["carnival_tickets"],
                "objects_changed": [
                    {"name": "carnival tickets", "old_state": 0, "new_state": 1}
                ],
            },
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
    """Drives the whole Indy (Last Crusade) demo, reconciled against a live
    capture. Control starts in the gym (room 25) after the intro cutscene; the
    harness then: crosses the corridor, goes outside (sitting through the ~30s
    Donovan cutscene), travels to Henry's house and ransacks it (plant / pull
    cloth -> chest / pull bookcase -> sticky tape); returns through corridor door
    103 into the office, calms the student mob (which opens Indy's office), takes
    the mail + package (grail diary) and uses the sticky tape on the solvent jar
    for the small key; travels back to Henry's, opens the chest for the old book
    and picks up the painting (which unlocks the Venice trip); then travels from
    outside to Venice.

    The "talk about this calmly" student line is never offered by this build, so
    it is not a goal (see the goal-set notes).
    """

    def __init__(self, walkthrough: "Walkthrough") -> None:
        self.walkthrough = walkthrough

    def run(self, ctx: RunContext) -> str | None:
        asyncio.run(self._play(ctx))
        return None

    async def _play(self, ctx: RunContext) -> None:
        async with Client(ctx.proxy.app) as client:

            async def call(tool: str, args: dict, tries: int = 15) -> dict:
                # Retries cover transient "not accepting input" while scripts /
                # the mob-banging cutscene hold input.
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

            async def go(target, dest: int, opens: bool = False) -> bool:
                # Walk to a door/exit until the room flips to ``dest``.
                if opens:
                    await call("act", {"verb": "open", "target1": target})
                for _ in range(12):
                    if stop.is_set():
                        return False
                    r = await call("act", {"verb": "walk to", "target1": target})
                    if r.get("room_changed") == dest or await room() == dest:
                        return True
                    await asyncio.sleep(0.5)
                return await room() == dest

            stop = ctx.stop_event

            async def travel_to(keyword: str, dest: int) -> bool:
                for _ in range(8):
                    if stop.is_set():
                        return False
                    r = await call("act", {"verb": "travel"})
                    q = r.get("question") or (await state()).get("question")
                    if isinstance(q, dict):
                        cid = next(
                            (c["id"] for c in q["choices"] if keyword in c["label"].lower()),
                            None,
                        )
                        if cid is None:
                            return False  # destination not offered
                        await call("answer", {"id": cid})
                    if await wait_room(dest, tries=8):
                        return True
                return await room() == dest

            async def settle_outside() -> None:
                # The first time we reach the outside (here, climbing out of the
                # office window) ScummVM plays a ~30s Donovan cutscene (room 29,
                # input locked). Wait it out; if it never starts, return as soon
                # as we're stable in room 24 with the travel verb available.
                saw = False
                for i in range(70):
                    if stop.is_set():
                        return
                    s = await state()
                    rid = (s.get("room") or {}).get("id")
                    if rid == 29:
                        saw = True
                    elif rid == 24 and "travel" in (s.get("verbs") or []) and (saw or i >= 6):
                        return
                    await asyncio.sleep(0.5)

            await state()  # state_first_room (gym, room 25)
            await go(213, 20)  # gym -> corridor
            await state()  # state_corridor (20)
            # Faster route: go straight to the office and never touch the left
            # door -- we reach the outside via the office window instead, which
            # saves the back-and-forth corridor trips. Take door 103 (next to the
            # gym) straight into the office.
            await call("act", {"verb": "open", "target1": 103})  # open_door_gym
            await go(103, 22)  # reach_office_via_103 (room 22)
            await state()  # state_office (22)
            # Calm the student mob -- unavoidable (the window only saves the
            # *return* corridor trips, not this first entry). Pick the diplomatic
            # lines; "take down names" resolves it and opens Indy's office.
            prefer = ("work something out", "calmly", "take it easy", "fair for everyone")
            resolve = "take down names"
            await call("act", {"verb": "talk to", "target1": "students"})
            for _ in range(8):
                if stop.is_set():
                    return
                q = (await state()).get("question")
                if not isinstance(q, dict):
                    break
                cid = next(
                    (c["id"] for c in q["choices"] if any(k in c["label"].lower() for k in prefer)),
                    None,
                ) or next(
                    (c["id"] for c in q["choices"] if resolve in c["label"].lower()), None
                )
                if cid is None:
                    break
                await call("answer", {"id": cid})
                await asyncio.sleep(0.6)
            await wait_room(21, tries=8)
            await state()  # state_indy_office (21)
            # Mail chain -> grail diary first (the small key needs the sticky tape,
            # still in Henry's house, so that waits for the second office trip).
            for item in ("junk_mail", "letters", "papers", "package"):
                await call("act", {"verb": "pick_up", "target1": item})
            await call("act", {"verb": "open", "target1": "package"})  # grail diary
            # Open the window only once we have the diary, then climb out to the
            # outside (first time -> Donovan cutscene).
            await call("act", {"verb": "open", "target1": "window"})  # open_window
            await go("window", 24)  # reach_outside (24)
            await settle_outside()
            await state()  # state_outside (24)
            # Henry's house #1: grab the painting and pull the bookcase for the
            # sticky tape. LEAVE the plant and the cloth -- the cloth can't be
            # pulled until the plant is moved, and that only happens on the second
            # trip (once we have the key).
            await travel_to("henry", 27)  # travel_henry (27)
            await call("act", {"verb": "pick_up", "target1": "painting"})
            await call("act", {"verb": "pull", "target1": "bookcase"})
            await call("act", {"verb": "pick_up", "target1": "sticky_tape"})
            # Back to Indy's office through the WINDOW (no corridor / mob this
            # time): use the sticky tape on the solvent jar to get the small key.
            await go(231, 24)  # Henry's -> outside
            await go("window", 21)  # outside -> Indy's office through the window
            await call("act", {"verb": "use", "target1": "sticky_tape", "target2": "jar"})  # small_key
            await go("window", 24)  # back outside through the window
            # Henry's house #2 (last trip): in order -- move the plant, then pull
            # the table cloth (only possible after the plant moves) to reveal the
            # chest, then open it for the old book. The painting is already in
            # hand, so taking the old book gives Indy everything for the Grail
            # quest and the Venice trip unlocks back outside.
            await travel_to("henry", 27)
            await state()  # state_henry (27)
            await call("act", {"verb": "pick_up", "target1": "plant"})  # plant_moved (after the key)
            await call("act", {"verb": "pull", "target1": "table_cloth"})  # cloth_pulled (needs plant moved)
            await call("act", {"verb": "use", "target1": "small_key", "target2": "chest"})
            await call("act", {"verb": "pick_up", "target1": "old_book"})
            await go(231, 24)
            await travel_to("venice", 28)  # travel_venice (STOP)


# The whole-demo arc is deterministic in the mock backend (canned responses),
# so the mock self-test exercises all 30 goals end to end, mirroring the faster
# route (office first, then the window as the outside<->office shortcut). The
# room ids (gym 25, corridor 20, outside 24, Henry's house 27) are reconciled
# against a live capture; office 22 / Indy's office 21 are the college offices.
_INDY_OFFICE_LINES = _msgs(
    ("indy", "Just a moment, folks. I'm sure we can work something out."),
    ("indy", "Please relax. I have a solution that is fair for everyone."),
    ("indy", "Irene, take down names and I will see everyone in order."),
)


def _objc(name: str) -> dict:
    return {"objects_changed": [{"name": name, "old_state": 0, "new_state": 1}]}


INDY3 = Walkthrough(
    game_id="pass",
    save_slot=3,
    initial_room=25,
    expected_goals=30,
    game_path_env="PASS_PATH",
    game_path_default=str(GAMES_DIR / "pass"),
    dynamic_real=True,
    real_harness_factory=lambda wt: IndyRealHarness(wt),
    calls=[
        ("state", {}),  # 1 state_first_room (gym 25)
        ("act", {"verb": "walk to", "target1": "door"}),  # reach_corridor (->20)
        ("state", {}),  # state_corridor (20)
        # Faster route: straight to the office via door 103 (no left door / no
        # going outside first).
        ("act", {"verb": "open", "target1": 103}),  # open_door_gym
        ("act", {"verb": "walk to", "target1": 103}),  # reach_office_via_103 (->22)
        ("state", {}),  # state_office (22)
        ("act", {"verb": "talk to", "target1": "students"}),  # talk_to_students + 3 lines (->21)
        ("state", {}),  # state_indy_office (21)
        # Mail chain -> grail diary, then open the window (only after the diary).
        ("act", {"verb": "pick_up", "target1": "junk_mail"}),
        ("act", {"verb": "pick_up", "target1": "letters"}),
        ("act", {"verb": "pick_up", "target1": "papers"}),
        ("act", {"verb": "pick_up", "target1": "package"}),
        ("act", {"verb": "open", "target1": "package"}),  # open_package (grail diary)
        ("act", {"verb": "open", "target1": "window"}),  # open_window
        ("act", {"verb": "walk to", "target1": "window"}),  # reach_outside (window ->24)
        ("state", {}),  # state_outside (24)
        # Henry's #1: painting + sticky tape (leave the plant AND the cloth).
        ("act", {"verb": "travel", "target1": "henry"}),  # travel_henry (->27)
        ("act", {"verb": "pick_up", "target1": "painting"}),  # pick_up_painting
        ("act", {"verb": "pull", "target1": "bookcase"}),  # bookcase_pulled
        ("act", {"verb": "pick_up", "target1": "sticky_tape"}),  # pick_up_sticky_tape
        # Back to the office through the window for the small key.
        ("act", {"verb": "walk to", "target1": 231}),  # nav Henry's -> outside (->24)
        ("act", {"verb": "walk to", "target1": "window"}),  # nav outside -> office (window ->21)
        ("act", {"verb": "use", "target1": "sticky_tape", "target2": "jar"}),  # obtain_small_key
        ("act", {"verb": "walk to", "target1": "window"}),  # nav office -> outside (window ->24)
        # Henry's #2 (last trip), in order: move the plant, then pull the cloth
        # (only possible after the plant), then open the chest.
        ("act", {"verb": "travel", "target1": "henry"}),  # nav -> Henry's (->27)
        ("state", {}),  # state_henry (27)
        ("act", {"verb": "pick_up", "target1": "plant"}),  # plant_moved (after the key)
        ("act", {"verb": "pull", "target1": "table_cloth"}),  # cloth_pulled (needs plant moved)
        ("act", {"verb": "use", "target1": "small_key", "target2": "chest"}),  # use_key_on_chest
        ("act", {"verb": "pick_up", "target1": "old_book"}),  # pick_up_old_book (last Grail item)
        ("act", {"verb": "walk to", "target1": 231}),  # nav Henry's -> outside (->24)
        ("act", {"verb": "travel", "target1": "venice"}),  # travel_venice (STOP)
    ],
    steps=[
        ScriptStep("act", {"verb": "walk to", "target1": "door"}, {"room_changed": 20}),
        ScriptStep("act", {"verb": "open", "target1": 103}, {}),
        # Door 103 (the one "next to the gym") leads into the office (22); the
        # student dialog then disperses the mob straight into Indy's office (21).
        ScriptStep("act", {"verb": "walk to", "target1": 103}, {"room_changed": 22}),
        ScriptStep(
            "act", {"verb": "talk to", "target1": "students"},
            {**dict(_INDY_OFFICE_LINES), "room_changed": 21},
        ),
        ScriptStep("act", {"verb": "pick_up", "target1": "junk_mail"}, {"inventory_added": ["junk_mail"]}),
        ScriptStep("act", {"verb": "pick_up", "target1": "letters"}, {"inventory_added": ["letters"]}),
        ScriptStep("act", {"verb": "pick_up", "target1": "papers"}, {"inventory_added": ["papers"]}),
        ScriptStep("act", {"verb": "pick_up", "target1": "package"}, {"inventory_added": ["package"]}),
        ScriptStep("act", {"verb": "open", "target1": "package"}, _objc("package")),
        ScriptStep("act", {"verb": "open", "target1": "window"}, _objc("window")),
        # The window is the outside<->office shortcut, walked three times in
        # order: out (->24), back in (->21), out again (->24).
        ScriptStep("act", {"verb": "walk to", "target1": "window"}, {"room_changed": 24}),
        ScriptStep("act", {"verb": "walk to", "target1": "window"}, {"room_changed": 21}),
        ScriptStep("act", {"verb": "walk to", "target1": "window"}, {"room_changed": 24}),
        ScriptStep("act", {"verb": "travel", "target1": "henry"}, {"room_changed": 27}),
        ScriptStep("act", {"verb": "pick_up", "target1": "painting"}, {"inventory_added": ["painting"]}),
        ScriptStep("act", {"verb": "pull", "target1": "bookcase"}, _objc("bookcase")),
        ScriptStep("act", {"verb": "pick_up", "target1": "sticky_tape"}, {"inventory_added": ["sticky_tape"]}),
        ScriptStep("act", {"verb": "walk to", "target1": 231}, {"room_changed": 24}),
        ScriptStep(
            "act", {"verb": "use", "target1": "sticky_tape", "target2": "jar"},
            {**_msgs(("indy", "Hey! There's a key in here!")), "inventory_added": ["small_key"]},
        ),
        ScriptStep("act", {"verb": "pick_up", "target1": "plant"}, _objc("plant")),
        ScriptStep("act", {"verb": "pull", "target1": "table_cloth"}, _objc("table cloth")),
        ScriptStep("act", {"verb": "use", "target1": "small_key", "target2": "chest"}, _objc("chest")),
        # The old book is the last Grail item taken, so Indy announces he has
        # everything he needs (which unlocks the Venice trip outside).
        ScriptStep(
            "act", {"verb": "pick_up", "target1": "old_book"},
            {
                **_msgs(("indy", "Now I have everything I need to begin my quest for my father and the Holy Grail!")),
                "inventory_added": ["old_book"],
            },
        ),
        ScriptStep("act", {"verb": "travel", "target1": "venice"}, {"room_changed": 28}),
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
