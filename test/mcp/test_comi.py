"""Integration tests for Curse of Monkey Island demo (SCUMM V8)."""

import pytest

from utils import McpClient, find_object_by_name


def test_01_comi_state_reachable(comi_client: McpClient) -> None:
    expected_object_names = {
        "small_pirate",
        "cannon_restraint_rope",
        "cannon",
        "rope",
        "cannon_balls",
        "keyhole",
        "locked_door",
        "grate",
        "ramrod",
    }
    state = comi_client.state()
    assert state.get("room") is not None
    assert isinstance(state.get("objects", []), list)
    assert len(state.get("objects", [])) > 0
    assert state["inventory"] == ["helium_balloons"]
    actual_object_names = {obj["name"] for obj in state["objects"]}
    assert len(actual_object_names & expected_object_names) == len(
        expected_object_names
    )


def test_02_comi_has_verbs(comi_client: McpClient) -> None:
    """Verify Monkey Island 3 (V8) has all 5 core verbs."""
    state = comi_client.state()
    expected_verbs = {"walk to", "talk to", "pick up", "look at", "use"}
    actual_verbs = set(state.get("verbs", []))
    assert expected_verbs.issubset(actual_verbs), (
        f"Missing verbs: {expected_verbs - actual_verbs}"
    )


def test_03_comi_objects_have_verbs(comi_client: McpClient) -> None:
    """Verify objects support verbs."""
    state = comi_client.state()
    objects = state.get("objects", [])

    # At least some objects should support multiple verbs
    multiverb_objects = [
        obj for obj in objects if len(obj.get("compatible_verbs", [])) >= 2
    ]
    assert len(multiverb_objects) > 0, "No objects with multiple verbs found"

    # Check for expected verb support
    obj = multiverb_objects[0]
    assert "walk to" in obj.get("compatible_verbs", [])


def test_04_comi_can_walk(comi_client: McpClient) -> None:
    """Verify walking works."""
    # Find an object to walk to
    result = comi_client.act("walk_to", "rope")
    assert result.get("position") is not None


def test_05_comi_can_look_at_objects(comi_client: McpClient) -> None:
    """Verify looking at objects works."""

    result = comi_client.act("look_at", "cannon_balls")
    # Look action might produce messages or change state
    messages = [msg["text"] for msg in result["messages"]]
    assert "Nice cannon balls." in messages


def test_06_comi_can_interact_with_objects(comi_client: McpClient) -> None:
    """Verify general interaction works."""
    result = comi_client.act("pick_up", "ramrod")
    assert result["inventory_added"] == ["ramrod"]


def test_06a_comi_can_use_different_verbs(comi_client: McpClient) -> None:
    """Verify general interaction works."""
    result = comi_client.act("walk_to", "small_pirate")
    assert result.get("position") is not None

    result = comi_client.act("pick_up", "small_pirate")
    assert "If I rough him up, he may shoot me." in [
        msg["text"] for msg in result["messages"]
    ]
    result = comi_client.act("look_at", "small_pirate")
    assert "I don't think I've ever seen a cuter pirate." in [
        msg["text"] for msg in result["messages"]
    ]


def test_07_comi_can_talk_to_pirate_and_get_dialog(comi_client: McpClient) -> None:
    """Verify talking to small pirate triggers a dialog."""
    result = comi_client.act("talk_to", "small_pirate")
    assert len(result["messages"][0]["text"]) > 0

    assert result["question"] == {
        "choices": [
            {"id": 1, "label": "I'm Guybrush Threepwood, who are you?"},
            {"id": 2, "label": "You don't scare me, you mangy pirate!"},
            {"id": 3, "label": "Hello.  Please don't kill me."},
            {"id": 4, "label": "Aaargh!"},
            {"id": 5, "label": "I'm selling these fine leather jackets."},
            {"id": 6, "label": "Aren't you a little short for a pirate!"},
        ]
    }
    result = comi_client.answer(6)

    assert [msg["text"] for msg in result["messages"]] == [
        "Aren't you a little short for a pirate!",
        "Hold yer tongue, captive!",
        "Or I'll be holdin' it fer ya!",
        "Eeewww!",
    ]

    assert result["question"] == {
        "choices": [
            {"id": 1, "label": "You sound pretty tough."},
            {"id": 2, "label": "Are you wearing a fake beard?"},
            {"id": 3, "label": "Is that a real eyepatch?"},
            {"id": 4, "label": "Is that hook for real?"},
            {"id": 5, "label": "Can I borrow your cannon for a second?"},
            {"id": 6, "label": "It's been swell talking to you."},
        ]
    }
    result = comi_client.answer(6)

    assert "It's been swell talking to you." in [msg["text"] for msg in result["messages"]]


def test_09_comi_can_change_rooms(comi_client: McpClient) -> None:
    """Verify changing rooms works via room transitions."""
    state = comi_client.state()
    initial_room = state.get("room", {}).get("id")

    # Look for a pathway or exit object
    pathways = [
        obj
        for obj in state.get("objects", [])
        if obj.get("pathway") or "door" in obj.get("name", "").lower()
    ]

    # Try to interact with the first pathway/door
    pathway = pathways[0]
    print(f"Trying to use pathway/door: {pathway['name']}")
    result = comi_client.act("walk_to", pathway["name"])

    # Check if room changed
    room_changed = result.get("room_changed")
    assert room_changed != initial_room, "Room should have changed"
