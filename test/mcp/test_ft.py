"""
Integration tests for Full Throttle (DOS demo, SCUMM V7).

Full Throttle uses a verb-coin interface: fist (punch/operate, object verb
script 9), kick (script 5) and mouth (talk/look, script 8), plus 'walk to'
(scene-click move/exit), generic 'interact' and 'use item'.

The tests follow the demo's storyline in order, on one session:

  1. Skip the intro until the alley (room 10), Ben inside the dumpster.
  2. Open the dumpster, climb out and leave the alley -> bar front (room 6).
  3. Punch the bar door ("Open up!"), then kick it open (state 0 -> 1).
  4. Walk through the doorway into the Kickstand bar (room 7).
  5. Look at the antlers, then punch the bartender — he hands over the keys
     to Ben's bike in a long cutscene.
"""

import os
from time import sleep

import httpx
import pytest

from assertions import assert_message_contains, assert_text_contains
from ft_helpers import (
    ALLEY_ROOM,
    BAR_AREA,
    BAR_ROOM,
    CAVE_ROOM,
    DEMO_END_ROOM,
    RIDE_END_ROOMS,
    _accumulate_messages_until,
    _act_retry,
    _assert_ascii_content,
    _assert_bar_front_sanity,
    _find,
    _launch_gorge_jump,
    _leave_alley,
    _leave_bar,
    _reach_bike,
    _ride_to_ramp,
    _room,
    _skip_until_room,
    _wait_room,
    _walk_into_bar,
    _watch_for_ending,
)
from utils import (
    McpClient,
    get_state_with_retry,
    joined_message_text,
    message_texts,
)

# Full Throttle cannot save/load: the demo is one strictly-ordered storyline run
# on a single instance. Pin every test in this file to one xdist worker so they
# run in sequence (the session-scoped ft_client is shared across them).
#
# Set SKIP_SLOW_TESTS=1 to skip this whole file (and test_atlantis):
# both are slow, no-save demo walkthroughs that some runs want to leave out.
pytestmark = [
    pytest.mark.xdist_group("ft"),
    pytest.mark.skipif(
        bool(os.environ.get("SKIP_SLOW_TESTS")),
        reason="SKIP_SLOW_TESTS is set",
    ),
]


# ---------------------------------------------------------------------------
# Tests (ordered walkthrough)
# ---------------------------------------------------------------------------
def test_ft_walkthrough(ft_client: McpClient) -> None:
    # Skip the opening intro/title until the alley (room 10).
    reached_alley = _skip_until_room(ft_client, ALLEY_ROOM)
    room = _room(ft_client)
    assert reached_alley, f"did not reach the alley (room {ALLEY_ROOM}), got {room}"

    sleep(2)  # let the wake-up script start before the first click
    left_alley = _leave_alley(ft_client)
    room = _room(ft_client)
    assert left_alley, f"never left the alley, still in room {room}"

    # Bar front sanity: coin verbs exposed, exits flagged, sign is mouth-only.
    _assert_bar_front_sanity(get_state_with_retry(ft_client))

    # Punching the locked bar door: Ben pounds it shouting 'Open up!'.
    punch_texts = message_texts(_act_retry(ft_client, "fist", "door"))
    assert "Open up!" in punch_texts, f"Ben didn't pound the door: {punch_texts}"

    # Kicking the door bursts it open (object state 0 -> 1).
    kick = _act_retry(ft_client, "kick", "door")
    door_opened = {"name": "door", "old_state": 0, "new_state": 1}
    assert door_opened in kick.get("objects_changed", []), (
        f"door did not open on kick: {kick}"
    )

    # Once kicked open, punching cannot affect the door any further.
    punch2 = _act_retry(ft_client, "fist", "door")
    assert not punch2.get("objects_changed"), (
        f"punching the open door must not change it: {punch2}"
    )
    door = _find(get_state_with_retry(ft_client), "door")
    assert door is not None and door["state"] == 1, f"door must remain open, got {door}"

    # Walk through the kicked-open doorway into the bar (room 6 -> 7).
    entered_bar = _walk_into_bar(ft_client)
    room = _room(ft_client)
    assert entered_bar, f"did not enter the bar, still in room {room}"

    # 'mouth' on the antlers makes Ben comment, with clean text output.
    antlers = _act_retry(ft_client, "mouth", "antlers")
    assert_message_contains(antlers, "mounted on my handlebars")
    _assert_ascii_content(antlers.get("messages", []))

    # Punching the bartender makes him hand over the keys to Ben's bike. The
    # interrogation streams for up to ~60s after act() returns; accumulate the
    # message history until the keys line has been spoken.
    bartender = _act_retry(ft_client, "fist", "bartender")
    blob = _accumulate_messages_until(
        ft_client,
        joined_message_text(bartender),
        ("Here are your keys", "I got your keys"),
    )
    assert "your keys" in blob, f"bartender never handed over the keys: {blob[-500:]}"

    # The close-up ends back in the bar.
    assert _wait_room(ft_client, BAR_ROOM), "cutscene did not return to the bar"


def test_ft_ride_bike(ft_client: McpClient) -> None:
    """Continue the walkthrough: leave the bar, mount the bike, and play the
    highway motorcycle minigame via the ride_bike tool.

    After the keys, the bar runs ambient cutscenes (the badger's warning, the TV
    news) that briefly lock input; once control returns Ben walks out of the
    bottom exit to the street (room 5), then on to his bike at the bar front
    (room 6). 'ride_bike' mounts the bike, rides onto the highway, and auto-plays
    the Rottwheeler fight (a real-time INSANE action sequence steered by the
    mouse with left-click punches) until the section resolves at Mo's shack."""
    # The 'ride_bike' tool blocks for the whole ride + fight + wipe-out, which is
    # far longer than the default per-request timeout, and the bike fight runs
    # inside INSANE's own loop (no SSE keepalives meanwhile), so give it a roomy
    # read timeout for this one call.
    saved_http = ft_client._client

    left_to = _leave_bar(ft_client)
    assert left_to not in BAR_AREA, f"never left the bar (room {left_to})"

    # From the street, head to the bike at the bar front (room 6).
    reached_bike = _reach_bike(ft_client)
    room = _room(ft_client)
    assert reached_bike, f"did not reach the bike at the bar front, in room {room}"

    # Play the minigame. Use a long read timeout for this single streaming call.
    ft_client._client = httpx.Client(timeout=httpx.Timeout(300.0))
    try:
        result = ft_client.ride_bike()
    finally:
        ft_client._client = saved_http

    # The section resolves off the highway: the Cavefish cave (room 48) after Ben
    # wins, or Mo's shack (room 17) after a wipe-out. The auto-pilot is built to
    # win, so we expect the cave, with the post-fight narration.
    end_room = result.get("room_changed")
    assert end_room in RIDE_END_ROOMS, f"ride_bike didn't resolve: {result}"
    assert end_room == CAVE_ROOM, f"auto-pilot didn't win (room {end_room}): {result}"
    # The winning path plays the post-fight narration that sets up the cave.
    blob = joined_message_text(result)
    narrated = "Cavefish" in blob or "weapons you pick up" in blob
    assert narrated, f"no post-fight narration: {blob!r}"


def test_ft_ride_bike_requires_context(ft_client: McpClient) -> None:
    """ride_bike refuses outside the bike-with-keys context.

    By this point the highway section is over: Ben is no longer at his bike at the
    bar front and the keys have been consumed, so the tool must reject the call
    (wrong room / no keys / not accepting input) rather than misfire."""
    with pytest.raises(RuntimeError) as exc:
        ft_client.ride_bike()
    assert_text_contains(str(exc.value), "ride_bike")


def test_ft_jump_gorge(ft_client: McpClient) -> None:
    """Finish the demo: ride through the cave and jump the canyon.

    After winning the bike fight Ben is in the Cavefish cave (room 48), still on
    his bike. Inside the cave you must travel by bike — walking dismounts and Ben
    just says "I don't spelunk" at the scene exits — so ride the right-hand exit
    (obj 227) to the ramp scene (room 49). There, using the ramp (obj 235)
    attaches it and launches the gorge jump, which rolls the demo's closing
    LucasArts card (room 169) and then loops back to the opening narration
    ("Sometimes, you just wake up in trouble.")."""
    # Should still be in the cave after the fight.
    in_cave = _wait_room(ft_client, CAVE_ROOM, tries=15)
    room = _room(ft_client)
    assert in_cave, f"not in the cave, room {room}"

    # Ride deeper into the cave to the ramp scene — on the bike (obj 227 exit).
    at_ramp = _ride_to_ramp(ft_client)
    room = _room(ft_client)
    assert at_ramp, f"did not ride to the ramp scene, in room {room}"

    # Use the ramp (obj 235) — look, attach, then launch the jump.
    _launch_gorge_jump(ft_client)

    # The gorge jump rolls the demo's end card (room 169) and then loops back to
    # the opening narration. Confirm we reached the ending.
    rooms_seen, looped = _watch_for_ending(ft_client)
    seen = sorted(rooms_seen)
    reached = DEMO_END_ROOM in rooms_seen or looped
    assert reached, f"gorge jump did not end the demo (rooms: {seen}, looped={looped})"
