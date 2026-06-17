"""
Integration test for Maniac Mansion C64 demo.
Walkthrough: door mat -> key -> use key -> front door,
then kid switching via the switch_character tool.
The phone/dial tests live in test_maniac_phone.py (save slot 2).
"""

import pytest
from assertions import assert_inventory_contains
from utils import McpClient


def _take_key(client: McpClient) -> None:
    """Pull the door mat (revealing the key) and pick the key up.

    Self-contained setup: the key only exists once the mat is pulled, so tests
    that need the key run this first instead of relying on a sibling test.
    """
    if "key" not in client.state().get("inventory", []):
        client.act("pull", "door mat")
        client.act("pick_up", "key")


def _unlock_front_door(client: McpClient) -> None:
    """Take the key and use it on the front door (unlocks it)."""
    _take_key(client)
    client.act("use", "key", "front_door")


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
    """Pick up the key from under the door mat."""
    # Reveal the key first so this test stands alone.
    maniac_client.act("pull", "door mat")
    result = maniac_client.act("pick_up", "key")
    assert_inventory_contains(result, "key")


def test_05_maniac_use_key_on_door(maniac_client: McpClient) -> None:
    """Unlock front door with key."""
    _take_key(maniac_client)
    result = maniac_client.act("use", "key", "front_door")
    assert (
        result["objects_changed"][0]["name"] == "front door"
    ), f"using the key should change the front door state, got: {result.get('objects_changed')}"


def test_06_maniac_walk_through_front_door(maniac_client: McpClient) -> None:
    """Walk through the unlocked front door into the mansion (room change)."""
    _unlock_front_door(maniac_client)
    result = maniac_client.act("walk_to", "front_door")
    assert result.get(
        "room_changed"
    ), f"walking through the unlocked door should change rooms, got: {result}"


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
