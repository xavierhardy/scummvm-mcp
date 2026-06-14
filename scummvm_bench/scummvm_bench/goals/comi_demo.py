"""Goal set for the Curse of Monkey Island demo, save slot 1 (the cannon scene).

Guybrush is captive in the cannon room (3) of LeChuck's ship, guarded by a
"small pirate". Reconciled against a live capture of the demo's branching dialog:
talk to the pirate, needle him about his hook/beard until he slips and reveals
he is actually Wally ("Wally!"), wheedle a pirate-philosophy leaflet
(``pirate_literature``) out of him, then call him a failure as a pirate, which
makes him threaten you ("...I'll do ya in!").

The pirate dialog is non-deterministic in *which* topic finally triggers the
recognition, so the real walkthrough navigates it dynamically; the goals here are
all observable end-states (messages / inventory) independent of that path.
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
    on_question_appeared,
)

ROOM_CANNON = 3


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
            "state_cannon_scene",
            "Check state in the cannon room",
            all_of(in_room(ROOM_CANNON), on_call("state")),
            kind="call",
        ),
        _goal(
            "talk_to_pirate",
            "Talk to the small pirate",
            all_of(
                on_call("act", verb="talk_to", target1="small_pirate"),
                on_question_appeared(),
            ),
            kind="call",
        ),
        _goal(
            "recognize_wally",
            "Recognise the pirate as Wally",
            on_message_contains("Wally!"),
        ),
        _goal(
            "get_leaflet",
            "Get the pirate-philosophy leaflet",
            on_inventory_added("pirate_literature"),
        ),
        _goal(
            "call_pirate_failure",
            "Tell the pirate he is a failure",
            on_message_contains("You're a failure as a pirate"),
        ),
        _goal(
            "pirate_threatens",
            "Provoke the pirate into threatening you",
            on_message_contains("do ya in"),
            stopping=True,
        ),
    )
}


COMI_DEMO_GOALSET = GoalSet(
    game_id="comi-demo",
    save_slot=1,
    goals=GOALS,
)
