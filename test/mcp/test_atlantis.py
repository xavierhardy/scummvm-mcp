"""
Integration test for Indiana Jones: Fate of Atlantis demo.

The demo cannot save/load arbitrary states, so the whole walkthrough is driven
as a single sequential test (the steps depend on each other and must run in
order on one instance). It is one test on one fixture/port, so it never has to
interleave with itself.

Walkthrough: skip intro -> answer opening dialog -> walk to the canyon (room 63)
-> find the jeep behind the mountain and take the tire repair kit -> read Plato's
Lost Dialogue (turn to the page giving Thera's bearing relative to Atlantis) ->
work out the course (distance / ten, reversed direction) -> tell the captain, who
ferries you to the dive site -> patch the punctured diving suit with the tire kit,
attach the air hose and put it on -> as Sophia, run the compressor and hoist Indy
down -> with the right course he sinks toward the Lost Kingdom (room 82) -> Kerner
cuts the air, so find the right cave (random each dive) before it runs out to
reach the Atlantis airlock (room 48) -> in the dark airlock, stand the ladder on
the rubble, open the stone box, take the rod, light it with an orichalcum bead,
use a bead on the sentry statue's mouth to open the bronze door, and step through
it: "What ancient secrets lie beyond this portal..." ends the demo.

Robustness note: the engine's MCP server is single-threaded and serves one
request at a time; a call that arrives while a stream is still settling is queued
and answered with an empty 202 body (which the client cannot parse), and httpx
keep-alive can keep that connection pooled. So every call here goes out on a
FRESH connection (forcing a clean connect/disconnect that lets the server drain
its stream state) and is gently retried — never hammered.
"""

import re
from time import sleep, time

import httpx
import pytest
from utils import McpClient

pytestmark = pytest.mark.xdist_group("atlantis")

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

PERMANENT = ("unknown verb", "unknown target", "must be", "out of bounds", "is negative")


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


def _act(client: McpClient, *args) -> dict:
    """One streaming call on a fresh connection, gently retried on transient
    backlog ("not accepting input" / 202-empty / HTTP errors)."""
    last = None
    for _ in range(10):
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
    raise AssertionError(f"act{args} never succeeded: {last}")


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


def _skip(client: McpClient) -> None:
    _fresh(client)
    try:
        client.skip()
    except Exception:
        pass


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


def test_atlantis_walkthrough(atlantis_client: McpClient) -> None:
    """Drive the whole Fate of Atlantis demo walkthrough in one sequential run."""
    client = atlantis_client

    # --- Wait for the opening dock dialog ----------------------------------
    # The demo auto-plays its intro straight into the "what's the plan?" dialog,
    # so just wait for it. (Do NOT spam skip() here: skip is a streaming call and
    # firing it repeatedly can leave the single-threaded server mid-stream, which
    # then queues the dialog answer with an empty body so it never lands.)
    question = _wait_question(client, timeout=90.0)
    assert question is not None, "[intro] opening dialog never appeared"

    # --- Answer the opening dialog (goal: answer_opening) ------------------
    result = _pick(client, "look around", {"question": question})
    assert "look around" in _msgs(result).lower() or _room(client) == 49, (
        f"[opening] expected Indy's 'take a look around' line, got {result.get('messages')}"
    )

    # --- Walk up the path; look for Kerner -> canyon (goal: reach_canyon) --
    # (No need to talk to Sophia first; walking off the dock triggers her
    # "where are you going?" prompt directly.)
    sleep(1.0)
    result = _act(client, "walk_to", "path_away_from_dock")
    result = _pick(client, "Kerner", result)
    assert _wait_room(client, ROOM_CANYON), f"[canyon] expected room {ROOM_CANYON}, got {_room(client)}"

    # --- Find the jeep behind the mountain (goal: get_tire_repair_kit) ------
    for opening in ("notch in mountain", "cleft in mountain", "gap in mountain"):
        if _room(client) != ROOM_CANYON:
            break
        _act(client, "walk_to", opening)
        sleep(1.5)
    assert _room(client) != ROOM_CANYON, "[mountain] none of notch/cleft/gap revealed the jeep"
    result = _act(client, "pick_up", "tire repair kit")
    assert any("tire_repair_kit" in i for i in result.get("inventory_added", [])) or (
        "tire_repair_kit" in (_state(client).get("inventory") or [])
    ), "[tire kit] expected the tire repair kit in inventory"

    # --- Back to the dock --------------------------------------------------
    _act(client, "walk_to", "path_to_landscape"); sleep(1.5)
    _act(client, "walk_to", "path_back_to_the_dock"); sleep(1.5)
    assert _wait_room(client, 49), "[dock] expected to return to the dock (room 49)"

    # --- Read the Lost Dialogue's heading (goal: read_dialogue) -------------
    _act(client, "look_at", "lost_dialogue_of_plato"); sleep(1.5)
    page_text = ""
    for _ in range(8):
        result = _act(client, "look_at", "page_3")  # turn to the heading page
        page_text = _msgs(result)
        if "Lesser" in page_text and "of the City" in page_text:
            break
        sleep(2.0)
    _skip(client); sleep(2.0)
    assert "of the City" in page_text, f"[book] page 3 should give Atlantis's bearing, got {page_text!r}"
    miles = re.search(r"Lesser\s+(\d+)\s+miles", page_text)
    direction = re.search(r"(northeast|northwest|north)\s+of the City", page_text)
    assert miles and direction, f"[book] could not parse heading from {page_text!r}"
    distance_label = f"{int(miles.group(1)) // 10} miles from here."
    direction_label = DIR_FROM_CITY[direction.group(1)]

    # --- Tell the captain the course (goal: board_salvage_boat) ------------
    _act(client, "walk_to", "salvage_boat"); sleep(1.0)
    result = _act(client, "talk_to", "captain"); sleep(0.5)
    result = _pick(client, "Atlantis", result); sleep(0.5)
    result = _pick(client, "take us", result); sleep(0.5)
    result = _pick(client, distance_label, result); sleep(0.5)
    result = _pick(client, direction_label, result); sleep(0.5)
    result = _pick(client, "I knew that", result); sleep(0.5)
    result = _pick(client, "borrow your diving", result); sleep(0.5)
    _pick(client, "Yes, of course", result); sleep(2.0)
    assert _wait_room(client, ROOM_BOAT), f"[captain] expected to arrive on the boat (room {ROOM_BOAT})"

    # --- Patch and don the diving suit (goal: patch_diving_suit) -----------
    sleep(1.0)
    for _ in range(8):  # arrival cutscene holds input; wait for the locker to open
        _act(client, "open", "storage_locker")
        if _wait_object(client, 491, 6) or "punctured_diving_suit" in _objects(client):
            break
        sleep(1.5)
    assert "punctured_diving_suit" in _objects(client), "[suit] storage locker never revealed the suit"
    result = _act(client, "use", "tire_repair_kit", "punctured_diving_suit")
    assert any("tire_repair_kit" in i for i in result.get("inventory_removed", [])) or (
        "tire_repair_kit" not in (_state(client).get("inventory") or [])
    ), "[patch suit] the tire repair kit should be used up patching the suit"
    _wait_object(client, 491, 20)  # repaired_suit
    _act(client, "use", "air_hose", "repaired_suit"); sleep(1.0)
    _act(client, "use", "repaired_diving_suit_with_hose"); sleep(1.0)

    # --- Hoist Indy into the sea toward Atlantis (goal: dive_to_atlantis) ---
    _act(client, "pull", "air_compressor_switch"); sleep(1.5)
    result = _act(client, "use", "hoist", "indy_in_diving_suit"); sleep(2.0)
    assert result.get("room_changed") == ROOM_GATEWAY or _wait_room(client, ROOM_GATEWAY), (
        f"[hoist] correct heading should sink Indy toward the Lost Kingdom (room {ROOM_GATEWAY})"
    )

    # --- Find the Atlantis cave before the air runs out (goal: reach_atlantis) -
    def cave_ids_nearest_first() -> list[int]:
        s = _state(client)
        ego_x = (s.get("position") or {}).get("x", 160)
        caves = [
            (o["id"], o.get("x", 0))
            for o in s.get("objects", [])
            if o["name"] == "cave" and o.get("id") in CAVE_IDS
        ]
        caves.sort(key=lambda t: abs((t[1] or 0) - ego_x))
        return [cid for cid, _ in caves]

    deadline = time() + 240
    tried: set[int] = set()
    while time() < deadline and _room(client) != ROOM_ATLANTIS:
        if _room(client) != ROOM_GATEWAY:
            sleep(2.5)  # betrayal cutscene (air-free); wait it out
            continue
        remaining = [c for c in cave_ids_nearest_first() if c not in tried]
        if not remaining:
            tried.clear()
            sleep(2.0)
            continue
        cave_id = remaining[0]
        tried.add(cave_id)
        result = _act(client, "walk_to", cave_id)
        if result.get("room_changed") == ROOM_ATLANTIS or _room(client) == ROOM_ATLANTIS:
            break
        sleep(1.0)
    assert _room(client) == ROOM_ATLANTIS, f"[caves] never reached the Atlantis airlock (room {ROOM_ATLANTIS})"

    # --- Solve the airlock (goal: open_airlock_box) ------------------------
    # Dark room: stand the ladder on the rubble to climb, then open the stone box.
    _act(client, "pick_up", LADDER); sleep(1.0)
    _act(client, "use", "ladder", RUBBLE); sleep(2.0)
    assert _wait_object(client, STONE_BOX, 30), "[airlock] climbing never exposed the stone box"
    result = _act(client, "open", STONE_BOX); sleep(1.5)
    assert "it opens" in _msgs(result).lower() or _wait_object(client, ROD, 15), (
        "[airlock] the stone box should open and reveal the rod"
    )

    # Take the rod, light it with a bead, then use a bead on the statue's mouth
    # to swing the bronze door open.
    _act(client, "pick_up", ROD); sleep(1.0)
    _act(client, "use", BEAD, ROD); sleep(2.0)  # light the airlock
    _act(client, "use", BEAD, STATUE); sleep(3.0)  # open the bronze door

    # --- Step through the open door (goal: enter_atlantis = demo end) ------
    final = ""
    deadline = time() + 60
    while time() < deadline:
        result = _act(client, "walk_to", BRONZE_DOOR)
        final = _msgs(result)
        if "ancient secrets" in final.lower():
            break
        # the door may still be swinging open / input held — try again shortly
        if "ancient secrets" in " ".join(
            m.get("text", "") for m in _state(client).get("messages", [])
        ).lower():
            final = "ancient secrets"
            break
        sleep(2.0)
    assert "ancient secrets" in final.lower(), (
        "[airlock] stepping through the bronze door should end the demo with "
        "Indy's 'What ancient secrets lie beyond this portal' line"
    )
