"""Goal set for The Dig demo, save slot 1 (the canyon).

Boston Low starts in the canyon (room 15) with Brink and Maggie and a trowel.
Reconciled against a live capture of the V7 single-cursor demo: leave the canyon
to the dias hub (room 16, "Where are you going, Low?"), branch off to the crashed
ship's wreck (room 18), then to the bone field (room 20) and dig the grave with
the trowel — Low complains about "digging around these ancient bones" and the
grave turns into exposed bones.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    in_room,
    on_call,
    on_message_contains,
    on_room_changed,
)

ROOM_CANYON = 15
ROOM_DIAS = 16
ROOM_WRECK = 18
ROOM_BONES = 20
WRECK_OBJ = 81


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
            "state_canyon",
            "Check state in the canyon",
            all_of(in_room(ROOM_CANYON), on_call("state")),
            kind="call",
        ),
        _goal(
            "leave_to_dias",
            "Leave the canyon to the dias",
            on_room_changed(ROOM_DIAS),
        ),
        _goal(
            "state_dias",
            "Check state at the dias",
            all_of(in_room(ROOM_DIAS), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_wreck",
            "Go to the crashed ship's wreck",
            on_room_changed(ROOM_WRECK),
        ),
        _goal(
            "interact_wreck",
            "Interact with the wreck",
            all_of(in_room(ROOM_WRECK), on_call("act", verb="interact", target1=WRECK_OBJ)),
            kind="call",
        ),
        _goal(
            "reach_bone_field",
            "Go to the bone field",
            on_room_changed(ROOM_BONES),
        ),
        _goal(
            "state_bone_field",
            "Check state in the bone field",
            all_of(in_room(ROOM_BONES), on_call("state")),
            kind="call",
        ),
        _goal(
            "dig_bones",
            "Dig the grave with the trowel",
            on_message_contains("ancient bones"),
            stopping=True,
        ),
    )
}


THE_DIG_DEMO_GOALSET = GoalSet(
    game_id="dig-demo",
    save_slot=1,
    goals=GOALS,
)
