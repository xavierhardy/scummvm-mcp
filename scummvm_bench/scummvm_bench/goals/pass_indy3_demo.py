"""Goal set for the Indiana Jones (Last Crusade) segment of Passport to Adventure.

Passport to Adventure boots to a passport menu (you click a book to pick Loom /
Monkey Island / Indiana Jones), so the bench loads a save state instead. Save
slot 3 (``pass.s03``) is the Indy3 interactive start: the demo's intro cutscene
(rooms 26 -> 24 -> 20) hands the player control in the boxing gym (room 25), the
"first room" of this walkthrough.

Room ids reconciled against a live capture of the SCUMM V3 demo:
  * gym / first room       = 25   (control starts here)
  * corridor               = 20   (gym door 213 -> here; doors 100/101/102/103)
  * outside / travel hub   = 24   (corridor door 100 -> here; has the 'travel' verb)
  * Henry's house          = 27   (travel destination)
  * office (student mob)    = 22   (corridor door 103, the one "next to the gym")
  * Indy's office          = 21   (entered after the student mob disperses)
The corridor's other doors (101/102) are locked classrooms ("class in session").

Full solve (reconciled live): leave the gym, cross the corridor, go outside
(sit through the Donovan cutscene), travel to Henry's house and ransack it
(plant / pull cloth -> chest / pull bookcase -> sticky tape); back through the
corridor's door 103 into the office, calm the student mob (work-something-out ->
please-relax -> Irene-take-down-names) which opens Indy's office; there open the
window, take the mail and open the package (grail diary), and use the sticky
tape on the solvent jar to fish out the small key; travel back to Henry's, use
the key on the chest for the old book and pick up the painting -- Indy then says
he "has everything he needs" for the Grail quest, which adds "To the Plane to
Venice" to the travel menu back outside; travel there to end the demo.

The student dialog in this build runs work-something-out -> please-relax ->
take-down-names; the "talk about this calmly" line is never offered, so it is
intentionally not a goal here.

NOTE:
 * Venice is room 28. Travel to it only unlocks once Indy holds the grail diary,
   the old book AND the painting (any one of them, picked up last, triggers the
   "everything I need" line and the menu entry). ``travel_venice`` matches the
   room change into Venice (not a "Venice" *message* -- the intro's Donovan
   cutscene says "...it is in Venice, Italy..." and would otherwise latch during
   the intro).
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
    on_object_changed,
    on_room_changed,
)

ROOM_FIRST = 25     # boxing gym — where control starts
ROOM_CORRIDOR = 20
ROOM_OUTSIDE = 24   # the travel hub
ROOM_OFFICE = 22    # the student-mob office
ROOM_INDY_OFFICE = 21
ROOM_HENRY = 27
ROOM_VENICE = 28    # confirmed live (reached after picking up the painting)

DOOR_LEFT = 100     # corridor -> outside
DOOR_GYM = 103      # corridor door "next to the gym" -> the office (room 22)


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
            "Check state in the first room (the gym)",
            all_of(in_room(ROOM_FIRST), on_call("state")),
            kind="call",
        ),
        _goal(
            "reach_corridor",
            "Walk through the door into the corridor",
            on_room_changed(ROOM_CORRIDOR),
        ),
        _goal(
            "state_corridor",
            "Check state in the corridor",
            all_of(in_room(ROOM_CORRIDOR), on_call("state")),
            kind="call",
        ),
        _goal(
            "open_door_left",
            "Open the door on the left",
            on_call("act", verb="open", target1=DOOR_LEFT),
            kind="call",
        ),
        _goal(
            "reach_outside",
            "Walk through the left door to the outside",
            on_room_changed(ROOM_OUTSIDE),
        ),
        _goal(
            "state_outside",
            "Check state outside",
            all_of(in_room(ROOM_OUTSIDE), on_call("state")),
            kind="call",
        ),
        _goal(
            "travel_henry",
            "Travel to Henry's House",
            on_room_changed(ROOM_HENRY),
        ),
        _goal(
            "plant_moved",
            "Move the plant (its state changes to 1)",
            on_object_changed("plant"),
        ),
        _goal(
            "cloth_pulled",
            "Pull the table cloth (its state changes to 1)",
            on_object_changed("table cloth"),  # objects_changed reports a space, not "_"
        ),
        _goal(
            "bookcase_pulled",
            "Pull the bookcase (its state changes to 1)",
            on_object_changed("bookcase"),
        ),
        _goal(
            "pick_up_sticky_tape",
            "Pick up the sticky tape",
            on_inventory_added("sticky_tape"),
        ),
        _goal(
            "open_door_gym",
            "In the corridor, open the door (103) next to the gym",
            on_call("act", verb="open", target1=DOOR_GYM),
            kind="call",
        ),
        _goal(
            "reach_office_via_103",
            "In the corridor, go through the door (103) next to the gym (into the office)",
            on_room_changed(ROOM_OFFICE),
        ),
        _goal(
            "state_office",
            "Check state in the office (room 22)",
            all_of(in_room(ROOM_OFFICE), on_call("state")),
            kind="call",
        ),
        _goal(
            "talk_to_students",
            "In the office, talk to the students",
            all_of(in_room(ROOM_OFFICE), on_call("act", verb="talk to")),
            kind="call",
        ),
        _goal(
            "office_line_moment",
            'Office line: "Just a moment, folks. I\'m sure we can work something out."',
            on_message_contains("we can work something out"),
        ),
        _goal(
            "office_line_relax",
            'Office line: "Please relax. I have a solution that is fair for everyone."',
            on_message_contains("a solution that is fair for everyone"),
        ),
        _goal(
            "office_line_names",
            'Office line: "Irene, take down names and I will see everyone in order."',
            on_message_contains("take down names"),
        ),
        _goal(
            "state_indy_office",
            "Check state in Indy's office (room 21)",
            all_of(in_room(ROOM_INDY_OFFICE), on_call("state")),
            kind="call",
        ),
        _goal(
            "open_window",
            "In Indy's office, open the window (state changes to 1)",
            all_of(in_room(ROOM_INDY_OFFICE), on_object_changed("window")),
        ),
        _goal(
            "pick_up_junk_mail",
            "In Indy's office, pick up the junk mail",
            on_inventory_added("junk_mail"),
        ),
        _goal(
            "pick_up_letters",
            "In Indy's office, pick up the letters",
            on_inventory_added("letters"),
        ),
        _goal(
            "pick_up_papers",
            "In Indy's office, pick up the papers",
            on_inventory_added("papers"),
        ),
        _goal(
            "pick_up_package",
            "In Indy's office, pick up the package",
            on_inventory_added("package"),
        ),
        _goal(
            "open_package",
            "In Indy's office, open the package",
            on_object_changed("package"),
        ),
        _goal(
            "obtain_small_key",
            "In Indy's office, use the sticky tape on the solvent jar to get the key",
            # The key is added asynchronously (no inventory_added in the act
            # result), so match the "There's a key in here!" line instead.
            on_message_contains("key in here"),
        ),
        _goal(
            "state_henry",
            "Check state in Henry's house",
            all_of(in_room(ROOM_HENRY), on_call("state")),
            kind="call",
        ),
        _goal(
            "use_key_on_chest",
            "In Henry's house, use the small key on the chest",
            on_object_changed("chest"),
        ),
        _goal(
            "pick_up_old_book",
            "In Henry's house, pick up the old book",
            on_inventory_added("old_book"),
        ),
        _goal(
            "pick_up_painting",
            "In Henry's house, pick up the painting (unlocks the trip to Venice)",
            # Picking this up makes Indy say he now has everything he needs for
            # the Grail quest, which is what adds "To the Plane to Venice" to the
            # travel menu back outside.
            on_inventory_added("painting"),
        ),
        _goal(
            "travel_venice",
            "Travel on to Venice",
            on_room_changed(ROOM_VENICE),
            stopping=True,
        ),
    )
}


PASS_INDY3_DEMO_GOALSET = GoalSet(
    game_id="pass",
    save_slot=3,
    goals=GOALS,
)
