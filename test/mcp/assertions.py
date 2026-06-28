#!/usr/bin/env python3
"""
MCP integration assertion utilities
"""


def assert_inventory_does_not_contain(result: dict, item: str) -> None:
    """Assert inventory_removed contains the item (case-insensitive)."""
    removed = result.get("inventory_removed", [])
    assert any(i.lower() == item.lower() for i in removed), (
        f"Expected '{item}' in inventory_removed, got {removed}"
    )


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


def assert_room(state: dict, room_id: int) -> None:
    """Assert the player is currently in *room_id*."""
    room = state.get("room") or {}
    actual = room.get("id")
    assert actual == room_id, f"expected room {room_id}, got {actual}"


def assert_has_position(result: dict) -> None:
    """Assert *result* carries an (x, y) position."""
    position = result.get("position", {})
    assert "x" in position and "y" in position, f"expected x/y position, got {result}"


def assert_message_present(result: dict, text: str) -> None:
    """Assert some message in *result* has exactly *text*."""
    texts = [message["text"] for message in result.get("messages", [])]
    assert text in texts, f"expected message {text!r}, got {texts}"
