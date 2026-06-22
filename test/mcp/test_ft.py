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

from time import sleep

import httpx
import pytest
from utils import McpClient, get_state_with_retry

# Full Throttle cannot save/load: the demo is one strictly-ordered storyline run
# on a single instance. Pin every test in this file to one xdist worker so they
# run in sequence (the session-scoped ft_client is shared across them).
pytestmark = pytest.mark.xdist_group("ft")


FT_VERBS = {"fist", "kick", "mouth", "walk to", "interact", "use item"}

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


# ---------------------------------------------------------------------------
# Tests (ordered walkthrough)
# ---------------------------------------------------------------------------
def test_ft_walkthrough(ft_client: McpClient) -> None:
    # Skip the opening intro/title until the alley (room 10).
    for _ in range(20):
        if _room(ft_client) == ALLEY_ROOM:
            break
        sleep(0.6)
        try:
            ft_client.skip()
        except Exception:
            pass
    assert (
        _room(ft_client) == ALLEY_ROOM
    ), f"did not reach the dumpster alley (room {ALLEY_ROOM}), got {_room(ft_client)}"

    sleep(2)  # let the wake-up script start before the first click
    for _ in range(10):
        for pt in ((160, 50), (240, 50), (300, 75)):
            _walk_click(ft_client, *pt)
            sleep(0.5)
        if _room(ft_client) == BAR_FRONT_ROOM:
            break
    assert (
        _room(ft_client) == BAR_FRONT_ROOM
    ), f"never left the alley, still in room {_room(ft_client)}"

    # Bar front sanity: coin verbs exposed, exits flagged, sign is mouth-only.
    state = get_state_with_retry(ft_client)

    verbs = set(state.get("verbs", []))
    missing = FT_VERBS - verbs
    assert not missing, f"Missing FT verbs: {missing}, got: {sorted(verbs)}"
    for forbidden in ("look at", "pick up", "talk to"):
        assert forbidden not in verbs, f"{forbidden!r} should not appear in FT verbs"

    # Exit hotspots carry no coin verbs and are flagged as pathways; objects
    # that script a coin verb (the kickable door, the bike) must not be.
    pathways = [o for o in state.get("objects", []) if o.get("pathway")]
    assert pathways, f"no pathways flagged in room {state.get('room')}"
    for p in pathways:
        cv = set(p.get("compatible_verbs", []))
        assert not cv & {
            "fist",
            "kick",
            "mouth",
        }, f"pathway {p['name']!r} should not advertise coin verbs, got {cv}"
    for o in state.get("objects", []):
        if set(o.get("compatible_verbs", [])) & {"fist", "kick", "mouth"}:
            assert not o.get(
                "pathway"
            ), f"{o['name']!r} scripts a coin verb, not a pathway"

    # Per-object verbs mirror real entrypoints: the sign can only be examined.
    sign = _find(state, "sign")
    assert sign is not None, "no sign at the bar front"
    cv = set(sign.get("compatible_verbs", []))
    assert "mouth" in cv and not cv & {
        "fist",
        "kick",
    }, f"sign should be mouth-only among coin verbs, got {cv}"

    # Punching the locked bar door: Ben pounds it shouting 'Open up!'.
    result = _act_retry(ft_client, "fist", "door")
    texts = [m.get("text", "") for m in result.get("messages", [])]
    assert "Open up!" in texts, f"expected Ben to pound the door, got {texts}"

    """Kicking the door bursts it open (object state 0 -> 1)."""
    result = _act_retry(ft_client, "kick", "door")
    assert {"name": "door", "old_state": 0, "new_state": 1} in result.get(
        "objects_changed", []
    ), f"door did not open on kick: {result}"

    # Once kicked open, punching cannot affect the door any further.
    result = _act_retry(ft_client, "fist", "door")
    assert not result.get(
        "objects_changed"
    ), f"punching the kicked-open door must not change it: {result}"
    state = get_state_with_retry(ft_client)
    door = _find(state, "door")
    assert (
        door is not None and door["state"] == 1
    ), f"door must remain open after the punch, got {door}"

    # Walk through the kicked-open doorway into the bar (room 6 -> 7).
    for _ in range(10):
        _walk_click(ft_client, 204, 80)
        sleep(1.5)
        if _room(ft_client) == BAR_ROOM:
            break
    assert (
        _room(ft_client) == BAR_ROOM
    ), f"did not enter the bar, still in room {_room(ft_client)}"

    # 'mouth' on the antlers makes Ben comment, with clean text output.
    result = _act_retry(ft_client, "mouth", "antlers")
    texts = [m.get("text", "") for m in result.get("messages", [])]
    assert any(
        "mounted on my handlebars" in t for t in texts
    ), f"expected Ben's antlers line, got {texts}"
    for t in texts:
        assert any(
            c.isalnum() and ord(c) < 128 for c in t
        ), f"garbage message with no ASCII alnum content: {t!r}"

    # Punching the bartender makes him hand over the keys to Ben's bike.
    result = _act_retry(ft_client, "fist", "bartender")
    texts = [m.get("text", "") for m in result.get("messages", [])]

    # The interrogation streams for up to ~60s after the act() returns;
    # accumulate message history until the keys line has been spoken.
    blob = " ".join(texts)
    for _ in range(40):
        if "Here are your keys" in blob or "I got your keys" in blob:
            break
        sleep(2)
        state = get_state_with_retry(ft_client)
        blob += " " + " ".join(m.get("text", "") for m in state.get("messages", []))
    assert "your keys" in blob, f"bartender never handed over the keys: {blob[-500:]}"

    # The close-up ends back in the bar.
    for _ in range(30):
        if _room(ft_client) == BAR_ROOM:
            break
        sleep(2)
    assert _room(ft_client) == BAR_ROOM, "cutscene did not return to the bar"


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

    def _leave_bar() -> int:
        # Walk to the bottom exit; the post-keys cutscenes lock input, so retry
        # patiently until control returns and Ben crosses to the street (room 5).
        for _ in range(120):
            r = _room(ft_client)
            if r not in BAR_AREA:
                return r
            _walk_click(ft_client, 95, 196)
            sleep(0.8)
        return _room(ft_client)

    left_to = _leave_bar()
    assert (
        left_to not in BAR_AREA
    ), f"never left the bar after the keys (still in room {left_to})"

    # From the street, head to the bike at the bar front (room 6).
    for _ in range(15):
        if _room(ft_client) == BAR_FRONT_ROOM:
            break
        if _room(ft_client) == STREET_ROOM:
            _act_retry(ft_client, "walk to", 36, attempts=3)
        sleep(1.2)
    assert (
        _room(ft_client) == BAR_FRONT_ROOM
    ), f"did not reach the bike at the bar front, in room {_room(ft_client)}"

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
    assert (
        end_room in RIDE_END_ROOMS
    ), f"ride_bike did not resolve the highway section: {result}"
    assert (
        end_room == CAVE_ROOM
    ), f"the auto-pilot did not win the fight (ended in room {end_room}): {result}"
    # The winning path plays the post-fight narration that sets up the cave.
    blob = " ".join(m.get("text", "") for m in result.get("messages", []))
    assert "Cavefish" in blob or "weapons you pick up" in blob, (
        f"expected the post-fight narration, got: {blob!r}"
    )


def test_ft_ride_bike_requires_context(ft_client: McpClient) -> None:
    """ride_bike refuses outside the bike-with-keys context.

    By this point the highway section is over: Ben is no longer at his bike at the
    bar front and the keys have been consumed, so the tool must reject the call
    (wrong room / no keys / not accepting input) rather than misfire."""
    with pytest.raises(RuntimeError) as exc:
        ft_client.ride_bike()
    assert "ride_bike" in str(exc.value).lower(), (
        f"unexpected ride_bike rejection: {exc.value}"
    )


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
    for _ in range(15):
        if _room(ft_client) == CAVE_ROOM:
            break
        sleep(2)
    assert _room(ft_client) == CAVE_ROOM, f"not in the cave, room {_room(ft_client)}"

    # Ride deeper into the cave to the ramp scene — on the bike (obj 227 exit).
    for _ in range(8):
        if _room(ft_client) == CAVE_RAMP_ROOM:
            break
        _act_retry(ft_client, "interact", CAVE_RIGHT_EXIT, attempts=3)
        sleep(2)
    assert (
        _room(ft_client) == CAVE_RAMP_ROOM
    ), f"did not ride to the ramp scene, in room {_room(ft_client)}"

    # Use the ramp (obj 235) — look, attach, then launch the jump. The jump plays
    # as a SMUSH that freezes the MCP pump, so give the fist a long read timeout
    # and tolerate it not returning cleanly.
    _act_retry(ft_client, "mouth", RAMP_OBJ, attempts=2)
    _act_retry(ft_client, "interact", RAMP_OBJ, attempts=2)
    saved_http = ft_client._client
    ft_client._client = httpx.Client(timeout=httpx.Timeout(180.0))
    try:
        try:
            ft_client.act("fist", RAMP_OBJ)
        except (httpx.ReadTimeout, httpx.ConnectError, RuntimeError):
            pass
    finally:
        ft_client._client = saved_http

    # The gorge jump rolls the demo's end card (room 169) and then loops back to
    # the opening narration. Confirm we reached the ending.
    rooms_seen: set[int] = set()
    looped = False
    for _ in range(20):
        state = get_state_with_retry(ft_client)
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
    assert DEMO_END_ROOM in rooms_seen or looped, (
        f"the gorge jump did not roll the demo ending "
        f"(rooms seen: {sorted(rooms_seen)}, looped={looped})"
    )
