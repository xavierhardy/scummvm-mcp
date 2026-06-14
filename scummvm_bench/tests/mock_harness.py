"""A deterministic mock backend + mock harness for the Monkey Island demo.

``monkey_backend()`` returns a fresh scripted ``MockBackend`` whose responses are
the exact result shapes the real game produces (room transitions, inventory,
dialog messages). ``MONKEY_CALLS`` is the goal-satisfying call sequence; the
``MockHarness`` replays it against the bench proxy via an in-memory client.
"""

import asyncio

from fastmcp import Client

from scummvm_bench.backend import MockBackend, ScriptStep
from scummvm_bench.harness.base import RunContext

INITIAL_ROOM = 55

# Exact, ordered call sequence that reaches every Monkey Island goal. The number
# of entries is the authoritative expected MCP call count for the mock run.
MONKEY_CALLS: list[tuple[str, dict[str, object]]] = [
    ("state", {}),  # 1  state_first_scene
    ("walk", {"x": 120, "y": 132}),  # 2  troll_appears
    ("act", {"verb": "walk_to", "target1": "Troll"}),  # 3
    ("act", {"verb": "talk_to", "target1": "Troll"}),  # 4  talk_to_troll
    ("act", {"verb": "open", "target1": "door"}),  # 5  open_scummbar_door
    ("act", {"verb": "walk_to", "target1": "door"}),  # 6  enter_scummbar (->52)
    ("state", {}),  # 7  state_scummbar
    ("act", {"verb": "pick_up", "target1": "bowl o' mints"}),  # 8  get_breath_mint
    ("act", {"verb": "open", "target1": "door"}),  # 9  open_backroom_door
    ("act", {"verb": "walk_to", "target1": "door"}),  # 10 enter_backroom (->51)
    ("state", {}),  # 11 state_backroom
    ("act", {"verb": "open", "target1": "door"}),  # 12 open_backroom_exit
    ("act", {"verb": "walk", "target1": "plank"}),  # 13 plank_1 (->56)
    ("act", {"verb": "walk", "target1": "plank"}),  # 14 plank_2 (->53)
    ("act", {"verb": "walk", "target1": "plank"}),  # 15 plank_3 (->50)
    ("act", {"verb": "pick_up", "target1": "red herring"}),  # 16 pick_red_herring
    ("act", {"verb": "walk", "target1": "archway"}),  # 17 reach_city (->57)
    ("state", {}),  # 18 state_city
    ("act", {"verb": "walk", "target1": "jail_entrance"}),  # 19 reach_jail (->54)
    ("state", {}),  # 20 state_prison
    ("act", {"verb": "give", "target1": "breath_mint", "target2": "prisoner"}),  # 21
    ("answer", {"id": 3}),  # 22 ask_magic_phrase
    ("act", {"verb": "walk", "target1": "archway"}),  # 23 back to city (->57)
    ("act", {"verb": "open", "target1": "door"}),  # 24 open_fortune_door
    ("act", {"verb": "walk_to", "target1": "door"}),  # 25 enter_fortune_shop (->58)
    ("state", {}),  # 26 state_fortune_shop
    (
        "act",
        {"verb": "give", "target1": "red_herring", "target2": "fortune_teller"},
    ),  # 27
    ("answer", {"id": 2}),  # 28 ask_teller_phrase
    ("answer", {"id": 1}),  # 29 teller_game_1
    ("answer", {"id": 4}),  # 30 teller_game_2
    ("act", {"verb": "talk_to", "target1": "Troll"}),  # 31 tell_troll_phrase (STOP)
]

EXPECTED_CALLS = len(MONKEY_CALLS)


def monkey_script() -> list[ScriptStep]:
    """The scripted backend responses for the Monkey Island journey."""
    return [
        ScriptStep(
            "walk",
            {"x": 120, "y": 132},
            {
                "messages": [{"actor": "troll", "text": "None shall pass!"}],
                "position": {"x": 120, "y": 132},
            },
        ),
        ScriptStep(
            "act",
            {"verb": "walk_to", "target1": "Troll"},
            {"position": {"x": 180, "y": 132}},
        ),
        # talk_to Troll: first call opens the dialog, the final call (after the
        # fortune teller) gets the troll's surrender line.
        ScriptStep(
            "act",
            {"verb": "talk_to", "target1": "Troll"},
            {
                "messages": [{"actor": "guybrush", "text": "Hi. I'm Guybrush."}],
                "question": {"choices": [{"id": 1, "label": "Pretty please?"}]},
            },
        ),
        ScriptStep(
            "act",
            {"verb": "talk_to", "target1": "Troll"},
            {
                "messages": [
                    {
                        "actor": "troll",
                        "text": "Hmmmm^ I don't know how you did it, but you "
                        "did it, Creepfood.",
                    }
                ]
            },
        ),
        ScriptStep(
            "act",
            {"verb": "open", "target1": "door"},
            {
                "objects_changed": [{"name": "door", "old_state": 0, "new_state": 1}],
            },
        ),
        # walk_to door: three successive transitions (SCUMM Bar, back room, shop).
        ScriptStep("act", {"verb": "walk_to", "target1": "door"}, {"room_changed": 52}),
        ScriptStep("act", {"verb": "walk_to", "target1": "door"}, {"room_changed": 51}),
        ScriptStep("act", {"verb": "walk_to", "target1": "door"}, {"room_changed": 58}),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "bowl o' mints"},
            {
                "inventory_added": ["breath_mint"],
                "messages": [{"actor": "guybrush", "text": "A breath mint."}],
            },
        ),
        ScriptStep("act", {"verb": "walk", "target1": "plank"}, {"room_changed": 56}),
        ScriptStep("act", {"verb": "walk", "target1": "plank"}, {"room_changed": 53}),
        ScriptStep("act", {"verb": "walk", "target1": "plank"}, {"room_changed": 50}),
        ScriptStep(
            "act",
            {"verb": "pick_up", "target1": "red herring"},
            {"inventory_added": ["red_herring"]},
        ),
        ScriptStep("act", {"verb": "walk", "target1": "archway"}, {"room_changed": 57}),
        ScriptStep(
            "act", {"verb": "walk", "target1": "jail_entrance"}, {"room_changed": 54}
        ),
        ScriptStep(
            "act",
            {"verb": "give", "target1": "breath_mint", "target2": "prisoner"},
            {
                "inventory_removed": ["breath_mint"],
                "messages": [{"actor": "prisoner", "text": "Don't mention it."}],
                "question": {
                    "choices": [
                        {"id": 3, "label": "Do you know anything about a magic phrase?"}
                    ]
                },
            },
        ),
        ScriptStep(
            "answer",
            {"id": 3},
            {"messages": [{"actor": "prisoner", "text": "A magic phrase, eh?"}]},
        ),
        ScriptStep(
            "act",
            {"verb": "give", "target1": "red_herring", "target2": "fortune_teller"},
            {
                "messages": [
                    {
                        "actor": "guybrush",
                        "text": "Put part of the fish into the cauldron^ "
                        "Guybrush Threepwood.",
                    }
                ]
            },
        ),
        ScriptStep(
            "answer",
            {"id": 2},
            {
                "messages": [
                    {
                        "actor": "guybrush",
                        "text": "What do you know about magic phrases?",
                    }
                ]
            },
        ),
        ScriptStep(
            "answer",
            {"id": 1},
            {"messages": [{"actor": "guybrush", "text": "Yes^that's amazing!"}]},
        ),
        ScriptStep(
            "answer",
            {"id": 4},
            {"messages": [{"actor": "guybrush", "text": "Yes, I was."}]},
        ),
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
