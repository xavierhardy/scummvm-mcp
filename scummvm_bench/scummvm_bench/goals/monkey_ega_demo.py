"""Goal set for the Monkey Island 1 EGA demo, save slot 1 (troll bridge).

Every predicate here was reconciled against a live capture of the actual demo
(English build), so the full walkthrough reaches 100% of the goals:

  troll bridge (55) -> SCUMM Bar (52) -> kitchen/dock (51) -> red herring puzzle
  -> city (57) -> jail (54, give the prisoner a breath mint and ask about the
  magic phrase) -> fortune teller (53, give the fish, ask about magic phrases ->
  she reveals "Dear, maybe you should get a new faculty advisor.") -> back to the
  troll -> choose "Ah, but a new faculty Shadow is nigh." -> the troll says
  "I don't know how you did it, but you did it" and lets you pass (stopping goal).

The red-herring dock is inside room 51 (no separate rooms); the loose plank is
object 307 and must be walked three times — those bounces are indistinguishable
in state, so the three plank goals share one call predicate and latch on the
1st/2nd/3rd occurrence via ``times``.
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
    on_room_changed,
)

ROOM_FIRST_SCENE = 55  # troll bridge / dock
ROOM_SCUMMBAR = 52
ROOM_KITCHEN = 51  # back room + (scrolled) red-herring dock
ROOM_CITY = 57  # Mêlée Island streets
ROOM_JAIL = 54
ROOM_FORTUNE = 53  # fortune teller
PLANK = 307  # loose plank object on the dock


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
    times: int = 1,
) -> Goal:
    return Goal(
        goal_id=goal_id,
        label=label,
        predicate=predicate,
        stopping=stopping,
        kind=kind,
        times=times,
    )


GOALS = _ordered(
    _goal(
        "state_first_scene",
        "Check state in the first scene",
        all_of(in_room(ROOM_FIRST_SCENE), on_call("state")),
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
        on_call("act", verb="talk_to", target1="Troll"),
        kind="call",
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
        on_room_changed(ROOM_KITCHEN),
    ),
    _goal(
        "state_backroom",
        "Check state in the SCUMM Bar back room",
        all_of(in_room(ROOM_KITCHEN), on_call("state")),
        kind="call",
    ),
    _goal(
        "open_backroom_exit",
        "Open the door onto the dock",
        all_of(in_room(ROOM_KITCHEN), on_object_changed("door")),
    ),
    _goal(
        "plank_1",
        "Walk on the plank on the right (1st time)",
        on_call("act", verb="walk_to", target1=PLANK),
        kind="call",
        times=1,
    ),
    _goal(
        "plank_2",
        "Walk on the plank on the right (2nd time)",
        on_call("act", verb="walk_to", target1=PLANK),
        kind="call",
        times=2,
    ),
    _goal(
        "plank_3",
        "Walk on the plank on the right (3rd time)",
        on_call("act", verb="walk_to", target1=PLANK),
        kind="call",
        times=3,
    ),
    _goal(
        "pick_red_herring",
        "Pick up the red herring",
        on_inventory_added("red_herring"),
    ),
    _goal(
        "reach_city",
        "Walk to the archway (reach the city)",
        all_of(in_room(ROOM_FIRST_SCENE), on_room_changed(ROOM_CITY)),
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
        on_message_contains("Do you know anything about a magic phrase"),
    ),
    _goal(
        "open_fortune_door",
        "Open the fortune teller's door",
        all_of(in_room(ROOM_CITY), on_object_changed("door")),
    ),
    _goal(
        "enter_fortune_shop",
        "Walk to the fortune teller's shop",
        on_room_changed(ROOM_FORTUNE),
    ),
    _goal(
        "state_fortune_shop",
        "Check state in the fortune teller's shop",
        all_of(in_room(ROOM_FORTUNE), on_call("state")),
        kind="call",
    ),
    _goal(
        "give_fish_to_teller",
        "Put part of the fish into the cauldron",
        on_message_contains("Put part of the fish into the cauldron"),
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
        "ask_teller_phrase",
        "Ask the fortune teller about magic phrases",
        on_message_contains("What do you know about magic phrases"),
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
