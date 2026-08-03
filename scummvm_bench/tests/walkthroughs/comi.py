"""Curse of Monkey Island demo walkthrough (non-deterministic pirate dialog)."""

import asyncio

from fastmcp import Client

from scummvm_bench.backend import ScriptStep
from scummvm_bench.harness.base import RunContext

from ._base import Walkthrough, _msgs

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

            async def state() -> dict:
                s = await call("state", {})
                return s if isinstance(s, dict) else {}

            async def room() -> object:
                r = (await state()).get("room")
                return r.get("id") if isinstance(r, dict) else None

            async def question() -> dict | None:
                q = (await state()).get("question")
                return q if isinstance(q, dict) else None

            async def wait_room(target: int, tries: int = 18) -> bool:
                for _ in range(tries):
                    if await room() == target:
                        return True
                    await asyncio.sleep(0.6)
                return False

            async def go_through(
                exit_obj: object, target: int, tries: int = 30
            ) -> bool:
                """Walk through a room exit until the room reaches ``target``.

                Winning the minigame returns to the cannon room behind a victory
                cutscene that locks input for a while ("game is not accepting
                input right now"); a single walk_to gives up before it clears, so
                retry patiently until the room actually flips."""
                for _ in range(tries):
                    if await room() == target:
                        return True
                    await call("act", {"verb": "walk_to", "target1": exit_obj}, tries=2)
                    if await wait_room(target, tries=3):
                        return True
                    await asyncio.sleep(1.0)
                return False

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

            # The failure insult leaves the pirate's topic menu open, and while a
            # dialog question is pending the engine rejects every act() with
            # "a dialog question is pending — use 'answer' first". Close the menu
            # (its last choice is always the "goodbye") so world actions are
            # accepted again; the threat speech plays first, so poll for the menu.
            for _ in range(10):
                if stop.is_set():
                    return
                q = await question()
                if q:
                    bye = (
                        pick(q, "swell talking", "goodbye", "later", "bye")
                        or q["choices"][-1]["id"]
                    )
                    await call("answer", {"id": bye})
                await asyncio.sleep(0.5)
                if not await question():
                    break

            # Pick up the hook Wally dropped and the ramrod off the wall.
            if stop.is_set():
                return
            await call("act", {"verb": "pick_up", "target1": "plastic_hook"})
            await call("act", {"verb": "pick_up", "target1": "ramrod"})

            # Man the cannon -> boat-sinking minigame (room 4). Each boat is
            # surfaced in state as a boat_N object carrying its aim point; sink
            # them one at a time until none remain (boats_remaining == 0).
            await call("act", {"verb": "use", "target1": "cannon"})
            await wait_room(4)
            for _ in range(12):
                if stop.is_set():
                    return
                boats = [
                    (o["x"], o["y"])
                    for o in (await state()).get("objects", [])
                    if str(o.get("name", "")).startswith("boat")
                ]
                if not boats:
                    break
                res = await call("shoot_cannon", {"x": boats[0][0], "y": boats[0][1]})
                if (
                    res.get("boats_remaining") == 0
                    or res.get("room_changed") is not None
                ):
                    break

            # The minigame returns to the cannon room (room 3) behind a victory
            # cutscene. Now that the boats are wreckage, fish the debris out at
            # the gunport (room 5): combine ramrod + plastic_hook into a gaff,
            # then gaff the debris to land the cutlass. Pathways: room 3 -> room 5
            # via obj_266, back via obj_321 (go_through waits out the cutscene).
            await go_through("obj_266", 5)
            await call(
                "act", {"verb": "use", "target1": "ramrod", "target2": "plastic_hook"}
            )
            await call("act", {"verb": "use", "target1": "gaff", "target2": "debris"})

            # Back to the cannon room and cut the restraint rope with the
            # cutlass, then fire the now-unrestrained cannon to escape (the
            # stopping goal). Firing only starts the closing video, which the
            # engine never settles out of, so the proxy latches the goal and
            # stops the run the moment this cannon call is received -- it is
            # never forwarded to the engine. One attempt, no retry, no hang.
            await go_through("obj_321", 3)
            await call(
                "act",
                {
                    "verb": "use",
                    "target1": "cutlass",
                    "target2": "cannon_restraint_rope",
                },
            )
            if stop.is_set():
                return
            await call("act", {"verb": "use", "target1": "cannon"}, tries=1)


COMI = Walkthrough(
    game_id="comi-demo",
    save_slot=1,
    initial_room=3,
    expected_goals=16,
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
        ("act", {"verb": "use", "target1": "cannon"}),  # man_the_cannon (-> room 4)
        ("shoot_cannon", {"x": 228, "y": 238}),  # sink_boat_1 (3 afloat)
        ("shoot_cannon", {"x": 304, "y": 203}),  # sink_boat_2 (2 afloat)
        ("shoot_cannon", {"x": 393, "y": 195}),  # sink_boat_3 (1 afloat)
        ("shoot_cannon", {"x": 468, "y": 280}),  # sink_boat_4 (0 -> back to room 3)
        (
            "act",
            {"verb": "use", "target1": "ramrod", "target2": "plastic_hook"},
        ),  # make_gaff
        (
            "act",
            {"verb": "use", "target1": "gaff", "target2": "debris"},
        ),  # fish cutlass
        # cut_restraint_rope — snip the rope (now in its cut state).
        (
            "act",
            {"verb": "use", "target1": "cutlass", "target2": "cannon_restraint_rope"},
        ),
        # fire_unrestrained_cannon (STOP) — firing with the rope already cut is
        # the escape; the proxy stops the run on receipt of this call (it is
        # never forwarded, so it starts no closing video and needs no response).
        ("act", {"verb": "use", "target1": "cannon"}),
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
        # First "use cannon": with the rope still intact it drops Guybrush into
        # the boat-sinking minigame (room 4).
        ScriptStep(
            "act",
            {"verb": "use", "target1": "cannon"},
            {"room_changed": 4, **_msgs(("guybrush", "Let's see how this works."))},
        ),
        # The four aimed shots: each sinks one war-canoe, so boats_remaining
        # counts 3, 2, 1, 0. The final hit wins the minigame and returns to the
        # cannon room (room 3). Same (tool, {}) match -> consumed in order.
        ScriptStep("shoot_cannon", {}, {"boats_remaining": 3}),
        ScriptStep("shoot_cannon", {}, {"boats_remaining": 2}),
        ScriptStep("shoot_cannon", {}, {"boats_remaining": 1}),
        ScriptStep(
            "shoot_cannon",
            {},
            {"boats_remaining": 0, "room_changed": 3, **_msgs(("guybrush", "Yes!"))},
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
        # Cutting the rope: the engine reports no object-state change (Wally just
        # weeps), so the "rope is cut" fact is carried by the cut *act* itself.
        ScriptStep(
            "act",
            {"verb": "use", "target1": "cutlass", "target2": "cannon_restraint_rope"},
            {**_msgs(("wally", "<weep>"))},
        ),
        # The second "use cannon" (rope now cut) is the stopping beat. The proxy
        # ends the run on receipt of that call — it is never forwarded to the
        # backend (firing would start the unstoppable closing video), so it needs
        # no scripted response here.
    ],
)
