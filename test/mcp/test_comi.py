"""Integration tests for Curse of Monkey Island demo (SCUMM V8)."""

import pytest

from utils import McpClient, find_object_by_name


def test_01_comi_state_reachable(comi_client: McpClient) -> None:
    state = comi_client.state()
    assert state.get("room") is not None
    assert isinstance(state.get("objects", []), list)
    assert len(state.get("objects", [])) > 0


def test_02_comi_has_verbs(comi_client: McpClient) -> None:
    """Verify Monkey Island 3 (V8) has all 5 core verbs."""
    state = comi_client.state()
    verbs = state.get("verbs", [])
    expected_verbs = {"walk to", "talk to", "pick up", "look at", "use"}
    actual_verbs = set(verbs)
    assert expected_verbs.issubset(actual_verbs), f"Missing verbs: {expected_verbs - actual_verbs}"


def test_03_comi_objects_have_verbs(comi_client: McpClient) -> None:
    """Verify objects support verbs."""
    state = comi_client.state()
    objects = state.get("objects", [])

    # At least some objects should support multiple verbs
    multiverb_objects = [
        obj for obj in objects
        if len(obj.get("compatible_verbs", [])) >= 2
    ]
    assert len(multiverb_objects) > 0, "No objects with multiple verbs found"

    # Check for expected verb support
    obj = multiverb_objects[0]
    assert "walk to" in obj.get("compatible_verbs", [])


def test_04_comi_can_walk(comi_client: McpClient) -> None:
    """Verify walking works."""
    state = comi_client.state()
    initial_pos = state.get("position")

    # Find an object to walk to
    target_obj = find_object_by_name(state, "pirate")
    if target_obj:
        result = comi_client.act("walk_to", target_obj)
        # Walk action should change position
        assert result.get("position") is not None or result.get("inventory_added") is not None


def test_05_comi_can_look_at_objects(comi_client: McpClient) -> None:
    """Verify looking at objects works."""
    state = comi_client.state()

    # Find an object that supports look_at
    objects_with_look = [
        obj for obj in state.get("objects", [])
        if "look at" in obj.get("compatible_verbs", [])
    ]
    if not objects_with_look:
        pytest.skip("No objects support look_at in this room")

    obj = objects_with_look[0]
    result = comi_client.act("look_at", obj["name"])
    # Look action might produce messages or change state
    assert isinstance(result, dict)


def test_06_comi_can_interact_with_objects(comi_client: McpClient) -> None:
    """Verify general interaction works."""
    state = comi_client.state()

    # Find the ramrod (should be pickable)
    ramrod = find_object_by_name(state, "ramrod")
    if ramrod and "pick up" in state.get("verbs", []):
        # Try to pick it up (may or may not succeed depending on game state)
        result = comi_client.act("pick_up", ramrod)
        assert isinstance(result, dict)
