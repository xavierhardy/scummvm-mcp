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
  * gym (re-entered)       = 25   (corridor door 103, "next to gym")
Office (room 22, the student mob) and Indy's office (room 21) sit behind the
corridor's "class in session" doors (100..103) and were provided directly.

Goals score the whole demo arc: leave the gym, cross the corridor and head
outside, travel to Henry's house, ransack it (plant / cloth / bookcase / sticky
tape and, with the small key, the chest -> old book), handle the student mob in
the office and the mail in Indy's office, and finally travel on to Venice.

NOTE: reaching Venice needs the full grail-diary puzzle, which is too deep to
capture here, so ``ROOM_VENICE`` below is a best-effort placeholder. The final
goal matches on the room change into Venice (not on a "Venice" *message* — the
intro's Donovan cutscene says "...it is in Venice, Italy..." and would otherwise
latch the goal during the intro). Reconcile ``ROOM_VENICE`` against a live
capture once the demo is driven all the way to Venice.
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
ROOM_VENICE = 28    # BEST-EFFORT placeholder — reconcile against a live capture

DOOR_LEFT = 100     # corridor -> outside
DOOR_GYM = 103      # corridor -> gym ("next to gym")


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
            on_object_changed("table_cloth"),
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
            "reach_gym_via_103",
            "In the corridor, go through the door (103) next to the gym",
            on_room_changed(ROOM_FIRST),
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
            "office_line_calmly",
            'Office line: "Hey, take it easy, let\'s talk about this calmly."',
            on_message_contains("let's talk about this calmly"),
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
            "In Indy's office, obtain the small key",
            on_inventory_added("small_key"),
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
