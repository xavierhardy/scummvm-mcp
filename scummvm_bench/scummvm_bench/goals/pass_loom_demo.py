"""Goal set for the Loom segment of Passport to Adventure (the full demo).

Loaded from save slot 6 (``pass.s06``): the very first Loom scene, before Bobbin
has the distaff -- the cliff-top "first room" (room 36) with the sky and the last
leaf. From there the demo runs (reconciled against a live capture):

  first room (36) --walk left--> village (39) --> tents hub (41) --> the elders'
  tent (44); walk to the Elders on the right to trigger the High Council scene
  (room 45), where Hetchel is turned into the cygnet's egg and Bobbin is left
  with the Elders' distaff. Listen to the egg ("It's trying to open!") and replay
  its Opening draft e-c-e-d ("Thank goodness you're still here."). Then: the
  forest (40) and its owl-holes; the other tent (38) and its darkness, which the
  Hole draft c-c-c-c opens into room 42 (the book + dye pot, dye something green);
  back to the first room to open the sky with e-c-e-d; then the dock, and use the
  tree to leave the island.

Single-cursor model: navigation is ``interact`` on ``pathway_<id>`` objects
(Bobbin's walk is interrupted at intermediate stand points, so each pathway needs
several interact clicks); drafts are cast with the ``play_note`` tool, e.g.
``play_note(notes=["e","c","e","d"])``.

Room ids reconciled live:
  * first room / cliff (sky 463)   = 36   (control starts here; also the leave point)
  * village                        = 39   (pathway 460 from 36)
  * tents hub                      = 41   (pathway 510 from 39)
  * elders' tent                   = 44   (pathway 541 from 41)
  * council / egg / loom           = 45   (walk to the Elders (actor 8) in 44)
  * forest (owl-holes)             = 40   [given by spec]
  * other tent (darkness 952)      = 38   (pathway 539 from 41)
  * inside the darkness            = 42   (cast c-c-c-c on the darkness in 38)

RECONCILE (still being confirmed live): the navigation to the forest (40), the
dock room id, and the exact distaff-pickup / book / dye-pot / dye-green /
open-sky observables. Those goals use the spec'd room ids and message text for
now and are flagged below.
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

ROOM_FIRST = 36       # cliff with the sky (control starts here; leave point too)
ROOM_VILLAGE = 39
ROOM_TENTS = 41       # the tents hub
ROOM_TENT_LEFT = 44   # the Elders' tent
ROOM_COUNCIL = 45     # the High Council / egg / loom
ROOM_FOREST = 40      # the owl-holes
ROOM_OTHER_TENT = 38  # holds the "darkness"
ROOM_DARKNESS = 42    # opened by casting the owls' draft on the darkness
ROOM_BEACH = 46       # the beach / dock; the tree (pathway 625) appears here
                      # once the sky is open and leads back to 36 (leaving)

PATH_TO_VILLAGE = 460     # 36 -> 39
PATH_TO_TENTS = 510       # 39 -> 41
PATH_TO_TENT_LEFT = 541   # 41 -> 44
PATH_TO_OTHER_TENT = 539  # 41 -> 38
ELDERS_ACTOR = 8          # walk to the Elders (room 44) -> council
EGG_OBJ = 609
DARKNESS_OBJ = 952


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
            "state_first_room",
            "Check state in the first room (the cliff with the sky)",
            all_of(in_room(ROOM_FIRST), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_village",
            "Get to another room (walk left out of the first room)",
            on_room_changed(ROOM_VILLAGE),
        ),
        _goal(
            "state_village",
            "Check state in the second room (the village)",
            all_of(in_room(ROOM_VILLAGE), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_tents",
            "Walk to the tents",
            on_room_changed(ROOM_TENTS),
        ),
        _goal(
            "state_tents",
            "Check state in the third room (the tents)",
            all_of(in_room(ROOM_TENTS), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_tent_left",
            "Go into the tent on the left",
            on_room_changed(ROOM_TENT_LEFT),
        ),
        _goal(
            "state_tent",
            "Check state inside the tent",
            all_of(in_room(ROOM_TENT_LEFT), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_council",
            "Walk to the Elders on the right (triggers the High Council)",
            on_room_changed(ROOM_COUNCIL),
        ),
        _goal(
            "pick_up_distaff",
            "Pick up the Elders' distaff",
            # RECONCILE: the distaff is acquired during the council; the clearest
            # confirmation seen so far is Hetchel's "...the Elder's distaff..." line.
            on_message_contains("distaff"),
        ),
        _goal(
            "listen_egg",
            'Interact with the egg: "It\'s trying to open!"',
            on_message_contains("It's trying to open"),
        ),
        _goal(
            "play_eced_egg",
            'Play the Opening draft (e-c-e-d) on the egg: "Thank goodness you\'re still here!"',
            on_message_contains("Thank goodness you're still here"),
        ),
        _goal(
            "reach_forest",
            "Go to the forest",
            on_room_changed(ROOM_FOREST),
        ),
        _goal(
            "owl_in_there",
            'Find an owl in a hole: "There\'s an owl in there!"',
            on_message_contains("There's an owl in there"),
        ),
        _goal(
            "owl_too",
            'Find another owl: "This hole has an owl too."',
            on_message_contains("has an owl too"),
        ),
        _goal(
            "owl_another",
            'Find another owl: "Another owl! The woods must be full of \'em."',
            on_message_contains("Another owl"),
        ),
        _goal(
            "owls_all_full",
            'All the holes are filled: "Looks as if all the holes are full."',
            on_message_contains("the holes are full"),
        ),
        _goal(
            "reach_other_tent",
            "Go to the other tent",
            on_room_changed(ROOM_OTHER_TENT),
        ),
        _goal(
            "state_other_tent",
            "Check state in the other tent",
            all_of(in_room(ROOM_OTHER_TENT), on_call("state")),
            kind="call",
        ),
        _goal(
            "play_darkness_draft",
            "Cast the owls' draft on the darkness (opens room 42)",
            on_room_changed(ROOM_DARKNESS),
        ),
        _goal(
            "interact_book",
            'Read the book ("This is the Book of Patterns...")',
            on_message_contains("Book of Patterns"),
        ),
        _goal(
            "interact_dye_pot",
            'Examine the dye pot (learns the dye draft)',
            on_message_contains("dye draft"),
        ),
        _goal(
            "dye_green",
            'Dye the heap green ("I changed the color!")',
            on_message_contains("changed the color"),
        ),
        _goal(
            "open_sky",
            "Cast the Opening draft on the sky (to the right of the first room)",
            all_of(in_room(ROOM_FIRST), on_message_contains("real game")),
        ),
        _goal(
            "reach_dock",
            "Go to the dock (the beach), where the tree now stands",
            on_room_changed(ROOM_BEACH),
        ),
        _goal(
            "leave_island",
            "Use the tree to leave the island (end of demo)",
            on_room_changed(ROOM_FIRST),
            stopping=True,
            times=2,  # first re-entry to 36 is for the sky; the 2nd (from the
            # beach, via the tree) is leaving the island
        ),
    )
}


PASS_LOOM_DEMO_GOALSET = GoalSet(
    game_id="pass",
    save_slot=6,
    goals=GOALS,
)
