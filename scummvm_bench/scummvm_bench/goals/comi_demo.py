"""Goal set for the Curse of Monkey Island demo, save slot 1 (escape the hold).

The demo opens with Guybrush a captive in the cannon room (room 3) of LeChuck's
ship, guarded by a "small pirate" who turns out to be his old friend Wally. The
whole demo is the escape, scored here as its minimum actions:

  talk to the pirate and needle him until he lets slip he is Wally ("Wally!"),
  wheedle a pirate-philosophy leaflet (``pirate_literature``) out of him, then
  fire the cannon (``use cannon``) to drop into the minigame and sink the four
  skeleton war-canoes. Call the pirate a failure until he drops his plastic
  hook; pick up the dropped ``plastic_hook`` and the ``ramrod``, combine them
  into a ``gaff``, and -- now that the boats are wreckage -- fish the ``debris``
  out of the water (landing a ``cutlass`` and a ``skeleton_arm``). Cut the
  ``cannon_restraint_rope`` with the cutlass and fire the cannon once more: with
  nothing to hold it down it backfires Guybrush out in the closing cutscene,
  which ends the demo (the stopping goal).

The cannon room and pirate (room 3, ``small_pirate``), the dialog lines, the
``ramrod`` pickup, the gaff combine (ramrod + plastic_hook) and the gaff-on-
debris fish (-> ``cutlass`` + ``skeleton_arm``) are reconciled against the
repo's live MCP captures (``test/mcp/test_comi.py`` and ``test/mcp/test_comi_s3.py``);
the cannon minigame (``shoot_cannon`` aimed at the ``boat_N`` targets in state,
``boats_remaining`` counting down to 0) against ``test/mcp/test_comi_cannon.py``.
The pirate dialog is non-deterministic in which topic triggers each beat, so the
dialog goals are observable end-states (messages / inventory) independent of the
path.

Note there is no separate "treasure room": the whole demo plays out across
rooms 3 (cannon), 4 (the minigame) and 5 (the gunport). The final escape is a
``use cannon`` that triggers the closing cutscene *without* a room change, so it
is scored as a tool call (``escape_via_cannon``), told apart from the minigame's
``use cannon`` by the cutlass already being in hand by then.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    in_inventory,
    in_room,
    on_boats_remaining_at_most,
    on_call,
    on_inventory_added,
    on_message_contains,
    on_question_appeared,
    on_room_changed,
)

ROOM_CANNON = 3
ROOM_MINIGAME = 4  # "use cannon" with the rope intact drops into the minigame here


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
            "man_the_cannon",
            "Fire the cannon to enter the boat-sinking minigame",
            on_room_changed(ROOM_MINIGAME),
        ),
        _goal(
            "sink_boat_1",
            "Sink the first skeleton war-canoe",
            on_boats_remaining_at_most(3),
        ),
        _goal(
            "sink_boat_2",
            "Sink the second skeleton war-canoe",
            on_boats_remaining_at_most(2),
        ),
        _goal(
            "sink_boat_3",
            "Sink the third skeleton war-canoe",
            on_boats_remaining_at_most(1),
        ),
        _goal(
            "sink_boat_4",
            "Sink the fourth skeleton war-canoe (minigame won)",
            on_boats_remaining_at_most(0),
        ),
        _goal(
            "provoke_failure",
            "Tell the pirate he is a failure (he drops his hook)",
            on_message_contains("failure as a pirate"),
        ),
        _goal(
            "get_plastic_hook",
            "Pick up the dropped plastic hook",
            on_inventory_added("plastic_hook"),
        ),
        _goal(
            "get_ramrod",
            "Pick up the ramrod off the wall",
            on_inventory_added("ramrod"),
        ),
        _goal(
            "make_gaff",
            "Combine the ramrod and plastic hook into a gaff",
            on_inventory_added("gaff"),
        ),
        _goal(
            "fish_out_cutlass",
            "Fish the debris out of the water to land a cutlass",
            on_inventory_added("cutlass"),
        ),
        _goal(
            "cut_restraint_rope",
            "Cut the cannon restraint rope with the cutlass",
            on_call(
                "act", verb="use", target1="cutlass", target2="cannon_restraint_rope"
            ),
            kind="call",
        ),
        _goal(
            "escape_via_cannon",
            "Fire the unrestrained cannon to backfire out and end the demo",
            all_of(
                on_call("act", verb="use", target1="cannon"),
                in_room(ROOM_CANNON),
                in_inventory("cutlass"),
            ),
            kind="call",
            stopping=True,
        ),
    )
}


COMI_DEMO_GOALSET = GoalSet(
    game_id="comi-demo",
    save_slot=1,
    goals=GOALS,
)
