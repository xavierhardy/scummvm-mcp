"""Goal set for the Indiana Jones 3 segment of Passport to Adventure, save 3.

Save slot 3 (``pass.s03``) drops Indy in the boxing gym (room 25). Reconciled
against a live capture of the SCUMM V3 demo: walking to the locker room makes the
coach offer to spar ("How would you like me to spar with you?") and opens a
five-choice dialog; choosing 1 ("Go easy on me...") starts the fist-fight and the
fight HUD appears ("Boxing Coach's Health"). Numpad-style ``keystroke`` keys then
drive Indy's punches — a high punch is key ``9``.

Whether a punch lands is up to the coach's (random) blocking, so the goals here
are deterministic: they score reaching the gym, triggering the dialog, accepting
the match, the HUD appearing, and throwing a three-punch combo — none of which
depend on actually winning the bout.
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
)

ROOM_GYM = 25
LOCKER_ROOM = "locker_room"
HIGH_PUNCH = "9"


def _goal(
    goal_id: str,
    label: str,
    predicate: Predicate,
    kind: str = "result",
    stopping: bool = False,
    times: int = 1,
) -> Goal:
    return Goal(goal_id, label, predicate, stopping=stopping, kind=kind, times=times)


GOALS = {
    g.goal_id: g
    for g in (
        _goal(
            "state_gym",
            "Check state in the boxing gym",
            all_of(in_room(ROOM_GYM), on_call("state")),
            kind="call",
        ),
        _goal(
            "approach_locker",
            "Walk over to the locker room",
            on_call("act", verb="walk to", target1=LOCKER_ROOM),
            kind="call",
        ),
        _goal(
            "coach_offers_spar",
            "Get the coach to offer a sparring match",
            on_question_appeared(),
        ),
        _goal(
            "accept_go_easy",
            "Accept the match (go easy on me)",
            on_call("answer", id=1),
            kind="call",
        ),
        _goal(
            "fight_begins",
            "Start the fight (the boxing HUD appears)",
            on_message_contains("Boxing Coach's Health"),
        ),
        _goal(
            "throw_high_punch",
            "Throw a high punch at the coach",
            on_call("keystroke", key=HIGH_PUNCH),
            kind="call",
        ),
        _goal(
            "land_three_punch_combo",
            "Press the attack with a three-punch combo",
            on_call("keystroke", key=HIGH_PUNCH),
            kind="call",
            times=3,
            stopping=True,
        ),
    )
}


PASS_INDY3_DEMO_GOALSET = GoalSet(
    game_id="pass",
    save_slot=3,
    goals=GOALS,
)
