"""Goal set for the Curse of Monkey Island demo, save slot 1 (escape the hold).

The demo opens with Guybrush a captive in the cannon room (room 3) of LeChuck's
ship, guarded by a "small pirate" who turns out to be his old friend Wally. The
whole demo is the escape, scored here as its minimum actions:

  talk to the pirate and needle him until he lets slip he is Wally ("Wally!"),
  wheedle a pirate-philosophy leaflet (``pirate_literature``) out of him, then
  call him a failure as a pirate -- which rattles him into dropping his plastic
  hook. Pick up the dropped ``plastic_hook`` and the ``ramrod`` off the wall,
  fire the cannon to sink the four skeleton boats, then combine the ramrod with
  the plastic hook into a ``gaff``, fish the ``debris`` out of the water (landing
  a ``cutlass`` and a ``skeleton_arm``), cut the ``cannon_restraint_rope`` with
  the cutlass and fire the cannon once more -- with nothing to hold it down it
  backfires Guybrush through the door into the treasure room (stopping goal).

The cannon room and pirate (room 3, ``small_pirate``), the dialog lines, the
``ramrod`` pickup, the gaff combine (ramrod + plastic_hook) and the gaff-on-
debris fish (-> ``cutlass`` + ``skeleton_arm``) are reconciled against the
repo's live MCP captures (``test/mcp/test_comi.py`` and ``test/mcp/test_comi_s3.py``);
the cannon minigame (``shoot_cannon``) against ``test/mcp/test_comi_cannon.py``.
The pirate dialog is non-deterministic in which topic triggers each beat, so the
dialog goals are observable end-states (messages / inventory) independent of the
path. The final cut-rope / backfire-escape chain is not covered by a repo
capture; its tool sequence follows the published demo walkthrough and
``TREASURE_ROOM`` is a best-effort id pending a live capture.
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

ROOM_CANNON = 3
TREASURE_ROOM = 4  # best-effort; the cannon backfires Guybrush in here on escape


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
            "fire_at_boats",
            "Fire the cannon at the skeleton boats",
            on_call("shoot_cannon"),
            kind="call",
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
            "escape_to_treasure_room",
            "Fire the unrestrained cannon and backfire into the treasure room",
            on_room_changed(TREASURE_ROOM),
            stopping=True,
        ),
    )
}


COMI_DEMO_GOALSET = GoalSet(
    game_id="comi-demo",
    save_slot=1,
    goals=GOALS,
)
