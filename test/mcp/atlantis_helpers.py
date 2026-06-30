"""Connection-robustness, retry/poll and walkthrough-step helpers for the
Fate of Atlantis demo integration test (``test_atlantis.py``).

The demo's MCP server is single-threaded; every call goes out on a FRESH
connection and is gently retried, never hammered. See the module docstring
of ``test_atlantis.py`` for the full walkthrough and robustness rationale.
"""

from time import sleep, time

import httpx

from utils import McpClient

INTRO_POLL_SECS = 0.5
ROOM_CANYON = 63
ROOM_BOAT = 42
ROOM_GATEWAY = 82
ROOM_ATLANTIS = 48
CAVE_IDS = range(1096, 1103)  # the seven underwater cave doorways in room 82
BEAD = 933  # orichalcum bead inventory item (its name does not round-trip cleanly)
# Airlock object ids (room 48) — fixed game-data numbers.
LADDER, RUBBLE, STONE_BOX, ROD, STATUE, BRONZE_DOOR = 583, 584, 585, 586, 587, 582

# The book gives Thera's location *relative to Atlantis* ("the Lesser N miles
# {north/northeast/northwest} of the City"). The course from Thera is the
# opposite heading, and Plato's tenfold error means the distance is divided by
# ten before it matches the captain's menu (e.g. 160 -> "16 miles from here.").
DIR_FROM_CITY = {
    "northeast": "Southwest of Thera.",
    "northwest": "Southeast of Thera.",
    "north": "South of Thera.",
}

PERMANENT = (
    "unknown verb",
    "unknown target",
    "must be",
    "out of bounds",
    "is negative",
)


def _fresh(client: McpClient) -> None:
    """Give the client a brand-new connection (no keep-alive) for the next call."""
    sid = client._session_id
    try:
        client._client.close()
    except Exception:
        pass
    client._client = httpx.Client(
        timeout=httpx.Timeout(120.0),
        limits=httpx.Limits(max_keepalive_connections=0),
    )
    client._session_id = sid


def _state(client: McpClient) -> dict:
    """state() on a fresh connection, patiently riding out cutscene backlog."""
    deadline = time() + 150
    last = None
    while time() < deadline:
        _fresh(client)
        try:
            return client.state()
        except Exception as exc:
            last = exc
            sleep(2.0)
    raise AssertionError(f"state() never responded: {last}")


def _room(client: McpClient):
    return (_state(client).get("room") or {}).get("id")


def _objects(client: McpClient) -> dict:
    return {o["name"]: o["id"] for o in _state(client).get("objects", [])}


def _question(client: McpClient):
    return _state(client).get("question")


def _act(client: McpClient, *args, input_wait: float = 150.0) -> dict:
    """One streaming call on a fresh connection, patiently retried.

    "not accepting input" is a legitimate wait: the captain's arrival at the dive
    site and Kerner's betrayal play long, uninterruptible cutscenes, so retry
    until input returns (up to ``input_wait`` seconds). Empty 202 bodies / HTTP
    errors are transient backlog and get the same gentle back-off.
    """
    deadline = time() + input_wait
    last = None
    while time() < deadline:
        _fresh(client)
        try:
            return client.act(*args) or {}
        except RuntimeError as exc:
            last = exc
            if any(p in str(exc) for p in PERMANENT):
                raise
            if "stream ended without result" in str(exc):
                return {}  # action fired; outcome read from state
            sleep(2.0)
        except Exception as exc:
            last = exc
            sleep(2.0)
    raise AssertionError(f"act{args} never succeeded within {input_wait}s: {last}")


def _answer(client: McpClient, choice_id: int) -> dict:
    for _ in range(8):
        _fresh(client)
        try:
            return client.answer(choice_id) or {}
        except RuntimeError as exc:
            if "no dialog question" in str(exc):
                return {}
            sleep(2.0)
        except Exception:
            sleep(2.0)
    return {}


def _skip(client: McpClient) -> dict:
    """skip() on a fresh connection; returns the result dict ({} on transient)."""
    for _ in range(6):
        _fresh(client)
        try:
            return client.skip() or {}
        except RuntimeError as exc:
            if "tool rejected" in str(exc) or "no " in str(exc):
                return {}  # nothing to skip right now
            sleep(1.0)
        except Exception:
            sleep(1.0)
    return {}


def _wait_question(client: McpClient, timeout: float = 45.0):
    deadline = time() + timeout
    while time() < deadline:
        q = _question(client)
        if q:
            return q
        sleep(1.5)
    return None


def _pick(client: McpClient, needle: str, src: dict | None = None) -> dict:
    """Answer the choice containing *needle*, reading the question from *src* (a
    prior act/answer result) when present so dialogs chain off results instead of
    polling state during the captain's cutscene-heavy exchange."""
    q = (src or {}).get("question") or _wait_question(client)
    assert q, f"no dialog question offered for {needle!r}"
    for choice in q["choices"]:
        if needle.lower() in choice["label"].lower():
            sleep(0.5)
            return _answer(client, choice["id"])
    raise AssertionError(f"{needle!r} not in {[c['label'] for c in q['choices']]}")


def _wait_room(client: McpClient, target: int, timeout: float = 90.0) -> bool:
    deadline = time() + timeout
    while time() < deadline:
        if _room(client) == target:
            return True
        sleep(2.0)
    return _room(client) == target


def _wait_object(client: McpClient, obj_id: int, timeout: float = 40.0) -> bool:
    deadline = time() + timeout
    while time() < deadline:
        if obj_id in _objects(client).values():
            return True
        sleep(1.5)
    return obj_id in _objects(client).values()


def _msgs(result: dict) -> str:
    return " ".join(m.get("text", "") for m in result.get("messages", []))


def _says(result: dict, needle: str) -> bool:
    """True if *result*'s spoken text contains *needle* (case-insensitive)."""
    return needle.lower() in _msgs(result).lower()


def _added(result: dict, item: str) -> bool:
    """True if *item* appears in *result*'s inventory_added list."""
    return any(item in i for i in result.get("inventory_added", []))


def _removed(result: dict, item: str) -> bool:
    """True if *item* appears in *result*'s inventory_removed list."""
    return any(item in i for i in result.get("inventory_removed", []))


def _in_inventory(client: McpClient, item: str) -> bool:
    """True if *item* is in the live inventory."""
    return item in (_state(client).get("inventory") or [])


def _debug(client: McpClient) -> dict:
    """debug() on a fresh connection, patiently retried."""
    deadline = time() + 60
    last = None
    while time() < deadline:
        _fresh(client)
        try:
            return client.debug()
        except Exception as exc:  # noqa: BLE001 - transient backlog, retry
            last = exc
            sleep(2.0)
    raise AssertionError(f"debug() never responded: {last}")


def _set_talk_speed(client: McpClient, speed: int = 255) -> dict:
    """set_talk_speed() on a fresh connection, patiently retried."""
    deadline = time() + 60
    last = None
    while time() < deadline:
        _fresh(client)
        try:
            return client.set_talk_speed(speed)
        except Exception as exc:  # noqa: BLE001 - transient backlog, retry
            last = exc
            sleep(2.0)
    raise AssertionError(f"set_talk_speed() never responded: {last}")


# ---------------------------------------------------------------------------
# Walkthrough step helpers — each owns the polling/retry loop for one beat so
# the test body stays a flat sequence of steps and single-line assertions.
# ---------------------------------------------------------------------------


def _skip_intro(client: McpClient):
    """Skip the SMUSH/scripted intro until the opening dock dialog is pending."""
    result = _skip(client)
    for _ in range(60):
        if "room_changed" in result:
            break
        sleep(INTRO_POLL_SECS)
        result = _skip(client)
    old_room = result.get("room_changed")
    for _ in range(60):  # wait past the first post-skip screen
        if _room(client) != old_room:
            old_room = _room(client)
            break
        sleep(INTRO_POLL_SECS)
    for _ in range(60):  # ...and the second
        if _room(client) != old_room:
            break
        sleep(INTRO_POLL_SECS)
    _skip(client)
    return _wait_question(client, timeout=60.0)


def _find_jeep_behind_mountain(client: McpClient) -> None:
    """Walk through the mountain opening (notch/cleft/gap) to reveal the jeep."""
    for opening in ("notch in mountain", "cleft in mountain", "gap in mountain"):
        if _room(client) != ROOM_CANYON:
            break
        _act(client, "walk_to", opening)
        sleep(1.5)


def _read_heading_page(client: McpClient) -> str:
    """Turn to the Lost Dialogue's heading page (page_3) and return its text."""
    page_text = ""
    for _ in range(8):
        result = _act(client, "look_at", "page_3")
        page_text = _msgs(result)
        if "Lesser" in page_text and "of the City" in page_text:
            break
        sleep(2.0)
    return page_text


def _open_storage_locker(client: McpClient) -> None:
    """Open the storage locker until the punctured diving suit appears."""
    for _ in range(8):  # arrival cutscene holds input; wait for the locker to open
        _act(client, "open", "storage_locker")
        if _wait_object(client, 491, 6) or "punctured_diving_suit" in _objects(client):
            break
        sleep(1.5)


def _cave_ids_nearest_first(client: McpClient) -> list:
    """Return the room-82 cave doorway ids ordered by distance from Indy."""
    s = _state(client)
    ego_x = (s.get("position") or {}).get("x", 160)
    caves = [
        (o["id"], o.get("x", 0))
        for o in s.get("objects", [])
        if o["name"] == "cave" and o.get("id") in CAVE_IDS
    ]
    caves.sort(key=lambda t: abs((t[1] or 0) - ego_x))
    return [cid for cid, _ in caves]


def _search_caves_for_atlantis(client: McpClient, timeout: float = 240) -> None:
    """Try caves nearest-first (before the air runs out) until room 48 is reached."""
    deadline = time() + timeout
    tried: set[int] = set()
    while time() < deadline and _room(client) != ROOM_ATLANTIS:
        if _room(client) != ROOM_GATEWAY:
            sleep(2.5)  # betrayal cutscene (air-free); wait it out
            continue
        remaining = [c for c in _cave_ids_nearest_first(client) if c not in tried]
        if not remaining:
            tried.clear()
            sleep(2.0)
            continue
        cave_id = remaining[0]
        tried.add(cave_id)
        result = _act(client, "walk_to", cave_id)
        if (
            result.get("room_changed") == ROOM_ATLANTIS
            or _room(client) == ROOM_ATLANTIS
        ):
            break
        sleep(1.0)


def _step_through_bronze_door(client: McpClient, timeout: float = 60) -> str:
    """Walk through the open bronze door until the demo's final line appears."""
    final = ""
    deadline = time() + timeout
    while time() < deadline:
        result = _act(client, "walk_to", BRONZE_DOOR)
        final = _msgs(result)
        if "ancient secrets" in final.lower():
            break
        # the door may still be swinging open / input held — try again shortly
        state_text = " ".join(
            m.get("text", "") for m in _state(client).get("messages", [])
        )
        if "ancient secrets" in state_text.lower():
            final = "ancient secrets"
            break
        sleep(2.0)
    return final
