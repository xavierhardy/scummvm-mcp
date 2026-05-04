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
