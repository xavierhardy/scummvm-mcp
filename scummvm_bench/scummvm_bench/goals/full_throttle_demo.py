"""Goal set for the Full Throttle DOS demo (start to finish).

Full Throttle uses a verb-coin interface (fist / kick / mouth / "walk to"); there
is no save, so the run skips the opening intro until Ben wakes inside the
dumpster. The whole demo, scored as its minimum actions:

  alley/dumpster (room 10) -> climb out and leave the alley to the bar front
  (room 6) -> kick the locked Kick-Stand door open (object "door" state 0 -> 1)
  -> walk through into the bar (room 7) -> punch the bartender, who hands over
  the keys to Ben's bike ("Here are your keys, all right?!") -> walk back out to
  the bike at the bar front -> ride_bike: mount the bike and play the highway
  motorcycle minigame (the Rottwheeler fight, a real-time INSANE action sequence
  the tool steers + punches automatically) -> winning rolls the post-fight
  narration ("...Sometimes you gotta face the Cavefish.") and drops Ben into the
  Cavefish cave (room 48) -> in the cave you travel ON the bike (walking gives
  "I don't spelunk"), so ride the right-hand exit to the ramp scene (room 49) ->
  use the ramp (obj 235) to attach it and jump the canyon, which rolls the demo's
  closing LucasArts card (room 169) and loops back to the intro -- the stopping
  goal.

Reconciled against the repo's live MCP capture (``test/mcp/test_ft.py`` and the
captured logs): the room ids (alley 10, bar front 6, bar 7, Cavefish cave 48,
ramp scene 49, demo end card 169), the kick that bursts the door (state 0 -> 1),
the bartender's "your keys" line, the ``ride_bike`` call and its room_changed=48
+ post-fight narration, riding the cave exit to room 49, and the gorge jump to
room 169 are all observed there. Knocking on the door first (fist -> "Open up!")
is optional flavour and is not scored; only kicking it open is required. The bike
sits at the bar front (room 6), so returning to it after the keys is not
separately scored -- room 6 is already credited when leaving the alley.
"""

from .engine import (
    Goal,
    GoalSet,
    Predicate,
    all_of,
    any_of,
    in_room,
    on_call,
    on_message_contains,
    on_object_changed,
    on_room_changed,
)

ROOM_ALLEY = 10  # Ben wakes inside the closed dumpster here
ROOM_BAR_FRONT = 6  # the Kick-Stand front, with the bike and the locked door
ROOM_BAR = 7  # inside the bar
ROOM_CAVE = 48  # the Cavefish cave Ben lands in after winning the highway fight
ROOM_CAVE_RAMP = 49  # the cave scene with the ramp (obj 235) and Ben's bike
ROOM_DEMO_END = 169  # the closing LucasArts card after the gorge jump


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
        ),
        _goal(
            "ride_bike",
            "Mount the bike and play the highway motorcycle minigame",
            on_call("ride_bike"),
            kind="call",
        ),
        _goal(
            "win_highway_fight",
            "Win the Rottwheeler fight and reach the Cavefish cave",
            any_of(
                on_room_changed(ROOM_CAVE),
                on_message_contains("Cavefish"),
            ),
        ),
        _goal(
            "ride_cave_to_ramp",
            "Ride the bike deeper into the cave to the ramp scene",
            on_room_changed(ROOM_CAVE_RAMP),
        ),
        _goal(
            "jump_gorge",
            "Use the ramp to jump the canyon and finish the demo",
            on_room_changed(ROOM_DEMO_END),
            stopping=True,
        ),
    )
}


FT_DEMO_GOALSET = GoalSet(
    game_id="ft-demo",
    save_slot=None,
    goals=GOALS,
)
