"""Sam & Max Hit the Road demo walkthrough — office -> street -> DeSoto."""

import asyncio

from fastmcp import Client

from scummvm_bench.backend import ScriptStep
from scummvm_bench.harness.base import RunContext

from ._base import GAMES_DIR, Walkthrough, _msgs

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
                data = await call(
                    "act", {"verb": "use", "target1": "max", "target2": 4}
                )
                inv = (await state()).get("inventory") or []
                if "carnival_tickets" in inv or "carnival_tickets" in (
                    data.get("inventory_added") or []
                ):
                    break
                await asyncio.sleep(1.2)
            # Board the DeSoto. The stopping goal (use_desoto) now latches on this
            # use-beat_up_desoto call with the carnival tickets in hand, so it no
            # longer races the drive-away cutscene's asynchronous room flip. Let
            # the use-Max cutscene settle back to the street first so it's a real
            # boarding (best-effort — the goal fires on the call regardless).
            await wait_room(9, tries=12)
            await call("act", {"verb": "use", "target1": "beat_up_desoto"})


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
