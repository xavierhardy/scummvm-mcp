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

import pytest
from time import sleep

from utils import McpClient, get_state_with_retry


FT_VERBS = {"fist", "kick", "mouth", "walk to", "interact", "use item"}

ALLEY_ROOM = 10      # Ben wakes inside the dumpster here
BAR_FRONT_ROOM = 6   # the Kickstand front, with the bike and the door
BAR_ROOM = 7         # inside the bar


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


def test_01_ft_intro_to_dumpster(ft_client: McpClient) -> None:
    """Skip the opening intro/title until the alley (room 10).

    Ben lies inside the closed dumpster, so the room is loaded but his actor
    is not placed yet — only the room id is observable at this point.
    """
    for _ in range(20):
        if _room(ft_client) == ALLEY_ROOM:
            break
        sleep(0.6)
        try:
            ft_client.skip()
        except Exception:
            pass
    assert _room(ft_client) == ALLEY_ROOM, (
        f"did not reach the dumpster alley (room {ALLEY_ROOM}), got {_room(ft_client)}"
    )


def test_02_ft_open_dumpster_and_leave_alley(ft_client: McpClient) -> None:
    """Open the dumpster, climb out, and leave the alley (room 10 -> 6).

    Ben is not placed in the room until he is out of the dumpster, so the
    lid-open / climb-out / alley-exit steps have no intermediate observable
    state — they are asserted together via the room transition. Clicks on
    the lid area (top of the screen) advance the wake-up sequence and the
    alley mouth at the top right is the exit. The engine ignores repeated
    clicks at an unchanged cursor position, so the click points alternate.
    """
    sleep(2)  # let the wake-up script start before the first click
    for _ in range(10):
        for pt in ((160, 50), (240, 50), (300, 75)):
            _walk_click(ft_client, *pt)
            sleep(0.5)
        if _room(ft_client) == BAR_FRONT_ROOM:
            break
    assert _room(ft_client) == BAR_FRONT_ROOM, (
        f"never left the alley, still in room {_room(ft_client)}"
    )


def test_03_ft_bar_front_verbs_and_pathways(ft_client: McpClient) -> None:
    """Bar front sanity: coin verbs exposed, exits flagged, sign is mouth-only."""
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
        assert not cv & {"fist", "kick", "mouth"}, (
            f"pathway {p['name']!r} should not advertise coin verbs, got {cv}"
        )
    for o in state.get("objects", []):
        if set(o.get("compatible_verbs", [])) & {"fist", "kick", "mouth"}:
            assert not o.get("pathway"), f"{o['name']!r} scripts a coin verb, not a pathway"

    # Per-object verbs mirror real entrypoints: the sign can only be examined.
    sign = _find(state, "sign")
    assert sign is not None, "no sign at the bar front"
    cv = set(sign.get("compatible_verbs", []))
    assert "mouth" in cv and not cv & {"fist", "kick"}, (
        f"sign should be mouth-only among coin verbs, got {cv}"
    )


def test_04_ft_punch_door(ft_client: McpClient) -> None:
    """Punching the locked bar door: Ben pounds it shouting 'Open up!'."""
    result = _act_retry(ft_client, "fist", "door")
    texts = [m.get("text", "") for m in result.get("messages", [])]
    assert "Open up!" in texts, f"expected Ben to pound the door, got {texts}"


def test_05_ft_kick_door_open(ft_client: McpClient) -> None:
    """Kicking the door bursts it open (object state 0 -> 1)."""
    result = _act_retry(ft_client, "kick", "door")
    assert {"name": "door", "old_state": 0, "new_state": 1} in result.get(
        "objects_changed", []
    ), f"door did not open on kick: {result}"


def test_06_ft_punch_after_kick(ft_client: McpClient) -> None:
    """Once kicked open, punching cannot affect the door any further.

    Note: the demo's fist script itself still runs and replays Ben's
    "Open up!" line even on the open door (verified in-game — the script
    does not branch on the door state). What must hold is that the punch
    has no effect: the door stays open (state 1) and no state change is
    reported.
    """
    result = _act_retry(ft_client, "fist", "door")
    assert not result.get("objects_changed"), (
        f"punching the kicked-open door must not change it: {result}"
    )
    state = get_state_with_retry(ft_client)
    door = _find(state, "door")
    assert door is not None and door["state"] == 1, (
        f"door must remain open after the punch, got {door}"
    )


def test_07_ft_enter_bar(ft_client: McpClient) -> None:
    """Walk through the kicked-open doorway into the bar (room 6 -> 7).

    The click must land in the door-only part of its bbox (192-216 x 64-95):
    below y=96 the bike's bbox overlaps and steals the hit, and the door's
    nominal object position (189,101) — which walk_to targets — misses the
    bbox entirely. Clicking (204, 80) hits only the doorway.
    """
    for _ in range(10):
        _walk_click(ft_client, 204, 80)
        sleep(1.5)
        if _room(ft_client) == BAR_ROOM:
            break
    assert _room(ft_client) == BAR_ROOM, (
        f"did not enter the bar, still in room {_room(ft_client)}"
    )


def test_08_ft_look_at_bar_scenery(ft_client: McpClient) -> None:
    """'mouth' on the antlers makes Ben comment, with clean text output."""
    result = _act_retry(ft_client, "mouth", "antlers")
    texts = [m.get("text", "") for m in result.get("messages", [])]
    assert any("mounted on my handlebars" in t for t in texts), (
        f"expected Ben's antlers line, got {texts}"
    )
    for t in texts:
        assert any(c.isalnum() and ord(c) < 128 for c in t), (
            f"garbage message with no ASCII alnum content: {t!r}"
        )


def test_09_ft_punch_bartender_gets_keys(ft_client: McpClient) -> None:
    """Punching the bartender makes him hand over the keys to Ben's bike.

    The punch cuts to a close-up (room 77) and a long scripted interrogation
    plays out; the bartender surrenders the keys ('Here are your keys, all
    right?!') and the scene returns to the bar. The keys are handed over in
    the cutscene (the demo never adds an inventory object for them).
    """
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
