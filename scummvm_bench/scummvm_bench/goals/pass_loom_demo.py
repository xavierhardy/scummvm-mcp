"""Goal set for the Loom segment of Passport to Adventure, save slot 2.

Save slot 2 (``pass.s02``) drops Bobbin in the tree clearing (room 36) with the
last leaf of the year still on the branch. The distaff is not yet in Bobbin's
hands in this save (``play_note`` is inert), so the goals here exercise the
single-cursor *navigation* model rather than spellcasting. Reconciled against a
live capture: knock the last leaf down ("Last leaf of the year."), then follow
the westward pathways through the village (room 39) and the crossroads (room 41)
to the dark clearing (room 38).

Bobbin's walk is interrupted at intermediate stand points, so each pathway needs
several ``interact`` clicks before the room changes; the live harness retries
until the transition fires, while the mock script returns each transition once.
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

ROOM_CLEARING = 36
ROOM_VILLAGE = 39
ROOM_CROSSROADS = 41
ROOM_DARK = 38
LEAF_OBJ = 461
PATH_TO_VILLAGE = 460
PATH_TO_CROSSROADS = 510
PATH_TO_DARK = 539


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
            "state_clearing",
            "Check state in the tree clearing",
            all_of(in_room(ROOM_CLEARING), on_call("state")),
            kind="call",
        ),
        _goal(
            "fell_last_leaf",
            "Knock the last leaf off the tree",
            on_message_contains("Last leaf of the year"),
        ),
        _goal(
            "reach_village",
            "Follow the pathway west to the village",
            on_room_changed(ROOM_VILLAGE),
        ),
        _goal(
            "state_village",
            "Check state in the village",
            all_of(in_room(ROOM_VILLAGE), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_crossroads",
            "Continue west to the crossroads",
            on_room_changed(ROOM_CROSSROADS),
        ),
        _goal(
            "state_crossroads",
            "Check state at the crossroads",
            all_of(in_room(ROOM_CROSSROADS), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_dark_clearing",
            "Reach the dark clearing",
            on_room_changed(ROOM_DARK),
            stopping=True,
        ),
    )
}


PASS_LOOM_DEMO_GOALSET = GoalSet(
    game_id="pass",
    save_slot=2,
    goals=GOALS,
)
