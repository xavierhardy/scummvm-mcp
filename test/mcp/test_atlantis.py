"""
Integration test for Indiana Jones: Fate of Atlantis demo.

The demo cannot save/load arbitrary states, so the whole walkthrough is driven
as a single sequential test (the steps depend on each other and must run in
order on one instance). This keeps the file parallel-safe: it is one test on one
fixture/port, so it never has to interleave with itself.

Walkthrough: skip intro -> answer opening dialog -> talk to Sophia -> answer ->
walk to path -> answer (-> room 63) -> walk to mountain cleft -> pick up the
tire repair kit -> close crate -> open crate.
"""

from time import sleep

import pytest
from assertions import assert_inventory_contains, assert_messages_produced
from utils import McpClient

# Fate of Atlantis cannot save/load: the demo is intro-driven and played as one
# ordered walkthrough. Pin it to a single xdist worker (one merged test today,
# but the mark keeps it grouped if it is ever split into steps).
pytestmark = pytest.mark.xdist_group("atlantis")

INTRO_POLL_SECS = 0.5


def test_atlantis_walkthrough(atlantis_client: McpClient) -> None:
    """Drive the whole Fate of Atlantis demo walkthrough in one sequential run."""

    # --- Skip the intro and reach the first interactive state -------------
    result = atlantis_client.skip()
    while "room_changed" not in result:
        sleep(INTRO_POLL_SECS)
        result = atlantis_client.skip()

    old_room_id = result["room_changed"]
    new_room_id = atlantis_client.state()["room"]["id"]
    while new_room_id == old_room_id:
        sleep(INTRO_POLL_SECS)
        new_room_id = atlantis_client.state()["room"]["id"]

    # There is a second screen that needs us to wait.
    old_room_id = new_room_id
    new_room_id = atlantis_client.state()["room"]["id"]
    while new_room_id == old_room_id:
        sleep(INTRO_POLL_SECS)
        new_room_id = atlantis_client.state()["room"]["id"]

    atlantis_client.skip()

    state = atlantis_client.state()
    assert (
        state.get("room") is not None
    ), f"[intro] state has no 'room' after skipping the intro: {state}"
    # Should have an opening dialog question.
    assert (
        state.get("question") is not None
    ), f"[intro] expected the opening dialog question after the intro, got state: {state}"

    # --- Answer the opening dialog ---------------------------------------
    result = atlantis_client.answer(4)
    assert result["messages"][0] == {
        "text": "Let's take a look around.",
        "actor": "indy",
    }, (
        f"[opening dialog] answer(4) should make Indy say 'Let's take a look around.', "
        f"got: {result.get('messages')}"
    )

    # --- Talk to Sophia ---------------------------------------------------
    result = atlantis_client.act("talk_to", "sophia")
    assert (
        result.get("question") is not None
    ), f"[talk_to sophia] expected a dialog question, got: {result}"
    assert any(
        choice
        == {"id": 1, "label": "What if Atlantis was vaporized when Thera exploded?"}
        for choice in result["question"]["choices"]
    ), (
        f"[talk_to sophia] expected the 'Atlantis vaporized' topic as choice 1, "
        f"got: {result['question']['choices']}"
    )

    # --- Answer Sophia's dialog -------------------------------------------
    atlantis_client.state()
    result = atlantis_client.answer(4)
    assert_messages_produced(result)

    # --- Walk to the path away from the dock ------------------------------
    result = atlantis_client.act("walk_to", "path away from dock")
    assert (
        "x" in result["position"] and "y" in result["position"]
    ), f"[walk_to path] expected a position (x,y) in the result, got: {result}"

    # --- Answer dialog choice 2 (-> room 63) ------------------------------
    result = atlantis_client.answer(2)
    assert_messages_produced(result)
    assert result["messages"][0] == {
        "text": "I want to see if our friend Kerner has been here.",
        "actor": "indy",
    }, f"[answer 2] expected Indy's 'friend Kerner' line first, got: {result.get('messages')}"
    assert (
        result["messages"][1]["actor"] == "sophia"
    ), f"[answer 2] expected Sophia to reply second, got: {result.get('messages')}"
    assert (
        result["room_changed"] == 63
    ), f"[answer 2] expected the walk to advance into room 63, got room_changed={result.get('room_changed')}"

    # --- Walk to the cleft in the mountain --------------------------------
    result = atlantis_client.act("walk_to", "notch in mountain")
    if not result.get("room_changed"):
        result = atlantis_client.act("walk_to", "cleft in mountain")
        if not result.get("room_changed"):
            result = atlantis_client.act("walk_to", "gap in mountain")
    assert result.get(
        "room_changed"
    ), f"[walk_to mountain] none of notch/cleft/gap triggered a room change, got: {result}"

    # --- Pick up the tire repair kit --------------------------------------
    result = atlantis_client.act("pick_up", "tire repair kit")
    assert_inventory_contains(result, "tire_repair_kit")

    # --- Close the crate --------------------------------------------------
    result = atlantis_client.act("close", "crate")
    assert (
        "x" in result["position"] and "y" in result["position"]
    ), f"[close crate] expected a position (x,y) in the result, got: {result}"
    assert (
        result["objects_changed"][0]["name"] == "crate"
    ), f"[close crate] expected the crate to change state, got: {result.get('objects_changed')}"

    # --- Open the crate ---------------------------------------------------
    result = atlantis_client.act("open", "crate")
    assert (
        result["objects_changed"][0]["name"] == "crate"
    ), f"[open crate] expected the crate to change state, got: {result.get('objects_changed')}"
