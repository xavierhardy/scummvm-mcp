"""Goal set for the Indiana Jones and the Fate of Atlantis demo (Thera).

The demo has no save: the run skips the opening intro until Indy and Sophia
stand on the Thera dock (room 75), where an opening "what's the plan?" dialog is
waiting. The Thera segment, scored as its minimum actions:

  dock (room 75): answer the opening dialog ("Let's take a look around.") and
  talk to Sophia -> walk up the path away from the dock; Indy heads off to look
  for Kerner while Sophia waits, moving to the canyon/path (room 63) -> walk to
  the cleft in the mountain (room 69), where a wrecked truck sits by a collapsed
  entrance -> pick up the tire repair kit (``tire_repair_kit``, stopping goal).

Reconciled against the repo's live MCP capture (``test/mcp/test_atlantis.py``
and the captured logs): the room ids (dock 75, canyon 63, cleft 69), the opening
dialog, the "look around" / Kerner lines and the ``tire_repair_kit`` pickup are
all observed there. The mountain entrance answers to several names
("notch in mountain" / "cleft in mountain" / "gap in mountain"); only the
resulting room change (69) is scored, so the exact phrasing does not matter.
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

ROOM_DOCK = 75  # Thera dock; Indy and Sophia start here
ROOM_CANYON = 63  # path/canyon away from the dock
ROOM_CLEFT = 69  # cleft in the mountain, with the wrecked truck


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
            "reach_mountain_cleft",
            "Walk to the cleft in the mountain",
            on_room_changed(ROOM_CLEFT),
        ),
        _goal(
            "get_tire_repair_kit",
            "Pick up the tire repair kit by the wrecked truck",
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
