"""Goal set for The Dig demo, save slot 1 (the starting area).

Boston Low starts in the starting area (room 15) with Brink and Maggie and a
trowel. Reconciled against a live capture of the V7 single-cursor demo:

  1. leave the start to the central hub (room 16; "Where are you going, Low?")
  2. branch off to the crashed ship's wreck exterior (room 18)
  3. enter the wreck interior (room 19) by clicking the wreck
  4. pull the hanging wire ("I'm going to pull this wire down") — this shorts
     something out and a small hole opens back in the starting area
  5. leave the wreck interior (room 18), then leave the wreck zone (room 16)
  6. take the hub's central "dias" path (obj 66) back to the starting area (15)
  7. use the trowel on the small hole that has appeared there — end of demo

Rooms 16 and 18 are each visited twice (out and back), so those transition goals
are disambiguated by the room the player was in *before* the call (``in_room``).
"""

from .engine import (
    Goal,
    GoalEvent,
    GoalSet,
    Predicate,
    all_of,
    in_room,
    on_call,
    on_message_contains,
    on_room_changed,
)

ROOM_START = 15  # starting area; the small hole opens here and you dig it
ROOM_HUB = 16  # central hub; its middle exit is the "dias" object (66 -> 15)
ROOM_WRECK_EXT = 18  # wreck exterior
ROOM_WRECK_INT = 19  # wreck interior (hanging wire, chest)

START_TO_HUB = 53  # room 15 -> 16 (clearing)
HUB_TO_WRECK = 67  # room 16 -> 18 (wreck path)
WRECK_OBJ = 81  # room 18 -> 19 (click the wreck to climb inside)
WIRE_OBJ = 85  # hanging wire in the wreck interior
WRECK_INT_EXIT = 84  # room 19 -> 18 ("outside")
WRECK_TO_HUB = 80  # room 18 -> 16 (clearing)
DIAS_CENTER = 66  # room 16 -> 15 (central "dias" path)
HOLE_OBJ = 54  # the small hole that opens in the starting area after the wire


def _used_trowel_on_hole() -> Predicate:
    """The agent successfully used the trowel on the small hole.

    Requires the call to have been accepted (``ok``): returning to the starting
    area plays a cutscene that locks input, during which dig attempts error out;
    only the accepted dig — once the hole is diggable — should latch this goal.
    """
    base = on_call("act", verb="use item", target1="trowel", target2=HOLE_OBJ)

    def predicate(event: GoalEvent) -> bool:
        return event.ok and base(event)

    return predicate


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
            "state_start",
            "Check state in the starting area",
            all_of(in_room(ROOM_START), on_call("state")),
            kind="call",
        ),
        _goal(
            "leave_to_hub",
            "Leave the starting area to the hub",
            all_of(in_room(ROOM_START), on_room_changed(ROOM_HUB)),
        ),
        _goal(
            "reach_wreck",
            "Go to the crashed ship's wreck",
            all_of(in_room(ROOM_HUB), on_room_changed(ROOM_WRECK_EXT)),
        ),
        _goal(
            "enter_wreck",
            "Climb inside the wreck",
            all_of(in_room(ROOM_WRECK_EXT), on_room_changed(ROOM_WRECK_INT)),
        ),
        _goal(
            "pull_wire",
            "Pull the hanging wire",
            all_of(in_room(ROOM_WRECK_INT), on_message_contains("pull this wire")),
        ),
        _goal(
            "leave_wreck",
            "Leave the wreck interior",
            all_of(in_room(ROOM_WRECK_INT), on_room_changed(ROOM_WRECK_EXT)),
        ),
        _goal(
            "back_to_hub",
            "Return to the hub",
            all_of(in_room(ROOM_WRECK_EXT), on_room_changed(ROOM_HUB)),
        ),
        _goal(
            "back_to_start",
            "Take the dias path back to the starting area",
            all_of(in_room(ROOM_HUB), on_room_changed(ROOM_START)),
        ),
        _goal(
            "dig_hole",
            "Dig the small hole with the trowel",
            all_of(in_room(ROOM_START), _used_trowel_on_hole()),
            kind="call",
            stopping=True,
        ),
    )
}


THE_DIG_DEMO_GOALSET = GoalSet(
    game_id="dig-demo",
    save_slot=1,
    goals=GOALS,
)
