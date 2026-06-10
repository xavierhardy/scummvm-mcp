"""
Integration test for Maniac Mansion C64 demo.
Walkthrough: door mat -> key -> use key -> front door,
then kid switching via the switch_character tool, and
phone dialing via the dial tool (save slot 2).
"""

from time import sleep

import pytest

from assertions import assert_inventory_contains
from utils import McpClient

PHONE_ROOM = 5
DIAL_PAD_ROOM = 43


def test_01_maniac_initial_state(maniac_client: McpClient) -> None:
    """Verify initial game state."""
    state = maniac_client.state()
    assert "room" in state
    assert isinstance(state.get("room"), dict)
    assert state["room"]["id"] == 1
    assert state.get("objects") is not None


def test_02_maniac_walk_to_front_door(maniac_client: McpClient) -> None:
    """Walk to front door."""
    result = maniac_client.act("walk_to", "front_door")
    assert "x" in result["position"]
    assert "y" in result["position"]


def test_03_maniac_pull_door_mat(maniac_client: McpClient) -> None:
    """Pull door mat."""
    result = maniac_client.act("pull", "door mat")
    assert result["objects_changed"][0]["name"] == "door mat"
    assert "x" in result["position"]
    assert "y" in result["position"]


def test_04_maniac_pickup_key(maniac_client: McpClient) -> None:
    """Pick up key."""
    result = maniac_client.act("pick_up", "key")
    assert_inventory_contains(result, "key")


def test_05_maniac_use_key_on_door(maniac_client: McpClient) -> None:
    """Unlock front door with key."""
    # for some reason, re-using the same client fails here
    result = maniac_client.act("use", "key", "front_door")
    assert result["objects_changed"][0]["name"] == "front door"


def test_06_maniac_walk_through_front_door(maniac_client: McpClient) -> None:
    """Walk to front door (should enter new room)."""
    result = maniac_client.act("walk_to", "front_door")
    assert result.get("room_changed")


def test_07_maniac_state_lists_characters(maniac_client: McpClient) -> None:
    """state() exposes the switchable kids and the currently controlled one."""
    state = maniac_client.state()
    characters = state.get("available_characters")
    assert characters, f"Expected switchable characters in state, got {state}"
    assert len(characters) >= 2
    assert state.get("controlling") in characters


def test_08_maniac_switch_character_round_trip(maniac_client: McpClient) -> None:
    """Switch to another kid and back, verifying state tracks the change."""
    state = maniac_client.state()
    characters = state["available_characters"]
    original = state["controlling"]
    other = next(c for c in characters if c != original)

    maniac_client.switch_character(other)
    assert maniac_client.state()["controlling"] == other

    maniac_client.switch_character(original)
    assert maniac_client.state()["controlling"] == original


def test_09_maniac_switch_character_rejects_unknown_name(
    maniac_client: McpClient,
) -> None:
    """Switching to a non-switchable name fails with the available kids listed."""
    with pytest.raises(RuntimeError, match="unknown character"):
        maniac_client.switch_character("purple tentacle")


def test_10_maniac_dial_requires_dial_pad(maniac_phone_client: McpClient) -> None:
    """dial() is rejected while the dial pad is not on screen."""
    state = maniac_phone_client.state()
    assert state["room"]["id"] == PHONE_ROOM
    assert any(o["name"] == "phone" for o in state["objects"])
    with pytest.raises(RuntimeError, match="no dial pad"):
        maniac_phone_client.dial("1234")


def test_11_maniac_use_phone_then_dial(maniac_phone_client: McpClient) -> None:
    """Use the phone, wait for the dial pad, then dial a 4-digit number.

    Each keypad press echoes its digit as a message — which also validates the
    button-grid mapping — and after the 4th digit the call resolves and the
    game returns to the phone room.
    """
    maniac_phone_client.act("use", "phone")
    for _ in range(20):
        if maniac_phone_client.state()["room"]["id"] == DIAL_PAD_ROOM:
            break
        sleep(0.5)
    else:
        raise AssertionError("dial pad room never appeared after using the phone")

    result = maniac_phone_client.dial("1234")
    assert [m["text"] for m in result["messages"]] == ["1", "2", "3", "4"]
    assert result.get("room_changed") == PHONE_ROOM


def test_12_maniac_dial_rejects_invalid_keys(maniac_phone_client: McpClient) -> None:
    """Non-keypad characters are rejected up front."""
    with pytest.raises(RuntimeError, match="invalid keypad key"):
        maniac_phone_client.dial("12a4")
