"""
Integration test for Indiana Jones: Fate of Atlantis demo.
Walkthrough: answer dialogs -> walk to path -> answer -> walk to cleft -> pickup item -> close crate.
"""
import pytest
from utils import McpClient


def assert_inventory_contains(result: dict, item: str) -> None:
    """Assert inventory_added contains the item (case-insensitive)."""
    added = result.get("inventory_added", [])
    assert any(i.lower() == item.lower() for i in added), (
        f"Expected '{item}' in inventory_added, got {added}"
    )


def assert_messages_produced(result: dict) -> None:
    """Assert messages list is non-empty."""
    messages = result.get("messages", [])
    assert messages, "Expected messages to be produced"


def test_atlantis_initial_state(atlantis_client: McpClient) -> None:
    """Verify initial game state with opening dialog."""
    state = atlantis_client.state()
    assert "room" in state
    assert state.get("room") is not None
    # Should have an opening dialog question
    assert state.get("question") is not None


def test_atlantis_answer_opening_dialog(atlantis_client: McpClient) -> None:
    """Answer opening dialog choice 4."""
    result = atlantis_client.answer(4)
    assert_messages_produced(result)


def test_atlantis_walk_to_path_away_from_dock(atlantis_client: McpClient) -> None:
    """Walk to path away from dock."""
    result = atlantis_client.act("walk_to", "path away from dock")
    assert result.get("position") or result.get("room_changed")


def test_atlantis_answer_dialog_2(atlantis_client: McpClient) -> None:
    """Answer dialog choice 2."""
    result = atlantis_client.answer(2)
    assert_messages_produced(result)


def test_atlantis_walk_to_cleft_in_mountain(atlantis_client: McpClient) -> None:
    """Walk to cleft in mountain."""
    result = atlantis_client.act("walk_to", "cleft in mountain")
    assert result.get("position") or result.get("room_changed")


def test_atlantis_pickup_tire_repair_kit(atlantis_client: McpClient) -> None:
    """Pick up tire repair kit."""
    result = atlantis_client.act("pick_up", "tire repair kit")
    assert_inventory_contains(result, "tire repair kit")


def test_atlantis_close_crate(atlantis_client: McpClient) -> None:
    """Close crate."""
    result = atlantis_client.act("close", "crate")
    assert result.get("messages") or result.get("position") or result.get("room_changed")
