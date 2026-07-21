"""
Integration test for Indiana Jones: Fate of Atlantis demo.

The demo cannot save/load arbitrary states, so the whole walkthrough is driven
as a single sequential test (the steps depend on each other and must run in
order on one instance). It is one test on one fixture/port, so it never has to
interleave with itself.

Walkthrough: skip intro -> answer opening dialog -> walk to the canyon (room 63)
-> find the jeep behind the mountain and take the tire repair kit -> read Plato's
Lost Dialogue page by page (including the one giving Thera's bearing relative to
Atlantis) and close it again ->
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

import os
import re
from time import sleep

import pytest

from assertions import assert_text_contains
from atlantis_helpers import (
    BEAD,
    BOOK_PAGES,
    DIR_FROM_CITY,
    LADDER,
    ROD,
    ROOM_ATLANTIS,
    ROOM_BOAT,
    ROOM_BOOK,
    ROOM_CANYON,
    ROOM_GATEWAY,
    RUBBLE,
    STATUE,
    STONE_BOX,
    _act,
    _added,
    _close_book,
    _debug,
    _find_jeep_behind_mountain,
    _in_inventory,
    _objects,
    _open_page,
    _open_storage_locker,
    _pick,
    _read_heading_page,
    _read_page,
    _removed,
    _room,
    _says,
    _search_caves_for_atlantis,
    _set_talk_speed,
    _skip_intro,
    _step_through_bronze_door,
    _wait_object,
    _wait_room,
)
from utils import McpClient

# Set SKIP_SLOW_TESTS=1 to skip this whole file (and test_ft):
# both are slow, no-save demo walkthroughs that some runs want to leave out.
pytestmark = [
    pytest.mark.xdist_group("atlantis"),
    pytest.mark.skipif(
        bool(os.environ.get("SKIP_SLOW_TESTS")),
        reason="SKIP_SLOW_TESTS is set",
    ),
]


def test_atlantis_talk_speed_maxed(atlantis_client: McpClient) -> None:
    """The session fixture forces max text speed via the mcp_debug-gated
    set_talk_speed tool, because the demo's boot script otherwise overrides the
    configured talkspeed (it sets VAR_CHARINC outside room 0, so the engine's
    room-0-only user override is skipped). See conftest.atlantis_client.

    Confirm the engine reflects the maximum, in the tool's own report and via
    debug(). Placed before the walkthrough so it runs on the fresh instance.
    """
    client = atlantis_client

    # The tool's own report (re-asserting is idempotent).
    res = _set_talk_speed(client, 255)
    assert res["talkspeed"] == 255
    assert res["text_speed"] == 9  # 0..9 engine scale, 9 == fastest
    assert res["charinc"] == 0  # live per-char delay, 0 == instant text

    # ...and debug() independently reflects it.
    dbg = _debug(client)
    assert dbg["talkspeed"] == 255
    assert dbg["text_speed"] == 9
    assert dbg["charinc"] == 0


def test_atlantis_walkthrough(atlantis_client: McpClient) -> None:
    """Drive the whole Fate of Atlantis demo walkthrough in one sequential run."""
    client = atlantis_client

    # --- Skip the intro until the opening dock dialog is pending -----------
    question = _skip_intro(client)
    assert question is not None, "[intro] opening dialog never appeared"

    # --- Answer the opening dialog (goal: answer_opening) ------------------
    result = _pick(client, "look around", {"question": question})
    messages = result.get("messages")
    saw_line = _says(result, "look around") or _room(client) == 49
    assert saw_line, f"[opening] no look-around line, got {messages}"

    # --- Walk up the path; look for Kerner -> canyon (goal: reach_canyon) --
    # (No need to talk to Sophia first; walking off the dock triggers her
    # "where are you going?" prompt directly.)
    sleep(1.0)
    result = _act(client, "walk_to", "path_away_from_dock")
    result = _pick(client, "Kerner", result)
    reached_canyon = _wait_room(client, ROOM_CANYON)
    room = _room(client)
    assert reached_canyon, f"[canyon] expected room {ROOM_CANYON}, got {room}"

    # --- Find the jeep behind the mountain (goal: get_tire_repair_kit) ------
    _find_jeep_behind_mountain(client)
    room = _room(client)
    assert room != ROOM_CANYON, "[mountain] none of notch/cleft/gap revealed the jeep"
    result = _act(client, "pick_up", "tire repair kit")
    got_kit = _added(result, "tire_repair_kit") or _in_inventory(
        client, "tire_repair_kit"
    )
    assert got_kit, "[tire kit] expected the tire repair kit in inventory"

    # --- Back to the dock --------------------------------------------------
    _act(client, "walk_to", "path_to_landscape")
    sleep(1.5)
    _act(client, "walk_to", "path_back_to_the_dock")
    sleep(1.5)
    assert _wait_room(client, 49), "[dock] expected to return to the dock (room 49)"

    # --- Read the Lost Dialogue's heading (goal: read_dialogue) -------------
    # The whole book is readable over MCP: opening it puts the close-up's five
    # pages in state as page_1..page_5, each turned to with `act look_at`, and
    # `act close book` shuts it again.
    _act(client, "look_at", "lost_dialogue_of_plato")
    sleep(1.5)
    assert _room(client) == ROOM_BOOK, (
        "[book] look at the book should open the close-up"
    )
    # The book opens on page 1, whose own tab the game ignores (it sits under its
    # disabled variants), so turning to it is rejected while it is on show.
    assert _open_page(client) == 1, "[book] the book should open on page 1"
    with pytest.raises(RuntimeError, match="already open"):
        _act(client, "look_at", "page_1")

    # Page 2 onwards, then back round to page 1: the whole dialogue, page by page.
    for page, needle in BOOK_PAGES[1:] + BOOK_PAGES[:1]:
        text = _read_page(client, page, needle)
        assert needle.lower() in text.lower(), (
            f"[book] page {page} should read {needle!r}, got {text!r}"
        )
        assert _open_page(client) == page, f"[book] state should show page {page} open"

    page_text = _read_heading_page(client)
    _close_book(client)
    sleep(2.0)
    assert _wait_room(client, 49), "[book] closing the book should return to the dock"
    assert "of the City" in page_text, f"[book] no bearing on page 3: {page_text!r}"
    miles = re.search(r"Lesser\s+(\d+)\s+miles", page_text)
    direction = re.search(r"(northeast|northwest|north)\s+of the City", page_text)
    assert miles and direction, f"[book] could not parse heading from {page_text!r}"
    tenths = int(miles.group(1)) // 10
    distance_label = f"{tenths} miles from here."
    direction_label = DIR_FROM_CITY[direction.group(1)]

    # --- Tell the captain the course (goal: board_salvage_boat) ------------
    _act(client, "walk_to", "salvage_boat")
    sleep(1.0)
    result = _act(client, "talk_to", "captain")
    sleep(0.5)
    result = _pick(client, "Atlantis", result)
    sleep(0.5)
    result = _pick(client, "take us", result)
    sleep(0.5)
    result = _pick(client, distance_label, result)
    sleep(0.5)
    result = _pick(client, direction_label, result)
    sleep(0.5)
    result = _pick(client, "I knew that", result)
    sleep(0.5)
    result = _pick(client, "borrow your diving", result)
    sleep(0.5)
    _pick(client, "Yes, of course", result)
    sleep(2.0)
    assert _wait_room(client, ROOM_BOAT), (
        f"[captain] expected the boat (room {ROOM_BOAT})"
    )

    # --- Patch and don the diving suit (goal: patch_diving_suit) -----------
    sleep(1.0)
    _open_storage_locker(client)
    assert "punctured_diving_suit" in _objects(client), (
        "[suit] locker never revealed the suit"
    )
    result = _act(client, "use", "tire_repair_kit", "punctured_diving_suit")
    used_kit = _removed(result, "tire_repair_kit") or not _in_inventory(
        client, "tire_repair_kit"
    )
    assert used_kit, "[patch suit] tire kit should be consumed"
    _wait_object(client, 491, 20)  # repaired_suit
    _act(client, "use", "air_hose", "repaired_suit")
    sleep(1.0)
    _act(client, "use", "repaired_diving_suit_with_hose")
    sleep(1.0)

    # --- Hoist Indy into the sea toward Atlantis (goal: dive_to_atlantis) ---
    _act(client, "pull", "air_compressor_switch")
    sleep(1.5)
    result = _act(client, "use", "hoist", "indy_in_diving_suit")
    sleep(2.0)
    sank = result.get("room_changed") == ROOM_GATEWAY or _wait_room(
        client, ROOM_GATEWAY
    )
    assert sank, f"[hoist] the correct heading should sink Indy (room {ROOM_GATEWAY})"

    # --- Find the Atlantis cave before the air runs out (goal: reach_atlantis) -
    _search_caves_for_atlantis(client)
    room = _room(client)
    assert room == ROOM_ATLANTIS, f"[caves] never reached the airlock ({ROOM_ATLANTIS})"

    # --- Solve the airlock (goal: open_airlock_box) ------------------------
    # Dark room: stand the ladder on the rubble to climb, then open the stone box.
    _act(client, "pick_up", LADDER)
    sleep(1.0)
    _act(client, "use", "ladder", RUBBLE)
    sleep(2.0)
    assert _wait_object(client, STONE_BOX, 30), (
        "[airlock] climbing never exposed the stone box"
    )
    result = _act(client, "open", STONE_BOX)
    sleep(1.5)
    opened = _says(result, "it opens") or _wait_object(client, ROD, 15)
    assert opened, "[airlock] the stone box should open and reveal the rod"

    # Take the rod, light it with a bead, then use a bead on the statue's mouth
    # to swing the bronze door open.
    _act(client, "pick_up", ROD)
    sleep(1.0)
    _act(client, "use", BEAD, ROD)
    sleep(2.0)  # light the airlock
    _act(client, "use", BEAD, STATUE)
    sleep(3.0)  # open the bronze door

    # --- Step through the open door (goal: enter_atlantis = demo end) ------
    final = _step_through_bronze_door(client)
    assert_text_contains(final, "ancient secrets")
