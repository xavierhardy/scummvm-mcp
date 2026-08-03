"""Loom segment of Passport to Adventure — clearing -> dark tent."""

import asyncio

from fastmcp import Client

from scummvm_bench.backend import ScriptStep
from scummvm_bench.harness.base import RunContext

from ._base import Walkthrough, _msgs

# ---------------------------------------------------------------------------
# Loom segment of Passport to Adventure (save slot 2) — clearing -> dark tent
# ---------------------------------------------------------------------------


class LoomRealHarness:
    """Drives the whole Loom demo (the 25-goal walkthrough), reconciled live.

    Navigation is single-cursor ``interact`` on ``pathway_<id>`` objects (Bobbin
    stops at intermediate points, so each pathway needs several clicks). Drafts
    are LEARNED dynamically and replayed -- the note sequences are randomized per
    game, so the harness listens (egg / owls / dye pot), reads the notes the
    engine plays back (surfaced as ``result["notes"]``), and replays them with
    ``play_note`` rather than hardcoding e-c-e-d etc.
    """

    def __init__(self, walkthrough: "Walkthrough") -> None:
        self.walkthrough = walkthrough

    def run(self, ctx: RunContext) -> str | None:
        asyncio.run(self._play(ctx))
        return None

    async def _play(self, ctx: RunContext) -> None:
        stop = ctx.stop_event
        async with Client(ctx.proxy.app) as client:

            async def call(tool: str, args: dict, tries: int = 14) -> dict:
                data: dict = {}
                for _ in range(tries):
                    if stop.is_set():
                        return data
                    try:
                        res = await client.call_tool(tool, args)
                        data = res.data if isinstance(res.data, dict) else {}
                    except Exception:  # noqa: BLE001 - transient "not accepting input" etc.
                        await asyncio.sleep(0.8)
                        continue
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

            async def has_obj(oid: int) -> bool:
                return any(
                    o.get("id") == oid for o in (await state()).get("objects") or []
                )

            async def wait_room(target: int, tries: int = 20) -> bool:
                for _ in range(tries):
                    if stop.is_set() or await room() == target:
                        return await room() == target
                    await asyncio.sleep(0.6)
                return await room() == target

            async def go_pathway(pid: int, target: int, tries: int = 10) -> bool:
                for _ in range(tries):
                    if stop.is_set():
                        return False
                    if await room() == target:
                        return True
                    await call("act", {"verb": "interact", "target1": pid})
                    await asyncio.sleep(2.0)
                return await room() == target

            async def settle(
                target: int, want_egg: bool | None, max_s: int = 80
            ) -> None:
                # Wait out a cutscene until control returns to ``target`` (and,
                # if given, the egg is present / consumed).
                for _ in range(max_s):
                    if stop.is_set():
                        return
                    s = await state()
                    rid = (s.get("room") or {}).get("id")
                    if rid == target:
                        if want_egg is None:
                            return
                        present = any(
                            o.get("id") == 609 for o in s.get("objects") or []
                        )
                        if present == want_egg:
                            return
                    await asyncio.sleep(1.0)

            async def point_cast(target, notes: list, tries: int = 8) -> dict:
                # Point at a target (Bobbin may need several clicks to walk over)
                # then cast the draft; retry until the cast is accepted.
                data: dict = {}
                for _ in range(tries):
                    if stop.is_set() or not notes:
                        return data
                    await call("act", {"verb": "interact", "target1": target})
                    await asyncio.sleep(2.0)
                    data = await call("play_note", {"notes": notes})
                    msgs = " ".join(
                        m.get("text", "") for m in (data.get("messages") or [])
                    )
                    if "point at something" not in msgs:
                        return data
                return data

            await state()  # state_first_room (36)
            await go_pathway(460, 39)  # reach_village
            await state()  # state_village (39)
            await go_pathway(510, 41)  # reach_tents
            await state()  # state_tents (41)
            await go_pathway(541, 44)  # reach_tent_left
            await state()  # state_tent (44)

            async def wait_input(max_s: int = 90) -> None:
                # A cutscene is playing (input locked); poll a harmless probe (the
                # loom) until the engine accepts input again.
                for _ in range(max_s):
                    if stop.is_set():
                        return
                    probe = await call(
                        "act", {"verb": "interact", "target1": 611}, tries=1
                    )
                    if not probe.get("error"):
                        return
                    await asyncio.sleep(1.0)

            # Walk to the Elders on the right -> the long High Council cutscene
            # leaves Bobbin in room 45 with Hetchel's egg.
            await call(
                "act", {"verb": "interact", "target1": 8}
            )  # reach_council (->45)
            await settle(45, want_egg=True)
            await wait_input()  # the egg appears mid-cutscene; wait for control
            await state()
            # PICK UP THE DISTAFF FIRST (object 610). Nothing -- not the egg, not
            # any later draft -- works without it ("...the distaff... Feels
            # heavier than it looks.").
            await call("act", {"verb": "interact", "target1": 610})  # pick_up_distaff
            await asyncio.sleep(1.5)
            # Listen to the egg ("It's trying to open!") and capture its Opening
            # draft (reused on the sky later); replay it to hatch -- now possible
            # because Bobbin holds the distaff.
            await call("walk", {"x": 40, "y": 130})
            await asyncio.sleep(2)
            listen = await call(
                "act", {"verb": "interact", "target1": 609}
            )  # listen_egg
            opening = listen.get("notes") or ["e", "c", "e", "d"]
            await call("play_note", {"notes": opening})  # play_eced_egg
            await wait_input()  # wait out the hatch cutscene
            # Out of the tent and over to the forest (top-left pathway of village 39).
            await go_pathway(606, 44)
            await go_pathway(592, 41)
            await go_pathway(540, 39)
            await go_pathway(511, 40)  # reach_forest
            # Hear all four owls; the last hole plays back the full darkness draft.
            darkness: list = ["d", "c", "c", "d"]
            for hid in (517, 518, 519, 520):
                r = await call(
                    "act", {"verb": "interact", "target1": hid}
                )  # owl_* messages
                if r.get("notes"):
                    darkness = r["notes"]
                await asyncio.sleep(1.0)
            # To the other tent and cast the owls' draft on the darkness.
            await go_pathway(516, 39)
            await go_pathway(510, 41)
            await go_pathway(539, 38)  # reach_other_tent
            await state()  # state_other_tent (38)
            await point_cast(952, darkness)  # play_darkness_draft (->42)
            await wait_room(42)
            # Read the book, learn the dye draft from the dye pot, dye the heap green.
            await call("act", {"verb": "interact", "target1": 548})  # interact_book
            dye = (await call("act", {"verb": "interact", "target1": 544})).get(
                "notes"
            ) or ["d", "d", "c", "d"]  # interact_dye_pot
            await point_cast(549, dye)  # dye_green
            # Back to the cliff; open the sky (it is on the right) with the Opening draft.
            await go_pathway(543, 41)
            await go_pathway(540, 39)
            await go_pathway(509, 36)  # return to the first room (re-entry #1)
            await point_cast(463, opening)  # open_sky
            # Off to the dock (the beach), then use the tree to leave the island.
            await go_pathway(460, 39)
            await go_pathway(512, 46)  # reach_dock
            await go_pathway(625, 36)  # leave_island (re-entry #2, STOP)


def _note_step(target, notes, result):
    return ScriptStep(
        "act", {"verb": "interact", "target1": target}, {**result, "notes": notes}
    )


LOOM = Walkthrough(
    game_id="pass",
    save_slot=6,
    initial_room=36,
    expected_goals=25,
    dynamic_real=True,
    real_harness_factory=lambda wt: LoomRealHarness(wt),
    calls=[
        ("state", {}),  # 1 state_first_room (36)
        ("act", {"verb": "interact", "target1": 460}),  # reach_village (->39)
        ("state", {}),  # state_village (39)
        ("act", {"verb": "interact", "target1": 510}),  # reach_tents (->41)
        ("state", {}),  # state_tents (41)
        ("act", {"verb": "interact", "target1": 541}),  # reach_tent_left (->44)
        ("state", {}),  # state_tent (44)
        ("act", {"verb": "interact", "target1": 8}),  # reach_council (->45)
        ("act", {"verb": "interact", "target1": 610}),  # pick_up_distaff (FIRST)
        ("act", {"verb": "interact", "target1": 609}),  # listen_egg
        ("play_note", {"notes": ["e", "c", "e", "d"]}),  # play_eced_egg
        ("act", {"verb": "interact", "target1": 511}),  # reach_forest (->40)
        ("act", {"verb": "interact", "target1": 517}),  # owl_in_there
        ("act", {"verb": "interact", "target1": 518}),  # owl_too
        ("act", {"verb": "interact", "target1": 519}),  # owl_another
        ("act", {"verb": "interact", "target1": 520}),  # owls_all_full
        ("act", {"verb": "interact", "target1": 539}),  # reach_other_tent (->38)
        ("state", {}),  # state_other_tent (38)
        ("act", {"verb": "interact", "target1": 952}),  # point at the darkness
        ("play_note", {"notes": ["d", "c", "c", "d"]}),  # play_darkness_draft (->42)
        ("act", {"verb": "interact", "target1": 548}),  # interact_book
        ("act", {"verb": "interact", "target1": 544}),  # interact_dye_pot
        ("act", {"verb": "interact", "target1": 549}),  # point at the heap
        ("play_note", {"notes": ["d", "d", "c", "d"]}),  # dye_green
        ("act", {"verb": "interact", "target1": 509}),  # return to 36 (re-entry #1)
        ("act", {"verb": "interact", "target1": 463}),  # point at the sky
        ("play_note", {"notes": ["e", "c", "e", "d"]}),  # open_sky ("real game", in 36)
        ("act", {"verb": "interact", "target1": 512}),  # reach_dock (->46)
        ("act", {"verb": "interact", "target1": 625}),  # leave_island (->36, #2, STOP)
    ],
    steps=[
        ScriptStep("act", {"verb": "interact", "target1": 460}, {"room_changed": 39}),
        ScriptStep("act", {"verb": "interact", "target1": 510}, {"room_changed": 41}),
        ScriptStep("act", {"verb": "interact", "target1": 541}, {"room_changed": 44}),
        ScriptStep("act", {"verb": "interact", "target1": 8}, {"room_changed": 45}),
        # Pick up the distaff first -- the prerequisite for every draft.
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 610},
            _msgs(("bobbin", "distaff"), ("bobbin", "Feels heavier than it looks.")),
        ),
        _note_step(
            609,
            ["e", "c", "e", "d"],
            _msgs(("bobbin", "egg"), ("bobbin", "It's trying to open!")),
        ),
        ScriptStep(
            "play_note",
            {"notes": ["e", "c", "e", "d"]},
            _msgs(
                ("hetchel", "Thank goodness you're still here."),
                ("hetchel", "You've already found the Elder's distaff."),
            ),
        ),
        ScriptStep("act", {"verb": "interact", "target1": 511}, {"room_changed": 40}),
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 517},
            _msgs(("bobbin", "There's an owl in there!")),
        ),
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 518},
            _msgs(("bobbin", "This hole has an owl too.")),
        ),
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 519},
            _msgs(("bobbin", "Another owl! The woods must be full of 'em.")),
        ),
        _note_step(
            520,
            ["d", "c", "c", "d"],
            _msgs(("bobbin", "Looks as if all the holes are full.")),
        ),
        ScriptStep("act", {"verb": "interact", "target1": 539}, {"room_changed": 38}),
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 952},
            _msgs(("bobbin", "darkness"), ("bobbin", "I can't see a thing.")),
        ),
        ScriptStep("play_note", {"notes": ["d", "c", "c", "d"]}, {"room_changed": 42}),
        ScriptStep(
            "act",
            {"verb": "interact", "target1": 548},
            _msgs(
                (
                    "bobbin",
                    "This is the Book of Patterns that Hetchel lets me read sometimes.",
                )
            ),
        ),
        _note_step(
            544,
            ["d", "d", "c", "d"],
            _msgs(("bobbin", "I wonder if this dye draft will work on the wool.")),
        ),
        ScriptStep(
            "act", {"verb": "interact", "target1": 549}, _msgs(("bobbin", "heap"))
        ),
        ScriptStep(
            "play_note",
            {"notes": ["d", "d", "c", "d"]},
            _msgs(("bobbin", "I changed the color!")),
        ),
        ScriptStep("act", {"verb": "interact", "target1": 509}, {"room_changed": 36}),
        ScriptStep(
            "act", {"verb": "interact", "target1": 463}, _msgs(("bobbin", "sky"))
        ),
        ScriptStep(
            "play_note",
            {"notes": ["e", "c", "e", "d"]},
            _msgs(("bobbin", "you should see the effects in the real game")),
        ),
        ScriptStep("act", {"verb": "interact", "target1": 512}, {"room_changed": 46}),
        ScriptStep("act", {"verb": "interact", "target1": 625}, {"room_changed": 36}),
    ],
)
