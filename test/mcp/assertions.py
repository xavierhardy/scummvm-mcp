#!/usr/bin/env python3
"""
MCP integration assertion utilities
"""


def assert_inventory_does_not_contain(result: dict, item: str) -> None:
    """Assert inventory_removed contains the item (case-insensitive)."""
    removed = result.get("inventory_removed", [])
    assert any(
        i.lower() == item.lower() for i in removed
    ), f"Expected '{item}' in inventory_removed, got {removed}"


def assert_inventory_contains(result: dict, item: str) -> None:
    """Assert inventory_added contains the item (case-insensitive)."""
    added = result.get("inventory_added", [])
    assert any(
        i.lower() == item.lower() for i in added
    ), f"Expected '{item}' in inventory_added, got {added}"


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


def assert_messages_contain(messages: list, substring: str) -> None:
    """Assert some message in *messages* contains *substring* (case-insensitive)."""
    blob = " ".join(m.get("text", "") for m in messages).lower()
    needle = substring.lower()
    assert needle in blob, f"expected a message containing {substring!r}, got {blob!r}"


def assert_message_contains(result: dict, substring: str) -> None:
    """Assert some message in *result* contains *substring* (case-insensitive)."""
    assert_messages_contain(result.get("messages", []), substring)


def assert_no_message_contains(result: dict, substring: str) -> None:
    """Assert no message in *result* contains *substring* (case-insensitive)."""
    blob = " ".join(m.get("text", "") for m in result.get("messages", [])).lower()
    assert substring.lower() not in blob, f"did not expect {substring!r}, got {blob!r}"


def assert_text_contains(text: str, substring: str) -> None:
    """Assert *text* contains *substring* (case-insensitive)."""
    assert substring.lower() in text.lower(), f"expected {substring!r} in {text!r}"


def assert_actor_spoke(result: dict, actor: str) -> None:
    """Assert *actor* produced at least one message in *result*."""
    actors = [m.get("actor") for m in result.get("messages", [])]
    assert actor in actors, f"expected {actor!r} to speak, got actors {actors}"


def _has_letters(text: str) -> bool:
    """True if *text* contains at least one ASCII letter."""
    return any(c.isascii() and c.isalpha() for c in text)


def assert_no_talkie_garbage(messages: list) -> None:
    """Every message must be readable text, not a V6 talkie sound-code fragment.

    The talkie prefix decodes (via the game code page) to a non-breaking space
    (U+00A0) plus stray bytes; a clean line always carries real letters.
    """
    for message in messages:
        text = message.get("text", "")
        assert "\u00a0" not in text, f"talkie garbage leaked into a message: {text!r}"
        assert _has_letters(text), f"message has no readable letters: {text!r}"
