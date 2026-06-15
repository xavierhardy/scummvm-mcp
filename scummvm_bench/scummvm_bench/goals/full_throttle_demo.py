"""Goal set for the Full Throttle DOS demo (the beginning).

Full Throttle uses a verb-coin interface (fist / kick / mouth / "walk to"); there
is no save, so the run skips the opening intro until Ben wakes inside the
dumpster. The demo's beginning, scored as its minimum actions:

  alley/dumpster (room 10) -> climb out and leave the alley to the bar front
  (room 6) -> kick the locked Kick-Stand door open (object "door" state 0 -> 1)
  -> walk through into the bar (room 7) -> punch the bartender, who hands over
  the keys to Ben's bike ("Here are your keys, all right?!", stopping goal).

Reconciled against the repo's live MCP capture (``test/mcp/test_ft.py`` and the
captured logs): the room ids (alley 10, bar front 6, bar 7), the kick that bursts
the door (state 0 -> 1) and the bartender's "your keys" line are all observed
there. Knocking on the door first (fist -> "Open up!") is optional flavour and is
not scored; only kicking it open is required.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    in_room,
    on_call,
    on_message_contains,
    on_object_changed,
    on_room_changed,
)

ROOM_ALLEY = 10  # Ben wakes inside the closed dumpster here
ROOM_BAR_FRONT = 6  # the Kick-Stand front, with the bike and the locked door
ROOM_BAR = 7  # inside the bar


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
            "state_alley",
            "Check state in the dumpster alley",
            all_of(in_room(ROOM_ALLEY), on_call("state")),
            kind="call",
        ),
        _goal(
            "leave_alley",
            "Climb out of the dumpster and leave the alley",
            on_room_changed(ROOM_BAR_FRONT),
        ),
        _goal(
            "state_bar_front",
            "Check state at the bar front",
            all_of(in_room(ROOM_BAR_FRONT), on_call("state")),
            kind="call",
        ),
        _goal(
            "kick_door_open",
            "Kick the locked Kick-Stand door open",
            all_of(in_room(ROOM_BAR_FRONT), on_object_changed("door")),
        ),
        _goal(
            "enter_bar",
            "Walk through the doorway into the bar",
            on_room_changed(ROOM_BAR),
        ),
        _goal(
            "state_bar",
            "Check state inside the bar",
            all_of(in_room(ROOM_BAR), on_call("state")),
            kind="call",
        ),
        _goal(
            "get_keys",
            "Punch the bartender into handing over the bike keys",
            on_message_contains("your keys"),
            stopping=True,
        ),
    )
}


FT_DEMO_GOALSET = GoalSet(
    game_id="ft-demo",
    save_slot=None,
    goals=GOALS,
)
