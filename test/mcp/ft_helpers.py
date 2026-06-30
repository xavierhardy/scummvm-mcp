"""Retry/poll, navigation and assertion helpers for the Full Throttle demo
integration test (``test_ft.py``).

See the module docstring of ``test_ft.py`` for the demo's storyline and the
verb-coin interface these helpers drive.
"""

from time import sleep

import httpx

from utils import (
    McpClient,
    get_state_with_retry,
    pathways,
)

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
