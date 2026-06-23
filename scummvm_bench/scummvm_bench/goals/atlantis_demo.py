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
  per playthrough, so the heading must be read from the book each time: divide the
  distance by ten (Plato's tenfold error) and reverse the direction (north->south,
  northwest->southeast, northeast->southwest) to get the course from Thera ->
  talk to the salvage-boat captain, ask about Atlantis, ask him to take you, and
  give him that distance + direction; he ferries you to the dive site ("...the
  rest is up to you.") -> on the boat (room 42) open the storage locker, patch the
  punctured diving suit with the tire repair kit (consuming it), attach the air
  hose, and put the suit on -> as Sophia, switch the air compressor on and work
  the hoist to lower Indy. With the right course he sinks toward the Lost Kingdom
  (room 82, the underwater gateway); with the wrong one the captain sails back.
  Reaching room 82 (the preceding milestone) needs the entire chain AND a
  correctly decoded heading -> Kerner then betrays Indy and cuts his air, leaving
  a timed underwater scramble: one of seven cave doorways (the entrance is chosen
  AT RANDOM each dive) leads inside. Walk into the right cave before the air runs
  out and the Atlantis airlock (room 48) loads (take too long and Indy drowns --
  the Guybrush Threepwood game-over gag) -> in the dark airlock, take the ladder
  and stand it against the stone rubble to climb up, open the stone box and take
  the metal rod, use an orichalcum bead on the rod to light it, then use a bead on
  the sentry statue's mouth to swing the bronze door open. Walking through the
  open door is the demo's true end: Indy muses "What ancient secrets lie beyond
  this portal..." and the demo bows out (systemOps -> back to the attract intro).
  That line is the stopping goal.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    on_inventory_added,
    on_inventory_removed,
    on_message_contains,
    on_room_changed,
)

ROOM_DOCK = 49  # Thera dock; Indy and Sophia start here
ROOM_CANYON = 63  # canyon/landscape away from the dock
ROOM_BOAT = 42  # the salvage boat at the dive site
ROOM_GATEWAY = 82  # underwater, lowered toward Atlantis (correct heading only)
ROOM_ATLANTIS = 48  # the Atlantis airlock (through the correct cave)


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
        ),
        _goal(
            "reach_atlantis",
            "Find the right cave before the air runs out and reach the airlock",
            on_room_changed(ROOM_ATLANTIS),
        ),
        _goal(
            "open_airlock_box",
            "Stand the ladder on the rubble and open the airlock's stone box",
            on_message_contains("it opens"),
            # "Hey! It opens!" — the stone box, reached by climbing the rubble; it
            # holds the rod that (lit with a bead) lights the airlock so a bead can
            # be used on the sentry statue to swing the bronze door open.
        ),
        _goal(
            "enter_atlantis",
            "Step through the bronze door into Atlantis (demo end)",
            on_message_contains("ancient secrets"),
            stopping=True,
        ),
    )
}


ATLANTIS_DEMO_GOALSET = GoalSet(
    game_id="atlantis",
    save_slot=None,
    goals=GOALS,
)
