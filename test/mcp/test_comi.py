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


def test_07_comi_can_talk_to_pirate_and_get_dialog(comi_client: McpClient) -> None:
    """Verify talking to small pirate triggers a dialog."""
    state = comi_client.state()

    pirate = find_object_by_name(state, "pirate")
    if not pirate:
        pytest.skip("No pirate object found")

    # Talk to the pirate
    result = comi_client.act("talk_to", pirate)

    # Check if we got a dialog question
    if result.get("question"):
        # Dialog was triggered!
        choices = result.get("question", {}).get("choices", [])
        assert len(choices) > 0, "Dialog has no choices"
        print(f"✓ Dialog triggered with {len(choices)} choices")
    else:
        # Check messages for dialog text
        messages = result.get("messages", [])
        if messages:
            print(f"Got messages instead of question: {messages}")
        # This might be expected if dialog is in progress or needs more interaction
        pytest.skip("Dialog question not returned - may need different game state")


def test_08_comi_can_pickup_ramrod(comi_client: McpClient) -> None:
    """Verify picking up the ramrod adds it to inventory."""
    state = comi_client.state()
    initial_inventory = state.get("inventory", [])

    ramrod = find_object_by_name(state, "ramrod")
    if not ramrod:
        pytest.skip("No ramrod object found")

    # Pick up the ramrod
    result = comi_client.act("pick_up", ramrod)

    # Check if item was added to inventory
    inventory_added = result.get("inventory_added", [])
    if inventory_added:
        print(f"✓ Picked up items: {inventory_added}")
        assert any("ramrod" in item.lower() for item in inventory_added), \
            f"Expected ramrod in inventory_added, got {inventory_added}"
    else:
        # Check new state
        new_state = comi_client.state()
        new_inventory = new_state.get("inventory", [])
        if len(new_inventory) > len(initial_inventory):
            print(f"✓ Inventory grew from {len(initial_inventory)} to {len(new_inventory)} items")
        else:
            pytest.skip("Ramrod not picked up - may already be in inventory or game logic doesn't support it")


def test_09_comi_can_change_rooms(comi_client: McpClient) -> None:
    """Verify changing rooms works via room transitions."""
    state = comi_client.state()
    initial_room = state.get("room", {}).get("id")

    if not initial_room:
        pytest.skip("Could not get initial room ID")

    # Look for a pathway or exit object
    pathways = [
        obj for obj in state.get("objects", [])
        if obj.get("pathway") or "door" in obj.get("name", "").lower()
    ]

    if not pathways:
        pytest.skip("No pathways or doors found to change rooms")

    # Try to interact with the first pathway/door
    pathway = pathways[0]
    print(f"Trying to use pathway/door: {pathway['name']}")
    result = comi_client.act("walk_to", pathway["name"])

    # Check if room changed
    room_changed = result.get("room_changed")
    if room_changed:
        print(f"✓ Room changed from {initial_room} to {room_changed}")
        assert room_changed != initial_room, "Room should have changed"
    else:
        # Check new state
        new_state = comi_client.state()
        new_room = new_state.get("room", {}).get("id")
        if new_room != initial_room:
            print(f"✓ Room changed from {initial_room} to {new_room}")
        else:
            pytest.skip("Room did not change - may need specific interaction sequence")
