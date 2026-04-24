"""
Integration test for Maniac Mansion C64 demo.
Walkthrough: mailbox -> flags -> bushes -> front door -> key -> use key.
"""
import pytest
from utils import McpClient


def assert_inventory_contains(result: dict, item: str) -> None:
    """Assert inventory_added contains the item (case-insensitive)."""
    added = result.get("inventory_added", [])
    assert any(i.lower() == item.lower() for i in added), (
        f"Expected '{item}' in inventory_added, got {added}"
    )


def test_maniac_initial_state(maniac_client: McpClient) -> None:
    """Verify initial game state."""
    state = maniac_client.state()
    assert "room" in state
    assert state.get("room") is not None
    assert state.get("objects") is not None


def test_maniac_open_mailbox(maniac_client: McpClient) -> None:
    """Open mailbox."""
    result = maniac_client.act("open", "mailbox")
    assert result.get("messages") or result.get("position") or result.get("room_changed")


def test_maniac_pull_flag(maniac_client: McpClient) -> None:
    """Pull flag."""
    result = maniac_client.act("pull", "flag")
    assert result.get("messages") or result.get("position") or result.get("room_changed")


def test_maniac_pull_bushes(maniac_client: McpClient) -> None:
    """Pull bushes."""
    result = maniac_client.act("pull", "bushes")
    assert result.get("messages") or result.get("position") or result.get("room_changed")


def test_maniac_walk_to_front_door(maniac_client: McpClient) -> None:
    """Walk to front door."""
    result = maniac_client.act("walk_to", "front door")
    assert result.get("position") or result.get("room_changed")


def test_maniac_pull_door_mat(maniac_client: McpClient) -> None:
    """Pull door mat."""
    result = maniac_client.act("pull", "door mat")
    assert result.get("messages") or result.get("position") or result.get("room_changed")


def test_maniac_pickup_key(maniac_client: McpClient) -> None:
    """Pick up key."""
    result = maniac_client.act("pick_up", "key")
    assert_inventory_contains(result, "key")


def test_maniac_use_key_on_door(maniac_client: McpClient) -> None:
    """Use key on front door."""
    result = maniac_client.act("use", "key", "front door")
    assert result.get("messages") or result.get("room_changed") or result.get("position")


def test_maniac_walk_through_front_door(maniac_client: McpClient) -> None:
    """Walk to front door (should enter new room)."""
    result = maniac_client.act("walk_to", "front door")
    assert result.get("room_changed") or result.get("position")
