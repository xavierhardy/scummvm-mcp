"""The Dig demo walkthrough — start -> hub -> wreck -> pull wire -> dig."""

import asyncio

from fastmcp import Client

from scummvm_bench.backend import ScriptStep
from scummvm_bench.harness.base import RunContext

from ._base import Walkthrough, _msgs

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
