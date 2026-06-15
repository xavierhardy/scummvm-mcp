"""Goal set for the Maniac Mansion C64 demo, save slot 1 (outside the mansion).

Objective: get into the mansion and reach the demo's wall. Scored as its minimum
actions and reconciled against a live playthrough (and
``test/mcp/test_maniac_c64.py``): pull the door mat (state 0->8) to reveal the
key, pick up the key, use it on the front door (state 4->8), and walk through
into the entrance hall (room 10). Getting inside is NOT the end -- you can roam
the ground floor (a door off the hall opens onto the kitchen, room 7), but the
staircase is walled off: walking to it ends the demo with the kid saying
"I can't go up until you buy the game." That message is the stopping goal.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    in_room,
    on_call,
    on_inventory_added,
    on_message_contains,
    on_object_changed,
    on_room_changed,
)

ROOM_OUTSIDE = 1
ROOM_INSIDE = 10


def _goal(
    goal_id: str,
    label: str,
    predicate: Predicate,
    kind: str = "result",
    stopping: bool = False,
) -> Goal:
    return Goal(goal_id, label, predicate, stopping=stopping, kind=kind)


GOALS = {
    g.goal_id: g
    for g in (
        _goal(
            "state_outside",
            "Check state outside the mansion",
            all_of(in_room(ROOM_OUTSIDE), on_call("state")),
            kind="call",
        ),
        _goal(
            "pull_door_mat",
            "Pull the door mat (reveals the key)",
            on_object_changed("door mat"),
        ),
        _goal(
            "pick_up_key",
            "Pick up the key",
            on_inventory_added("key"),
        ),
        _goal(
            "use_key_on_door",
            "Use the key on the front door",
            all_of(
                on_call("act", verb="use", target1="key", target2="front_door"),
                on_object_changed("front door"),
            ),
            kind="call",
        ),
        _goal(
            "enter_mansion",
            "Walk through the front door into the mansion",
            on_room_changed(ROOM_INSIDE),
        ),
        _goal(
            "reach_demo_wall",
            "Walk to the stairs and hit the demo wall (buy the game)",
            on_message_contains("buy the game"),
            stopping=True,
        ),
    )
}


MANIAC_C64_DEMO_GOALSET = GoalSet(
    game_id="maniac-c64",
    save_slot=1,
    goals=GOALS,
)
