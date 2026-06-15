"""Goal set for the Maniac Mansion C64 demo, save slot 1 (outside the mansion).

Objective: get into the mansion and explore the whole ground floor -- the demo
locks the staircase ("I can't go up until you buy the game"), so the demo is the
ground floor and finishing it means visiting every reachable room. Scored as its
minimum actions and reconciled against a live playthrough (and
``test/mcp/test_maniac_c64.py``):

  outside (room 1): pull the door mat (state 0->8) to reveal the key, pick up the
  key, use it on the front door (state 4->8), walk through into the entrance hall
  (room 10), then tour the ground floor -- kitchen (7) -> dining room (37) ->
  pantry (36) -> living room (3) -> library (5, the last room, stopping goal).

Interior doors share the name "door", so they are walked by their object id
(kitchen 35, dining 49, pantry 65, living room 37, library 93; the back-tracking
ids 91/62/48 return toward the hall).
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    in_room,
    on_call,
    on_inventory_added,
    on_object_changed,
    on_room_changed,
)

ROOM_OUTSIDE = 1
ROOM_HALL = 10
ROOM_KITCHEN = 7
ROOM_DINING = 37
ROOM_PANTRY = 36
ROOM_LIVING = 3
ROOM_LIBRARY = 5


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
            "Walk through the front door into the entrance hall",
            on_room_changed(ROOM_HALL),
        ),
        _goal(
            "reach_kitchen",
            "Reach the kitchen",
            on_room_changed(ROOM_KITCHEN),
        ),
        _goal(
            "reach_dining_room",
            "Reach the dining room",
            on_room_changed(ROOM_DINING),
        ),
        _goal(
            "reach_pantry",
            "Reach the pantry",
            on_room_changed(ROOM_PANTRY),
        ),
        _goal(
            "reach_living_room",
            "Reach the living room",
            on_room_changed(ROOM_LIVING),
        ),
        _goal(
            "reach_library",
            "Reach the library (the last ground-floor room)",
            on_room_changed(ROOM_LIBRARY),
            stopping=True,
        ),
    )
}


MANIAC_C64_DEMO_GOALSET = GoalSet(
    game_id="maniac-c64",
    save_slot=1,
    goals=GOALS,
)
