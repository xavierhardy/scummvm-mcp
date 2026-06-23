"""
Integration test for Indiana Jones: Fate of Atlantis demo.

The demo cannot save/load arbitrary states, so the whole walkthrough is driven
as a single sequential test (the steps depend on each other and must run in
order on one instance). This keeps the file parallel-safe: it is one test on one
fixture/port, so it never has to interleave with itself.

Walkthrough: skip intro -> answer opening dialog -> talk to Sophia -> walk to
the canyon (room 63) -> find the jeep behind the mountain and take the tire
repair kit -> read Plato's Lost Dialogue (the book is a tabbed close-up; turn to
the page giving Thera's bearing relative to Atlantis) -> work out the course
(divide the distance by ten, reverse the direction) -> tell the captain, who
ferries you to the dive site -> patch the punctured diving suit with the tire
kit, attach the air hose and put it on -> as Sophia, switch the compressor on
and work the hoist to lower Indy. With the right course he sinks toward the Lost
Kingdom (room 82); the demo's randomised destination means the heading must be
read from the book each run, exercising the MCP book-reading path end to end.
"""

import re
from time import sleep, time

import pytest
from assertions import assert_inventory_contains, assert_messages_produced
from utils import McpClient

# Fate of Atlantis cannot save/load: the demo is intro-driven and played as one
# ordered walkthrough. Pin it to a single xdist worker (one merged test today,
# but the mark keeps it grouped if it is ever split into steps).
pytestmark = pytest.mark.xdist_group("atlantis")

INTRO_POLL_SECS = 0.5
ROOM_CANYON = 63
ROOM_BOAT = 42
ROOM_GATEWAY = 82

# The book gives Thera's location *relative to Atlantis* ("the Lesser N miles
# {north/northeast/northwest} of the City"). The course from Thera is the
# opposite heading, and Plato's tenfold error means the distance is divided by
# ten before it matches the captain's menu (e.g. 160 -> "16 miles from here.").
DIR_FROM_CITY = {
    "northeast": "Southwest of Thera.",
    "northwest": "Southeast of Thera.",
    "north": "South of Thera.",
}


def _question(client: McpClient):
    return client.state().get("question")


def _wait_question(client: McpClient, timeout: float = 30.0):
    """Poll until a dialog question is pending (or timeout)."""
    deadline = time() + timeout
    while time() < deadline:
        q = _question(client)
        if q:
            return q
        sleep(INTRO_POLL_SECS)
    return None


def _answer_label(client: McpClient, question: dict, needle: str) -> dict:
    """Answer the choice whose label contains *needle* (case-insensitive)."""
    for choice in question["choices"]:
        if needle.lower() in choice["label"].lower():
            return client.answer(choice["id"])
    labels = [c["label"] for c in question["choices"]]
    raise AssertionError(f"choice containing {needle!r} not offered; got {labels}")


def _act_when_ready(client: McpClient, *args, retries: int = 25) -> dict:
    """act(), retrying while a cutscene is still holding input.

    The captain's arrival at the dive site plays an uninterruptible cutscene, so
    the first few boat actions can come back "not accepting input" / "already in
    progress" until it settles.
    """
    for _ in range(retries):
        try:
            return client.act(*args)
        except RuntimeError as exc:
            if "not accepting input" in str(exc) or "already in progress" in str(exc):
                sleep(1.0)
                continue
            raise
    raise AssertionError(f"act{args} never accepted (input stayed blocked)")


def test_atlantis_walkthrough(atlantis_client: McpClient) -> None:
    """Drive the whole Fate of Atlantis demo walkthrough in one sequential run."""
    client = atlantis_client

    # --- Skip the intro until the opening dock dialog is pending -----------
    deadline = time() + 60.0
    question = None
    while time() < deadline:
        question = _question(client)
        if question:
            break
        try:
            client.skip()
        except RuntimeError:
            pass
        sleep(INTRO_POLL_SECS)
    assert question is not None, "[intro] opening dialog never appeared after skipping the intro"

    state = client.state()
    assert state.get("room") is not None, f"[intro] state has no 'room': {state}"

    # --- Answer the opening dialog (goal: answer_opening) ------------------
    result = _answer_label(client, question, "look around")
    assert result["messages"][0] == {
        "text": "Let's take a look around.",
        "actor": "indy",
    }, f"[opening] answer should make Indy say 'Let's take a look around.', got {result.get('messages')}"

    # --- Talk to Sophia (goal: talk_to_sophia) -----------------------------
    result = client.act("talk_to", "sophia")
    assert result.get("question") is not None, f"[talk_to sophia] expected a dialog question, got {result}"
    assert any(
        c == {"id": 1, "label": "What if Atlantis was vaporized when Thera exploded?"}
        for c in result["question"]["choices"]
    ), f"[talk_to sophia] expected the 'Atlantis vaporized' topic, got {result['question']['choices']}"
    _answer_label(client, result["question"], "Excuse me")

    # --- Walk up the path; head off to look for Kerner (goal: reach_canyon) -
    result = client.act("walk_to", "path_away_from_dock")
    assert result.get("question") is not None, f"[walk path] expected Sophia's 'where are you going?' dialog, got {result}"
    result = _answer_label(client, result["question"], "Kerner")
    assert_messages_produced(result)
    assert (
        result.get("room_changed") == ROOM_CANYON
    ), f"[answer Kerner] expected the walk to advance into the canyon (room {ROOM_CANYON}), got {result.get('room_changed')}"

    # --- Find the jeep behind the mountain (goal: get_tire_repair_kit) ------
    for opening in ("notch in mountain", "cleft in mountain", "gap in mountain"):
        result = client.act("walk_to", opening)
        if result.get("room_changed"):
            break
    assert result.get("room_changed"), f"[walk mountain] none of notch/cleft/gap revealed the jeep, got {result}"
    result = client.act("pick_up", "tire repair kit")
    assert_inventory_contains(result, "tire_repair_kit")

    # --- Back to the dock --------------------------------------------------
    client.act("walk_to", "path_to_landscape")
    client.act("walk_to", "path_back_to_the_dock")

    # --- Read the Lost Dialogue's heading (goal: read_dialogue) -------------
    # The book is a tabbed close-up; page 3 carries the randomised bearing.
    client.act("look_at", "lost_dialogue_of_plato")
    sleep(0.5)
    result = client.act("look_at", "page_3")
    page_text = " ".join(m.get("text", "") for m in result.get("messages", []))
    if "of the City" not in page_text:  # fall back to the state buffer
        page_text = " ".join(m.get("text", "") for m in client.state().get("messages", []))
    assert "of the City" in page_text, f"[book] page 3 should give Atlantis's bearing, got {page_text!r}"
    miles = re.search(r"Lesser\s+(\d+)\s+miles", page_text)
    direction = re.search(r"(northeast|northwest|north)\s+of the City", page_text)
    assert miles and direction, f"[book] could not parse heading from {page_text!r}"
    distance_label = f"{int(miles.group(1)) // 10} miles from here."
    direction_label = DIR_FROM_CITY[direction.group(1)]
    client.skip()  # close the book

    # --- Tell the captain the course (goal: board_salvage_boat) ------------
    client.act("walk_to", "salvage_boat")
    client.act("talk_to", "captain")
    _answer_label(client, _wait_question(client), "Atlantis")
    _answer_label(client, _wait_question(client), "take us")
    _answer_label(client, _wait_question(client), distance_label)
    _answer_label(client, _wait_question(client), direction_label)
    _answer_label(client, _wait_question(client), "I knew that")
    _answer_label(client, _wait_question(client), "borrow your diving")
    _answer_label(client, _wait_question(client), "Yes, of course")

    # The captain ferries you out and drops you at the dive site (room 42).
    deadline = time() + 30.0
    boarded = False
    while time() < deadline:
        texts = " ".join(m.get("text", "") for m in client.state().get("messages", []))
        if "the rest is up to you" in texts:
            boarded = True
            break
        sleep(INTRO_POLL_SECS)
    assert boarded, "[captain] expected to be ferried to the dive site ('...the rest is up to you.')"
    assert (
        client.state()["room"]["id"] == ROOM_BOAT
    ), f"[captain] expected to arrive on the salvage boat (room {ROOM_BOAT})"

    # --- Patch and don the diving suit (goal: patch_diving_suit) -----------
    _act_when_ready(client, "open", "storage_locker")
    result = _act_when_ready(client, "use", "tire_repair_kit", "punctured_diving_suit")
    removed = result.get("inventory_removed", [])
    assert any(
        "tire_repair_kit" in i for i in removed
    ), f"[patch suit] expected the tire repair kit to be used up patching the suit, got {removed}"
    _act_when_ready(client, "use", "air_hose", "repaired_suit")
    result = _act_when_ready(client, "use", "repaired_diving_suit_with_hose")
    assert_messages_produced(result)

    # --- Hoist Indy into the sea toward Atlantis (goal: dive_to_atlantis) ---
    _act_when_ready(client, "pull", "air_compressor_switch")
    result = _act_when_ready(client, "use", "hoist", "indy_in_diving_suit")
    assert (
        result.get("room_changed") == ROOM_GATEWAY
    ), f"[hoist] correct heading should sink Indy toward the Lost Kingdom (room {ROOM_GATEWAY}), got {result.get('room_changed')}"
