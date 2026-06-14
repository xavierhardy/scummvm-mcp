"""Goal set for the Sam & Max Hit the Road demo, save slot 1 (the office).

Sam & Max start in the detectives' office (room 7). Reconciled against a live
capture: head downstairs to the office staircase (room 8) where a TV brawl
cutscene plays ("So, you want a piece of me, huh!"), out to the street (room 9),
use Max on the kitten to open its topic dialog and ask it the "question" topic —
which reveals the swallowed message from the Commissioner — then board the
DeSoto, which drives off (room 10).
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    in_room,
    on_call,
    on_message_contains,
    on_question_appeared,
    on_room_changed,
)

ROOM_OFFICE = 7
ROOM_STAIRCASE = 8
ROOM_STREET = 9
ROOM_DRIVING = 10


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
            "state_office",
            "Check state in the office",
            all_of(in_room(ROOM_OFFICE), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_staircase",
            "Go down to the office staircase",
            on_room_changed(ROOM_STAIRCASE),
        ),
        _goal(
            "state_staircase",
            "Check state on the office staircase",
            all_of(in_room(ROOM_STAIRCASE), on_call("state")),
            kind="call",
        ),
        _goal(
            "trigger_cutscene",
            "Trigger the staircase TV brawl cutscene",
            on_message_contains("you want a piece of me"),
        ),
        _goal(
            "reach_street",
            "Walk out to the street",
            on_room_changed(ROOM_STREET),
        ),
        _goal(
            "state_street",
            "Check state in the street",
            all_of(in_room(ROOM_STREET), on_call("state")),
            kind="call",
        ),
        _goal(
            "talk_to_cat",
            "Use Max on the kitten to open its dialog",
            all_of(in_room(ROOM_STREET), on_question_appeared()),
        ),
        _goal(
            "ask_cat_commissioner",
            "Ask the kitten and learn of the Commissioner's message",
            on_message_contains("Commissioner"),
        ),
        _goal(
            "use_desoto",
            "Board the DeSoto and drive off",
            on_room_changed(ROOM_DRIVING),
            stopping=True,
        ),
    )
}


SAMNMAX_DEMO_GOALSET = GoalSet(
    game_id="samnmax",
    save_slot=1,
    goals=GOALS,
)
