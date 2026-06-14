"""A deterministic mock backend + mock harness for the Monkey Island demo.

``MONKEY_CALLS`` mirrors the *real* captured walkthrough call-for-call;
``monkey_script()`` returns a scripted ``MockBackend`` whose responses are the
exact result shapes the live demo produces (room transitions, inventory, dialog
messages). The ``MockHarness`` replays the calls against the bench proxy via an
in-memory client. Steps that repeat the same ``(tool, args)`` with different
outcomes (dialog answers, the two troll conversations, the two walks to the bar
door) rely on the backend's sequential-consumption behaviour.
"""

import asyncio

from fastmcp import Client

from scummvm_bench.backend import MockBackend, ScriptStep
from scummvm_bench.harness.base import RunContext

INITIAL_ROOM = 55

# The exact, ordered call sequence that reaches every goal — the authoritative
# expected MCP call count for the mock run.
MONKEY_CALLS: list[tuple[str, dict[str, object]]] = [
    ("state", {}),  # 1  state_first_scene
    ("walk", {"x": 120, "y": 132}),  # 2  troll_appears
    ("act", {"verb": "talk_to", "target1": "Troll"}),  # 3  talk_to_troll
    ("answer", {"id": 3}),  # 4  close troll dialog
    ("act", {"verb": "walk_to", "target1": "door"}),  # 5
    ("act", {"verb": "open", "target1": "door"}),  # 6  open_scummbar_door
    ("act", {"verb": "walk_to", "target1": "door"}),  # 7  enter_scummbar (->52)
    ("state", {}),  # 8  state_scummbar
    ("act", {"verb": "pick_up", "target1": "bowl o' mints"}),  # 9  get_breath_mint
    ("act", {"verb": "open", "target1": 354}),  # 10 open_backroom_door
    ("act", {"verb": "walk_to", "target1": 354}),  # 11 enter_backroom (->51)
    ("state", {}),  # 12 state_backroom
    ("act", {"verb": "open", "target1": 304}),  # 13 open_backroom_exit
    ("act", {"verb": "walk_to", "target1": 304}),  # 14 onto the dock
    ("act", {"verb": "walk_to", "target1": 307}),  # 15 plank_1
    ("act", {"verb": "walk_to", "target1": 307}),  # 16 plank_2
    ("act", {"verb": "walk_to", "target1": 307}),  # 17 plank_3
    ("act", {"verb": "pick_up", "target1": "red_herring"}),  # 18 pick_red_herring
    ("act", {"verb": "walk_to", "target1": 305}),  # 19 back to bar (->52)
    ("act", {"verb": "walk", "target1": 353}),  # 20 to dock clearing (->55)
    ("act", {"verb": "walk", "target1": "archway"}),  # 21 reach_city (->57)
    ("state", {}),  # 22 state_city
    ("act", {"verb": "walk", "target1": "jail_entrance"}),  # 23 reach_jail (->54)
    ("state", {}),  # 24 state_prison
    ("act", {"verb": "give", "target1": "breath_mint", "target2": "prisoner"}),  # 25
    ("answer", {"id": 3}),  # 26 ask_magic_phrase
    ("act", {"verb": "walk", "target1": 401}),  # 27 leave jail (->57)
    ("act", {"verb": "open", "target1": 432}),  # 28 open_fortune_door
    ("act", {"verb": "walk_to", "target1": 432}),  # 29 enter_fortune_shop (->53)
    ("state", {}),  # 30 state_fortune_shop
    ("walk", {"x": 300, "y": 110}),  # 31 trigger the fortune teller
    ("answer", {"id": 4}),  # 32 give_fish_to_teller
    ("answer", {"id": 1}),  # 33 teller_game_1
    ("answer", {"id": 1}),  # 34 teller_game_2
    ("answer", {"id": 4}),  # 35 ask_teller_phrase
    ("answer", {"id": 3}),  # 36 leave the fortune teller
    ("act", {"verb": "open", "target1": 381}),  # 37
    ("act", {"verb": "walk_to", "target1": 381}),  # 38 exit shop (->57)
    ("act", {"verb": "walk", "target1": 438}),  # 39 back to the bridge (->55)
    ("walk", {"x": 120, "y": 132}),  # 40 approach the troll again
    ("act", {"verb": "talk_to", "target1": "Troll"}),  # 41 magic-phrase choice
    ("answer", {"id": 3}),  # 42 tell_troll_phrase (STOPPING)
]

EXPECTED_CALLS = len(MONKEY_CALLS)


def _door_changed(name: str = "door") -> dict[str, object]:
    return {"objects_changed": [{"name": name, "old_state": 0, "new_state": 1}]}


def _msgs(*pairs: tuple[str, str]) -> dict[str, object]:
    return {"messages": [{"actor": a, "text": t} for a, t in pairs]}


def monkey_script() -> list[ScriptStep]:
    """Scripted backend responses mirroring the live demo, in consumption order."""
    fish_q = {"question": {"choices": [{"id": 4, "label": "magic phrase"}]}}
    return [
        ScriptStep("walk", {"x": 120, "y": 132}, _msgs(("troll", "None shall pass!"))),
        ScriptStep(
            "walk",
            {"x": 300, "y": 110},
            {
                **_msgs(
                    ("citizen of mêlée", "Ah, I smell that you have brought a fish^")
                ),
                **fish_q,
            },
        ),
        # two troll conversations (sequential): first opens the dialog, the second
        # (after learning the phrase) offers the magic-phrase choice.
        ScriptStep(
            "act",
            {"verb": "talk_to", "target1": "Troll"},
            {
                **_msgs(("troll", "No one gets by me until they say the magic words.")),
                "question": {"choices": [{"id": 3, "label": "Pretty please?"}]},
            },
        ),
        ScriptStep(
            "act",
            {"verb": "talk_to", "target1": "Troll"},
            {
                **_msgs(("troll", "Listen carefully: say the magic words.")),
                "question": {
                    "choices": [
                        {"id": 3, "label": "Ah, but a new faculty Shadow is nigh."}
                    ]
                },
            },
        ),
        # walk_to the bar door: first call positions, second goes through.
        ScriptStep("act", {"verb": "walk_to", "target1": "door"}, {}),
        ScriptStep("act", {"verb": "walk_to", "target1": "door"}, {"room_changed": 52}),
        ScriptStep("act", {"verb": "open", "target1": "door"}, _door_changed()),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "bowl o' mints"},
            {
                "inventory_added": ["breath_mint"],
                **_door_changed("breath mint"),
                **_msgs(("guybrush", "I'll just take what I need.")),
            },
        ),
        ScriptStep("act", {"verb": "open", "target1": 354}, _door_changed()),
        ScriptStep("act", {"verb": "walk_to", "target1": 354}, {"room_changed": 51}),
        ScriptStep("act", {"verb": "open", "target1": 304}, _door_changed()),
        ScriptStep("act", {"verb": "walk_to", "target1": 304}, {}),
        ScriptStep(
            "act", {"verb": "walk_to", "target1": 307}, _door_changed("obj-307")
        ),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "red_herring"},
            {
                "inventory_added": ["red_herring"],
                **_door_changed("red herring"),
            },
        ),
        ScriptStep("act", {"verb": "walk_to", "target1": 305}, {"room_changed": 52}),
        ScriptStep("act", {"verb": "walk", "target1": 353}, {"room_changed": 55}),
        ScriptStep("act", {"verb": "walk", "target1": "archway"}, {"room_changed": 57}),
        ScriptStep(
            "act", {"verb": "walk", "target1": "jail_entrance"}, {"room_changed": 54}
        ),
        ScriptStep(
            "act",
            {
                "verb": "give",
                "target1": "breath_mint",
                "target2": "prisoner",
            },
            {
                "inventory_removed": ["breath_mint"],
                **_msgs(("prisoner", "Ooooh! Grog-o-mint!")),
                "question": {
                    "choices": [
                        {"id": 3, "label": "Do you know anything about a magic phrase?"}
                    ]
                },
            },
        ),
        ScriptStep("act", {"verb": "walk", "target1": 401}, {"room_changed": 57}),
        ScriptStep(
            "act",
            {"verb": "open", "target1": 432},
            _door_changed("fortune teller's door"),
        ),
        ScriptStep("act", {"verb": "walk_to", "target1": 432}, {"room_changed": 53}),
        ScriptStep("act", {"verb": "open", "target1": 381}, {}),
        ScriptStep("act", {"verb": "walk_to", "target1": 381}, {"room_changed": 57}),
        ScriptStep("act", {"verb": "walk", "target1": 438}, {"room_changed": 55}),
        # dialog answers — consumed sequentially per id, so each id's steps must
        # be listed in call order:
        #   answer 3 -> calls 4, 26, 36, 42   answer 4 -> 32, 35   answer 1 -> 33, 34
        ScriptStep("answer", {"id": 3}, _msgs(("troll", "Not those magic words!"))),
        ScriptStep(
            "answer",
            {"id": 3},
            _msgs(
                ("guybrush", "Do you know anything about a magic phrase?"),
                ("prisoner", "Tim once told me that `a third Shadow is nigh.`"),
            ),
        ),
        ScriptStep(
            "answer", {"id": 3}, _msgs(("guybrush", "I think I'll just browse."))
        ),
        ScriptStep(
            "answer",
            {"id": 3},
            _msgs(
                ("guybrush", "Ah, but a new faculty Shadow is nigh."),
                (
                    "troll",
                    "Hmmmm^ I don't know how you did it, but you did it, Creepfood.",
                ),
            ),
        ),
        ScriptStep(
            "answer",
            {"id": 4},
            _msgs(
                ("guybrush", "But all I'm looking for is the magic phrase."),
                (
                    "citizen of mêlée",
                    "Put part of the fish into the cauldron^ Guybrush Threepwood.",
                ),
            ),
        ),
        ScriptStep(
            "answer",
            {"id": 4},
            _msgs(
                ("guybrush", "What do you know about magic phrases?"),
                (
                    "citizen of mêlée",
                    "She said `Dear, maybe you should get a new faculty advisor.`",
                ),
            ),
        ),
        ScriptStep("answer", {"id": 1}, _msgs(("guybrush", "Yes^that's amazing!"))),
        ScriptStep("answer", {"id": 1}, _msgs(("guybrush", "Yes, I was."))),
    ]


def monkey_backend() -> MockBackend:
    """A fresh, scripted Monkey Island backend (steps reset)."""
    return MockBackend(monkey_script(), initial_room=INITIAL_ROOM)


async def drive(client: Client, calls: list[tuple[str, dict[str, object]]]) -> None:
    """Replay ``calls`` against an open in-memory MCP ``client``."""
    for tool, args in calls:
        await client.call_tool(tool, args)


class MockHarness:
    """Replays ``MONKEY_CALLS`` against the bench proxy (no pi, no ScummVM)."""

    def __init__(
        self, calls: list[tuple[str, dict[str, object]]] | None = None
    ) -> None:
        self.calls = calls if calls is not None else MONKEY_CALLS

    def run(self, ctx: RunContext) -> str | None:
        async def go() -> None:
            async with Client(ctx.proxy.app) as client:
                await drive(client, self.calls)

        asyncio.run(go())
        return None
