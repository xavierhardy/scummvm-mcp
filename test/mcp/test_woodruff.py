"""
Integration test for The Bizarre Adventures of Woodruff and the Schnibble.

Woodruff runs on ScummVM's Gob engine (a script-driven, one-click point-and-
click game — no verb bar, no object model). The MCP bridge derives its semantic
snapshot from the game's rectangular hotspots and the status-bar text the game
draws, and replays real cursor input for every action.

The game has no save support, so the whole walkthrough runs as one ordered
sequence on a single fresh instance (like the atlantis / ft demos), skipping the
intro videos first. It exercises the interactions an agent relies on:

  1. Skip the intro until the game is interactive (Azimuth's house, room 2002).
  2. Read the semantic state: the harvested object/exit names.
  3. Talk to the onlooker -> he explains the house belongs to Professor Azimuth.
  4. Pick up the button (Woodruff's teddy bear eye) -> it enters the inventory
     with its real name.
  5. Walk through the right-hand exit to the Sad Boozook street (room 2003),
     the inventory carrying across the screen change.
  6. Talk to the young woman and examine the inscribed stone there.
"""

import pytest

from mcp_client import McpClient
from woodruff_helpers import (
    ROOM_AZIMUTH,
    ROOM_BOOZOOK,
    act,
    inventory_names,
    messages_text,
    object_names,
    skip_intro,
    wait_in_inventory,
    wait_named_objects,
    wait_room,
)

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("woodruff")]


def test_woodruff_walkthrough(woodruff_client: McpClient) -> None:
    client = woodruff_client

    # --- Skip the intro until the game is interactive ----------------------
    assert skip_intro(client), "intro never became interactive"

    # --- Read the opening screen (goal: reach Azimuth's house) -------------
    assert wait_room(client, ROOM_AZIMUTH), (
        f"expected Azimuth's house (room {ROOM_AZIMUTH})"
    )
    objs = wait_named_objects(client)
    # Named from the game's own status-bar hover text.
    assert "onlooker" in objs, f"onlooker not found among {objs}"
    assert "trash_heap" in objs, f"trash heap not found among {objs}"
    assert "underwear_button" in objs, f"button not found among {objs}"
    # The right-hand exit to the next screen is a named pathway.
    assert any(o.startswith("to_the_street_of_the_sad_boozook") for o in objs), (
        f"exit to the Sad Boozook street not found among {objs}"
    )

    # --- Talk to the onlooker ---------------------------------------------
    # Each interact first walks Woodruff to the onlooker; if he starts too far
    # he ambles part-way and says "come closer", so re-issue until he arrives
    # and delivers the line (re-issuing walks him the rest of the way).
    said = ""
    for _ in range(4):
        said = messages_text(act(client, "interact", "onlooker"))
        if "azimuth" in said.lower():
            break
    assert "azimuth" in said.lower(), f"onlooker should mention Azimuth, got {said!r}"

    # --- Pick up the button (enters the inventory with its real name) ------
    act(client, "interact", "underwear_button")
    assert wait_in_inventory(client, "button"), (
        f"button should be in the inventory, got {inventory_names(client)}"
    )
    # ...and it is no longer a world object.
    assert "underwear_button" not in object_names(client), (
        "button should be gone from the world after pickup"
    )

    # --- Walk right to the Sad Boozook street (inventory carries over) -----
    act(client, "interact", "to_the_street_of_the_sad_boozook")
    assert wait_room(client, ROOM_BOOZOOK), (
        f"expected the Sad Boozook street (room {ROOM_BOOZOOK})"
    )
    assert "button" in inventory_names(client), (
        "the button should still be carried after the screen change"
    )
    objs = wait_named_objects(client)
    assert "young_woman" in objs, f"young woman not found among {objs}"
    assert "sad_boozook" in objs, f"sad Boozook not found among {objs}"

    # --- Talk to the young woman ------------------------------------------
    result = act(client, "interact", "young_woman")
    said = messages_text(result)
    assert said.strip(), "the young woman should say something"

    # --- Examine the inscribed stone (Woodruff cannot read yet) ------------
    # Talking to the young woman rebuilds the screen's hotspots, so wait for the
    # bridge to re-harvest the object names before looking for the stone.
    objs = wait_named_objects(client)
    # Re-examinable, so retry: a single streamed call can occasionally miss the
    # short trailing line when it lands right on the stream's settle boundary.
    if "stone" in objs:
        said = ""
        for _ in range(3):
            said = messages_text(act(client, "interact", "stone"))
            if "read" in said.lower():
                break
        assert "read" in said.lower(), (
            f"the stone should note Woodruff cannot read, got {said!r}"
        )
