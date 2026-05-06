"""
Integration test for Indiana Jones: Fate of Atlantis demo.
Walkthrough: answer dialogs -> walk to path -> answer -> walk to cleft -> pickup item -> close crate.
"""

from assertions import assert_messages_produced, assert_inventory_contains
from utils import McpClient
from time import sleep

INTRO_POLL_SECS = 0.5


def test_01_atlantis_initial_state(atlantis_client: McpClient) -> None:
    """Verify initial game state with opening dialog."""

    # skip the intro as much as possible
    result = atlantis_client.skip()
    while "room_changed" not in result:
        sleep(INTRO_POLL_SECS)
        result = atlantis_client.skip()

    old_room_id = result["room_changed"]
    new_room_id = atlantis_client.state()["room"]["id"]
    while new_room_id == old_room_id:
        sleep(INTRO_POLL_SECS)
        new_room_id = atlantis_client.state()["room"]["id"]

    # there is a second screen that needs for us to wait
    old_room_id = new_room_id
    new_room_id = atlantis_client.state()["room"]["id"]
    while new_room_id == old_room_id:
        sleep(INTRO_POLL_SECS)
        new_room_id = atlantis_client.state()["room"]["id"]

    atlantis_client.skip()

    state = atlantis_client.state()
    assert "room" in state
    assert state.get("room") is not None

    # Should have an opening dialog question
    assert state.get("question") is not None


def test_02_atlantis_answer_opening_dialog(atlantis_client: McpClient) -> None:
    """Answer opening dialog choice 4."""
    result = atlantis_client.answer(4)
    assert result["messages"][0] == {
        "text": "Let's take a look around.",
        "actor": "indy",
    }


def test_03_atlantis_talk_to_sophia(atlantis_client: McpClient) -> None:
    """Talk to Sophia."""
    result = atlantis_client.act("talk_to", "sophia")
    assert result.get("question") is not None
    assert any(
        choice
        == {"id": 1, "label": "What if Atlantis was vaporized when Thera exploded?"}
        for choice in result["question"]["choices"]
    )


def test_04_atlantis_answer_sophia_dialog_1(atlantis_client: McpClient) -> None:
    """Answer dialog choice 1 if available."""
    atlantis_client.state()
    result = atlantis_client.answer(4)
    assert_messages_produced(result)


def test_05_atlantis_walk_to_path_away_from_dock(atlantis_client: McpClient) -> None:
    """Walk to path away from dock."""
    result = atlantis_client.act("walk_to", "path away from dock")
    assert "x" in result["position"]
    assert "y" in result["position"]


def test_06_atlantis_answer_dialog_2(atlantis_client: McpClient) -> None:
    """Answer dialog choice 2."""
    result = atlantis_client.answer(2)
    assert_messages_produced(result)
    assert result["messages"][0] == {
        "text": "I want to see if our friend Kerner has been here.",
        "actor": "indy",
    }
    assert result["messages"][1]["actor"] == "sophia"
    assert result["room_changed"] == 63


def test_07_atlantis_walk_to_mountain(atlantis_client: McpClient) -> None:
    """Walk to cleft in mountain."""
    result = atlantis_client.act("walk_to", "notch in mountain")

    if not result.get("room_changed"):
        result = atlantis_client.act("walk_to", "cleft in mountain")

        if not result.get("room_changed"):
            result = atlantis_client.act("walk_to", "gap in mountain")

    assert result.get("room_changed")


def test_08_atlantis_pickup_tire_repair_kit(atlantis_client: McpClient) -> None:
    """Pick up tire repair kit."""
    result = atlantis_client.act("pick_up", "tire repair kit")
    assert_inventory_contains(result, "tire_repair_kit")


def test_09_atlantis_close_crate(atlantis_client: McpClient) -> None:
    """Close crate."""
    result = atlantis_client.act("close", "crate")

    assert "x" in result["position"]
    assert "y" in result["position"]
    assert result["objects_changed"][0]["name"] == "crate"


def test_10_atlantis_open_crate(atlantis_client: McpClient) -> None:
    """Open crate."""
    result = atlantis_client.act("open", "crate")

    assert result["objects_changed"][0]["name"] == "crate"
