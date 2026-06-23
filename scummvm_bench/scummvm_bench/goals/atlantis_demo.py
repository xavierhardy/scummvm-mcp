"""Goal set for the Indiana Jones and the Fate of Atlantis demo (Thera).

The demo has no save: it is played as one ordered walkthrough, skipping the
opening intro until Indy and Sophia stand on the Thera dock (room 49). The full
playable arc — reconciled against a live MCP playthrough and
``test/mcp/test_atlantis.py`` — is:

  dock (49): answer the opening dialog ("Let's take a look around.") and talk to
  Sophia -> walk up the path away from the dock (Indy goes to look for Kerner
  while Sophia waits) into the canyon (room 63) -> search the mountain's openings
  (gap / notch / cleft, the jeep hides behind one AT RANDOM) for the wrecked jeep
  and take the tire repair kit -> back at the dock, read Plato's Lost Dialogue and
  turn to the page giving Thera's bearing relative to Atlantis ("the Lesser N
  miles {north/northeast/northwest} of the City"). The destination is RANDOMISED
  per playthrough, so the heading must be read from the book each time: halve...
  no -- divide the distance by ten and reverse the direction (north->south,
  northwest->southeast, northeast->southwest) to get the course from Thera ->
  talk to the salvage-boat captain, ask about Atlantis, ask him to take you, and
  give him that distance + direction; he ferries you to the dive site ("...the
  rest is up to you.") -> on the boat (room 42) open the storage locker, patch the
  punctured diving suit with the tire repair kit (consuming it), attach the air
  hose, and put the suit on -> as Sophia, switch the air compressor on and work
  the hoist to lower Indy. With the right course he sinks toward the Lost Kingdom
  (room 82, the underwater gateway to Atlantis); with the wrong one the captain
  sails back. Reaching room 82 is therefore the demo's climax and the stopping
  goal -- it requires the entire chain AND a correctly decoded heading.

NOTE: past room 82 the demo continues into a timed underwater scramble (Kerner
cuts Indy's air; find the right cave among several before the air runs out). That
finale is randomised and death-timed, so it is intentionally left out of the
scored goals; the descent into room 82 is the deterministic completion marker.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    on_call,
    on_inventory_added,
    on_inventory_removed,
    on_message_contains,
    on_question_appeared,
    on_room_changed,
)

ROOM_DOCK = 49  # Thera dock; Indy and Sophia start here
ROOM_CANYON = 63  # canyon/landscape away from the dock
ROOM_BOAT = 42  # the salvage boat at the dive site
ROOM_GATEWAY = 82  # underwater, lowered toward Atlantis (correct heading only)


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
        ),
        _goal(
            "read_dialogue",
            "Read Atlantis's bearing from Plato's Lost Dialogue",
            on_message_contains("of the City"),
        ),
        _goal(
            "board_salvage_boat",
            "Give the captain the heading; he ferries you to the dive site",
            on_message_contains("the rest is up to you"),
        ),
        _goal(
            "patch_diving_suit",
            "Patch the punctured diving suit with the tire repair kit",
            on_inventory_removed("tire_repair_kit"),
        ),
        _goal(
            "dive_to_atlantis",
            "Don the suit and hoist Indy down toward the Lost Kingdom",
            on_room_changed(ROOM_GATEWAY),
            stopping=True,
        ),
    )
}


ATLANTIS_DEMO_GOALSET = GoalSet(
    game_id="atlantis",
    save_slot=None,
    goals=GOALS,
)
