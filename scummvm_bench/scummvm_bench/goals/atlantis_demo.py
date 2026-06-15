"""Goal set for the Indiana Jones and the Fate of Atlantis demo (Thera).

The demo has no save: the run skips the opening intro until Indy and Sophia
stand on the Thera dock (room 49), where an opening "what's the plan?" dialog is
waiting. The Thera segment with Sophia, scored as its minimum actions:

  dock (room 49): answer the opening dialog ("Let's take a look around.") and
  talk to Sophia -> walk up the path away from the dock; Indy heads off to look
  for Kerner while Sophia waits, moving to the canyon (room 63) -> search the
  mountain's openings (gap / notch / cleft) for the wrecked jeep and grab the
  tire repair kit (``tire_repair_kit``, the Thera objective and stopping goal).

Reconciled against a live playthrough (and ``test/mcp/test_atlantis.py``): the
dock (49), the canyon (63), the opening "look around" / Kerner lines and the
``tire_repair_kit`` pickup are all observed there. The jeep is hidden behind one
of the three mountain openings AT RANDOM each playthrough, so the room it sits in
varies -- the goal therefore keys on the tire-kit pickup (constant) rather than a
fixed room id.

NOTE: the tire kit is NOT the demo's true end -- the demo keeps going past it,
but the continuation could not be reached over the MCP (the dig interior, room 72
``th-dig-i``, is behind a collapsed entrance that stays blocked; the salvage-boat
captain only ever opens Sophia's "where are you going?" dialog and never sails).
The stopping goal is provisional, pending a live run that finds how the demo ends.
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
    on_room_changed,
)

ROOM_DOCK = 49  # Thera dock; Indy and Sophia start here
ROOM_CANYON = 63  # canyon/landscape away from the dock


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
            "state_dock",
            "Check state on the Thera dock",
            all_of(in_room(ROOM_DOCK), on_call("state")),
            kind="call",
        ),
        _goal(
            "answer_opening",
            "Answer the opening dialog (take a look around)",
            on_message_contains("look around"),
        ),
        _goal(
            "talk_to_sophia",
            "Talk to Sophia on the dock",
            all_of(
                on_call("act", verb="talk_to", target1="sophia"),
                on_question_appeared(),
            ),
            kind="call",
        ),
        _goal(
            "reach_canyon",
            "Walk up the path away from the dock (look for Kerner)",
            on_room_changed(ROOM_CANYON),
        ),
        _goal(
            "get_tire_repair_kit",
            "Find the jeep behind the mountain and grab the tire repair kit",
            on_inventory_added("tire_repair_kit"),
            stopping=True,
        ),
    )
}


ATLANTIS_DEMO_GOALSET = GoalSet(
    game_id="atlantis",
    save_slot=None,
    goals=GOALS,
)
