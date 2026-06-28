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
from utils import (
    McpClient,
    get_state_with_retry,
    joined_message_text,
    message_texts,
    pathways,
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


FT_VERBS = {"fist", "kick", "mouth", "walk to", "interact", "use item"}
COIN_VERBS = {"fist", "kick", "mouth"}

ALLEY_ROOM = 10  # Ben wakes inside the dumpster here
BAR_FRONT_ROOM = 6  # the Kickstand front, with the bike and the door
BAR_ROOM = 7  # inside the bar
STREET_ROOM = 5  # the street outside the bar (between the bar and the bike)
# The highway fight resolves to one of two rooms: the Cavefish cave after Ben wins
# the Rottwheeler fight (room 48), or Mo's mechanic shack after a wipe-out (room
# 17). The auto-pilot is built to win, so it normally reaches the cave (48).
CAVE_ROOM = 48  # cave entry, where the post-fight "Cavefish" narration plays
SHACK_ROOM = 17
RIDE_END_ROOMS = {CAVE_ROOM, SHACK_ROOM}
# Inside the cave you travel ON the bike (walking gives "I don't spelunk"). From
# the cave entry, riding the right-hand exit (obj 227) reaches the ramp scene
# (room 49); using the ramp there (obj 235, fist) does the gorge jump, which rolls
# the demo's closing LucasArts card (room 169) and then loops back to the intro.
CAVE_RAMP_ROOM = 49
CAVE_RIGHT_EXIT = 227
RAMP_OBJ = 235
DEMO_END_ROOM = 169
BAR_AREA = {BAR_FRONT_ROOM, BAR_ROOM, 77}  # 77 is the keys close-up


def _room(client: McpClient) -> int:
    return get_state_with_retry(client).get("room", {}).get("id")


def _find(state: dict, name: str):
    for o in state.get("objects", []):
        if o["name"] == name:
            return o
    return None


def _act_retry(client: McpClient, verb: str, target, attempts: int = 15) -> dict:
    """act() with retries while a cutscene is holding input."""
    last = None
    for _ in range(attempts):
        try:
            return client.act(verb, target)
        except RuntimeError as e:
            last = e
            if "not accepting input" in str(e):
                sleep(1.0)
                continue
            raise
    raise AssertionError(f"act({verb!r}, {target!r}) never accepted input: {last}")


def _walk_click(client: McpClient, x: int, y: int) -> None:
    """walk() that tolerates input being briefly held by a script."""
    try:
        client.walk(x, y)
    except RuntimeError as e:
        if "not accepting input" not in str(e):
            raise


def _wait_room(
    client: McpClient, room: int, tries: int = 30, poll: float = 2.0
) -> bool:
    """Poll until *room* is reached; return True if so."""
    for _ in range(tries):
        if _room(client) == room:
            return True
        sleep(poll)
    return _room(client) == room


def _skip_until_room(
    client: McpClient, room: int, tries: int = 20, poll: float = 0.6
) -> bool:
    """Press skip until *room* is reached (advances the intro/title)."""
    for _ in range(tries):
        if _room(client) == room:
            return True
        sleep(poll)
        try:
            client.skip()
        except Exception:
            pass
    return _room(client) == room


def _leave_alley(client: McpClient, tries: int = 10) -> bool:
    """Click the wake-up exits until Ben leaves the alley for the bar front."""
    for _ in range(tries):
        for pt in ((160, 50), (240, 50), (300, 75)):
            _walk_click(client, *pt)
            sleep(0.5)
        if _room(client) == BAR_FRONT_ROOM:
            return True
    return _room(client) == BAR_FRONT_ROOM


def _walk_into_bar(client: McpClient, tries: int = 10) -> bool:
    """Walk through the kicked-open doorway into the bar (room 6 -> 7)."""
    for _ in range(tries):
        _walk_click(client, 204, 80)
        sleep(1.5)
        if _room(client) == BAR_ROOM:
            return True
    return _room(client) == BAR_ROOM


def _accumulate_messages_until(
    client: McpClient, blob: str, needles, tries: int = 40, poll: float = 2.0
) -> str:
    """Append streamed message history to *blob* until any needle appears."""
    for _ in range(tries):
        if any(n in blob for n in needles):
            return blob
        sleep(poll)
        state = get_state_with_retry(client)
        blob += " " + " ".join(m.get("text", "") for m in state.get("messages", []))
    return blob


def _assert_ascii_content(messages: list) -> None:
    """Every message must carry at least one ASCII alphanumeric character."""
    for m in messages:
        text = m.get("text", "")
        assert any(c.isalnum() and ord(c) < 128 for c in text), (
            f"garbage message: {text!r}"
        )


def _pathways_advertising_coin_verbs(state: dict) -> list:
    """Return names of pathway exits that wrongly advertise a coin verb."""
    return [
        p["name"]
        for p in pathways(state)
        if set(p.get("compatible_verbs", [])) & COIN_VERBS
    ]


def _coin_objects_flagged_as_pathway(state: dict) -> list:
    """Return names of coin-verb objects that are wrongly flagged as pathways."""
    return [
        o["name"]
        for o in state.get("objects", [])
        if set(o.get("compatible_verbs", [])) & COIN_VERBS and o.get("pathway")
    ]


def _assert_bar_front_sanity(state: dict) -> None:
    """Coin verbs exposed, exits flagged as pathways, the sign is mouth-only."""
    verbs = set(state.get("verbs", []))
    missing = FT_VERBS - verbs
    assert not missing, f"Missing FT verbs: {missing}, got: {sorted(verbs)}"
    leaked = {"look at", "pick up", "talk to"} & verbs
    assert not leaked, f"non-coin verbs leaked into FT verbs: {leaked}"

    assert pathways(state), f"no pathways flagged in room {state.get('room')}"
    bad_pathways = _pathways_advertising_coin_verbs(state)
    assert not bad_pathways, f"pathways advertise coin verbs: {bad_pathways}"
    coin_as_pathway = _coin_objects_flagged_as_pathway(state)
    assert not coin_as_pathway, f"coin objects as pathway: {coin_as_pathway}"

    # Per-object verbs mirror real entrypoints: the sign can only be examined.
    sign = _find(state, "sign")
    assert sign is not None, "no sign at the bar front"
    sign_verbs = set(sign.get("compatible_verbs", []))
    assert "mouth" in sign_verbs and not sign_verbs & {
        "fist",
        "kick",
    }, f"sign not mouth-only: {sign_verbs}"


def _leave_bar(client: McpClient, tries: int = 120) -> int:
    """Walk to the bottom exit until Ben crosses to the street; return the room.

    The post-keys cutscenes lock input, so retry patiently until control returns.
    """
    for _ in range(tries):
        r = _room(client)
        if r not in BAR_AREA:
            return r
        _walk_click(client, 95, 196)
        sleep(0.8)
    return _room(client)


def _reach_bike(client: McpClient, tries: int = 15) -> bool:
    """From the street, head to the bike at the bar front (room 6)."""
    for _ in range(tries):
        if _room(client) == BAR_FRONT_ROOM:
            return True
        if _room(client) == STREET_ROOM:
            _act_retry(client, "walk to", 36, attempts=3)
        sleep(1.2)
    return _room(client) == BAR_FRONT_ROOM


def _ride_to_ramp(client: McpClient, tries: int = 8) -> bool:
    """Ride deeper into the cave to the ramp scene — on the bike (obj 227 exit)."""
    for _ in range(tries):
        if _room(client) == CAVE_RAMP_ROOM:
            return True
        _act_retry(client, "interact", CAVE_RIGHT_EXIT, attempts=15)
        sleep(2)
    return _room(client) == CAVE_RAMP_ROOM


def _launch_gorge_jump(client: McpClient) -> None:
    """Look at, attach, then launch the ramp jump (a SMUSH that freezes the pump).

    Give the fist a long read timeout and tolerate it not returning cleanly.
    """
    _act_retry(client, "mouth", RAMP_OBJ, attempts=2)
    _act_retry(client, "interact", RAMP_OBJ, attempts=2)
    saved_http = client._client
    client._client = httpx.Client(timeout=httpx.Timeout(180.0))
    try:
        try:
            client.act("fist", RAMP_OBJ)
        except (httpx.ReadTimeout, httpx.ConnectError, RuntimeError):
            pass
    finally:
        client._client = saved_http


def _watch_for_ending(client: McpClient, tries: int = 20):
    """Watch rooms/messages for the demo end card or the loop-back narration."""
    rooms_seen: set[int] = set()
    looped = False
    for _ in range(tries):
        state = get_state_with_retry(client)
        rid = state.get("room", {}).get("id")
        if rid is not None:
            rooms_seen.add(rid)
        if any(
            "wake up in trouble" in (m.get("text") or "")
            for m in state.get("messages", [])
        ):
            looped = True
        if DEMO_END_ROOM in rooms_seen or looped:
            break
        sleep(1.5)
    return rooms_seen, looped


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
