"""Goal set for the Monkey Island 1 EGA demo, save slot 1 (troll bridge).

The scenario follows the user's walkthrough: reach the troll, learn a magic
phrase is needed, work through the SCUMM Bar / back room / dock, the city, the
jail (give the prisoner a breath mint), the fortune teller, and finally tell the
troll the magic phrase (the stopping goal).

Room ids and item/object tokens for the early steps (55/52/51, ``breath_mint``,
``door``, ``prisoner``) come from ``test/mcp/test_monkey.py``. The dock/city/shop
room ids (56/53/50/57/58) are best-effort for *real* runs; the late dialogue
goals are matched on the user's exact quoted lines (room-agnostic), which is
robust. The mock backend scripts these exact transitions/messages so the
mock-harness benchmark is deterministic.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    in_room,
    on_call,
    on_inventory_added,
    on_inventory_removed,
    on_message_contains,
    on_object_changed,
    on_question_appeared,
    on_room_changed,
)

# Room ids used along the journey.
ROOM_FIRST_SCENE = 55
ROOM_SCUMMBAR = 52
ROOM_BACKROOM = 51
ROOM_DOCK_1 = 56
ROOM_DOCK_2 = 53
ROOM_DOCK_3 = 50
ROOM_CITY = 57
ROOM_JAIL = 54
ROOM_FORTUNE_SHOP = 58


def _ordered(*goals: Goal) -> dict[str, Goal]:
    table: dict[str, Goal] = {}
    for goal in goals:
        if goal.goal_id in table:
            raise ValueError(f"duplicate goal id {goal.goal_id!r}")
        table[goal.goal_id] = goal
    return table


def _goal(
    goal_id: str,
    label: str,
    predicate: Predicate,
    kind: str = "result",
    stopping: bool = False,
) -> Goal:
    return Goal(
        goal_id=goal_id,
        label=label,
        predicate=predicate,
        stopping=stopping,
        kind=kind,
    )


GOALS = _ordered(
    _goal(
        "state_first_scene",
        "Check state in the first scene",
        on_call("state"),
        kind="call",
    ),
    _goal(
        "troll_appears",
        "Troll appears (walked toward the troll)",
        on_message_contains("None shall pass"),
    ),
    _goal(
        "talk_to_troll",
        "Talk to the troll (learn a magic phrase is needed)",
        on_question_appeared(),
    ),
    _goal(
        "open_scummbar_door",
        "Open the SCUMM Bar door",
        all_of(in_room(ROOM_FIRST_SCENE), on_object_changed("door")),
    ),
    _goal(
        "enter_scummbar",
        "Enter the SCUMM Bar",
        on_room_changed(ROOM_SCUMMBAR),
    ),
    _goal(
        "state_scummbar",
        "Check state in the SCUMM Bar",
        all_of(in_room(ROOM_SCUMMBAR), on_call("state")),
        kind="call",
    ),
    _goal(
        "get_breath_mint",
        "Get the breath mint",
        on_inventory_added("breath_mint"),
    ),
    _goal(
        "open_backroom_door",
        "Open the other SCUMM Bar door",
        all_of(in_room(ROOM_SCUMMBAR), on_object_changed("door")),
    ),
    _goal(
        "enter_backroom",
        "Walk to the SCUMM Bar back room",
        on_room_changed(ROOM_BACKROOM),
    ),
    _goal(
        "state_backroom",
        "Check state in the SCUMM Bar back room",
        all_of(in_room(ROOM_BACKROOM), on_call("state")),
        kind="call",
    ),
    _goal(
        "open_backroom_exit",
        "Open the door in the back room",
        all_of(in_room(ROOM_BACKROOM), on_object_changed("door")),
    ),
    _goal(
        "plank_1",
        "Walk on the plank on the right (1st time)",
        all_of(in_room(ROOM_BACKROOM), on_room_changed(ROOM_DOCK_1)),
    ),
    _goal(
        "plank_2",
        "Walk on the plank on the right (2nd time)",
        all_of(in_room(ROOM_DOCK_1), on_room_changed(ROOM_DOCK_2)),
    ),
    _goal(
        "plank_3",
        "Walk on the plank on the right (3rd time)",
        all_of(in_room(ROOM_DOCK_2), on_room_changed(ROOM_DOCK_3)),
    ),
    _goal(
        "pick_red_herring",
        "Pick up the red herring",
        on_inventory_added("red_herring"),
    ),
    _goal(
        "reach_city",
        "Walk to the archway (reach the city)",
        all_of(in_room(ROOM_DOCK_3), on_room_changed(ROOM_CITY)),
    ),
    _goal(
        "state_city",
        "Check state in the city",
        all_of(in_room(ROOM_CITY), on_call("state")),
        kind="call",
    ),
    _goal(
        "reach_jail",
        "Go to the jail entrance",
        on_room_changed(ROOM_JAIL),
    ),
    _goal(
        "state_prison",
        "Check state in the prison",
        all_of(in_room(ROOM_JAIL), on_call("state")),
        kind="call",
    ),
    _goal(
        "give_breath_mint",
        "Give the breath mint to the prisoner",
        all_of(
            on_call("act", verb="give", target1="breath_mint", target2="prisoner"),
            on_inventory_removed("breath_mint"),
        ),
        kind="call",
    ),
    _goal(
        "ask_magic_phrase",
        "Ask the prisoner about a magic phrase",
        all_of(in_room(ROOM_JAIL), on_call("answer", id=3)),
        kind="call",
    ),
    _goal(
        "open_fortune_door",
        "Open the fortune teller's door",
        all_of(in_room(ROOM_CITY), on_object_changed("door")),
    ),
    _goal(
        "enter_fortune_shop",
        "Walk to the fortune teller's shop",
        on_room_changed(ROOM_FORTUNE_SHOP),
    ),
    _goal(
        "state_fortune_shop",
        "Check state in the fortune teller's shop",
        all_of(in_room(ROOM_FORTUNE_SHOP), on_call("state")),
        kind="call",
    ),
    _goal(
        "give_fish_to_teller",
        "Put part of the fish into the cauldron",
        on_message_contains("Put part of the fish into the cauldron"),
    ),
    _goal(
        "ask_teller_phrase",
        "Ask the fortune teller about magic phrases",
        on_message_contains("What do you know about magic phrases"),
    ),
    _goal(
        "teller_game_1",
        "Get past the fortune teller game (1)",
        on_message_contains("that's amazing"),
    ),
    _goal(
        "teller_game_2",
        "Get past the fortune teller game (2)",
        on_message_contains("Yes, I was"),
    ),
    _goal(
        "tell_troll_phrase",
        "Tell the troll the magic phrase",
        on_message_contains("I don't know how you did it, but you did it"),
        stopping=True,
    ),
)


MONKEY_EGA_DEMO_GOALSET = GoalSet(
    game_id="monkey-ega-demo",
    save_slot=1,
    goals=GOALS,
)
