"""Goal set for the Monkey Island 2: LeChuck's Revenge demo (Scabb Island).

The MI2 demo is a *rolling* demo: pressing skip (escape) past the intro hands
the player control of Guybrush in Woodtick on Scabb Island. The demo's Scabb
Island slice, scored as its minimum actions:

  skip the rolling intro to take control in Woodtick -> pick up the
  "No Trezer Huntin zone" sign, which breaks off into a ``shovel`` -> enter the
  bar and talk to the barkeep -> ask "How's business?" (he blames Largo) ->
  Largo LaGrande barges in and shakes the barkeep down for protection money
  ("Fork over the dough...", stopping goal).

UNLIKE the other goal sets, this one is NOT reconciled against a live MCP
capture (the demo has no test or captured log in this repo yet). The beats and
strings are derived from the GameFAQs walkthrough and game script (the demo's
own dialogue is cut shorter than the retail game, e.g. the barkeep's voodoo-doll
line is absent), so the message text and object/verb names below are best-effort
and should be reconciled against a live run before relying on the scores. The
predicates deliberately avoid hard-coded room ids (none are known yet) in favour
of the skip/inventory/dialog beats, which are the most portable across that
reconciliation.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    on_call,
    on_inventory_added,
    on_message_contains,
    on_question_appeared,
)


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
            "skip_intro",
            "Skip the rolling intro to take control in Woodtick",
            on_call("skip"),
            kind="call",
        ),
        _goal(
            "get_shovel",
            "Pick up the 'No Trezer Huntin zone' sign (it becomes a shovel)",
            on_inventory_added("shovel"),
        ),
        _goal(
            "talk_to_barkeep",
            "Talk to the barkeep in the bar",
            all_of(
                on_call("act", verb="talk_to", target1="bartender"),
                on_question_appeared(),
            ),
            kind="call",
        ),
        _goal(
            "ask_business",
            "Ask the barkeep how business is",
            on_message_contains("How's business"),
        ),
        _goal(
            "largo_shakedown",
            "Largo barges in and shakes the barkeep down",
            on_message_contains("Fork over the dough"),
            stopping=True,
        ),
    )
}


MONKEY2_DEMO_GOALSET = GoalSet(
    game_id="monkey2-demo",
    save_slot=None,
    goals=GOALS,
)
