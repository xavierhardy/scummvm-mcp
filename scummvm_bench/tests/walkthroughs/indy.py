"""Indy3 segment of Passport to Adventure — the boxing gym fight."""

import asyncio

from fastmcp import Client

from scummvm_bench.backend import ScriptStep
from scummvm_bench.harness.base import RunContext

from ._base import Walkthrough, _msgs

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
                            (
                                c["id"]
                                for c in q["choices"]
                                if keyword in c["label"].lower()
                            ),
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
                    elif (
                        rid == 24
                        and "travel" in (s.get("verbs") or [])
                        and (saw or i >= 6)
                    ):
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
            prefer = (
                "work something out",
                "calmly",
                "take it easy",
                "fair for everyone",
            )
            resolve = "take down names"
            # Entering the office plays the mob-banging cutscene, which disables
            # input (talk to students returns "not accepting input") for longer
            # than a single call()'s retry budget. Keep re-opening the dialog
            # until the question actually appears, then answer the diplomatic
            # lines until the mob disperses into Indy's office (room 21) -- don't
            # bail the instant no question is up yet, or the cutscene wins.
            for _ in range(20):
                if stop.is_set():
                    return
                if await room() == 21:
                    break
                q = (await state()).get("question")
                if not isinstance(q, dict):
                    # Cutscene still holding input / dialog not open yet: nudge it.
                    await call("act", {"verb": "talk to", "target1": "students"})
                    await asyncio.sleep(0.6)
                    continue
                cid = next(
                    (
                        c["id"]
                        for c in q["choices"]
                        if any(k in c["label"].lower() for k in prefer)
                    ),
                    None,
                ) or next(
                    (c["id"] for c in q["choices"] if resolve in c["label"].lower()),
                    None,
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
            await call(
                "act", {"verb": "use", "target1": "sticky_tape", "target2": "jar"}
            )  # small_key
            await go("window", 24)  # back outside through the window
            # Henry's house #2 (last trip): in order -- move the plant, then pull
            # the table cloth (only possible after the plant moves) to reveal the
            # chest, then open it for the old book. The painting is already in
            # hand, so taking the old book gives Indy everything for the Grail
            # quest and the Venice trip unlocks back outside.
            await travel_to("henry", 27)
            await state()  # state_henry (27)
            await call(
                "act", {"verb": "pick_up", "target1": "plant"}
            )  # plant_moved (after the key)
            await call(
                "act", {"verb": "pull", "target1": "table_cloth"}
            )  # cloth_pulled (needs plant moved)
            await call(
                "act", {"verb": "use", "target1": "small_key", "target2": "chest"}
            )
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
        (
            "act",
            {"verb": "talk to", "target1": "students"},
        ),  # talk_to_students + 3 lines (->21)
        ("state", {}),  # state_indy_office (21)
        # Mail chain -> grail diary, then open the window (only after the diary).
        ("act", {"verb": "pick_up", "target1": "junk_mail"}),
        ("act", {"verb": "pick_up", "target1": "letters"}),
        ("act", {"verb": "pick_up", "target1": "papers"}),
        ("act", {"verb": "pick_up", "target1": "package"}),
        ("act", {"verb": "open", "target1": "package"}),  # open_package (grail diary)
        ("act", {"verb": "open", "target1": "window"}),  # open_window
        (
            "act",
            {"verb": "walk to", "target1": "window"},
        ),  # reach_outside (window ->24)
        ("state", {}),  # state_outside (24)
        # Henry's #1: painting + sticky tape (leave the plant AND the cloth).
        ("act", {"verb": "travel", "target1": "henry"}),  # travel_henry (->27)
        ("act", {"verb": "pick_up", "target1": "painting"}),  # pick_up_painting
        ("act", {"verb": "pull", "target1": "bookcase"}),  # bookcase_pulled
        ("act", {"verb": "pick_up", "target1": "sticky_tape"}),  # pick_up_sticky_tape
        # Back to the office through the window for the small key.
        ("act", {"verb": "walk to", "target1": 231}),  # nav Henry's -> outside (->24)
        (
            "act",
            {"verb": "walk to", "target1": "window"},
        ),  # nav outside -> office (window ->21)
        (
            "act",
            {"verb": "use", "target1": "sticky_tape", "target2": "jar"},
        ),  # obtain_small_key
        (
            "act",
            {"verb": "walk to", "target1": "window"},
        ),  # nav office -> outside (window ->24)
        # Henry's #2 (last trip), in order: move the plant, then pull the cloth
        # (only possible after the plant), then open the chest.
        ("act", {"verb": "travel", "target1": "henry"}),  # nav -> Henry's (->27)
        ("state", {}),  # state_henry (27)
        ("act", {"verb": "pick_up", "target1": "plant"}),  # plant_moved (after the key)
        (
            "act",
            {"verb": "pull", "target1": "table_cloth"},
        ),  # cloth_pulled (needs plant moved)
        (
            "act",
            {"verb": "use", "target1": "small_key", "target2": "chest"},
        ),  # use_key_on_chest
        (
            "act",
            {"verb": "pick_up", "target1": "old_book"},
        ),  # pick_up_old_book (last Grail item)
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
            "act",
            {"verb": "talk to", "target1": "students"},
            {**dict(_INDY_OFFICE_LINES), "room_changed": 21},
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "junk_mail"},
            {"inventory_added": ["junk_mail"]},
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "letters"},
            {"inventory_added": ["letters"]},
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "papers"},
            {"inventory_added": ["papers"]},
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "package"},
            {"inventory_added": ["package"]},
        ),
        ScriptStep("act", {"verb": "open", "target1": "package"}, _objc("package")),
        ScriptStep("act", {"verb": "open", "target1": "window"}, _objc("window")),
        # The window is the outside<->office shortcut, walked three times in
        # order: out (->24), back in (->21), out again (->24).
        ScriptStep(
            "act", {"verb": "walk to", "target1": "window"}, {"room_changed": 24}
        ),
        ScriptStep(
            "act", {"verb": "walk to", "target1": "window"}, {"room_changed": 21}
        ),
        ScriptStep(
            "act", {"verb": "walk to", "target1": "window"}, {"room_changed": 24}
        ),
        ScriptStep("act", {"verb": "travel", "target1": "henry"}, {"room_changed": 27}),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "painting"},
            {"inventory_added": ["painting"]},
        ),
        ScriptStep("act", {"verb": "pull", "target1": "bookcase"}, _objc("bookcase")),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "sticky_tape"},
            {"inventory_added": ["sticky_tape"]},
        ),
        ScriptStep("act", {"verb": "walk to", "target1": 231}, {"room_changed": 24}),
        ScriptStep(
            "act",
            {"verb": "use", "target1": "sticky_tape", "target2": "jar"},
            {
                **_msgs(("indy", "Hey! There's a key in here!")),
                "inventory_added": ["small_key"],
            },
        ),
        ScriptStep("act", {"verb": "pick_up", "target1": "plant"}, _objc("plant")),
        ScriptStep(
            "act", {"verb": "pull", "target1": "table_cloth"}, _objc("table cloth")
        ),
        ScriptStep(
            "act",
            {"verb": "use", "target1": "small_key", "target2": "chest"},
            _objc("chest"),
        ),
        # The old book is the last Grail item taken, so Indy announces he has
        # everything he needs (which unlocks the Venice trip outside).
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "old_book"},
            {
                **_msgs(
                    (
                        "indy",
                        "Now I have everything I need to begin my quest "
                        "for my father and the Holy Grail!",
                    )
                ),
                "inventory_added": ["old_book"],
            },
        ),
        ScriptStep(
            "act", {"verb": "travel", "target1": "venice"}, {"room_changed": 28}
        ),
    ],
)
