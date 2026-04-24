"""
Integration test for Monkey Island 1 EGA demo.
Walkthrough: Troll -> door sequence -> pick up items -> use item with pot.
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


def test_monkey_initial_state(monkey_client: McpClient) -> None:
    """Verify initial game state."""
    state = monkey_client.state()
    assert "room" in state
    assert state.get("room") is not None
    assert state.get("objects") is not None
    assert isinstance(state.get("objects"), list)
    assert len(state.get("objects", [])) > 0


def test_monkey_walk_to_troll(monkey_client: McpClient) -> None:
    """Walk to Troll."""
    result = monkey_client.act("walk_to", "Troll")
    assert result.get("position") or result.get("room_changed"), (
        "Expected position or room_changed after walking"
    )


def test_monkey_talk_to_troll(monkey_client: McpClient) -> None:
    """Talk to Troll to trigger dialog."""
    result = monkey_client.act("talk_to", "Troll")
    # Dialog should present a question
    state = monkey_client.state()
    assert state.get("question") is not None, (
        "Expected dialog question after talking to Troll"
    )


def test_monkey_answer_troll_dialog(monkey_client: McpClient) -> None:
    """Answer dialog choice 3."""
    result = monkey_client.answer(3)
    assert_messages_produced(result)


def test_monkey_walk_to_door_1(monkey_client: McpClient) -> None:
    """Walk to door (first time)."""
    result = monkey_client.act("walk_to", "door")
    assert result.get("position") or result.get("room_changed")


def test_monkey_open_door_1(monkey_client: McpClient) -> None:
    """Open door (first time)."""
    result = monkey_client.act("open", "door")
    # Should produce some state change
    assert result.get("messages") or result.get("position") or result.get("room_changed")


def test_monkey_walk_to_door_2(monkey_client: McpClient) -> None:
    """Walk to door (second time) - enter new room."""
    result = monkey_client.act("walk_to", "door")
    assert result.get("room_changed") or result.get("position")


def test_monkey_pickup_bowl(monkey_client: McpClient) -> None:
    """Pick up bowl o' mints."""
    result = monkey_client.act("pick_up", "bowl o' mints")
    assert_inventory_contains(result, "bowl o' mints")


def test_monkey_open_door_2(monkey_client: McpClient) -> None:
    """Open door (second time)."""
    result = monkey_client.act("open", "door")
    assert result.get("messages") or result.get("position") or result.get("room_changed")


def test_monkey_walk_to_door_3(monkey_client: McpClient) -> None:
    """Walk to door (third time)."""
    result = monkey_client.act("walk_to", "door")
    assert result.get("room_changed") or result.get("position")


def test_monkey_pickup_meat(monkey_client: McpClient) -> None:
    """Pick up hunk o' meat."""
    result = monkey_client.act("pick_up", "hunk o' meat")
    assert_inventory_contains(result, "hunk o' meat")


def test_monkey_use_meat_with_pot(monkey_client: McpClient) -> None:
    """Use hunk o' meat with pot o' soup."""
    result = monkey_client.act("use", "hunk o' meat", "pot o' soup")
    # Should produce messages or some state change
    assert result.get("messages") or result.get("inventory_removed") or result.get("inventory_added")
