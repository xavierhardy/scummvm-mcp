"""Integration tests for Curse of Monkey Island demo (SCUMM V8)."""

import pytest
from utils import McpClient


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
    assert state.get("room") is not None, f"expected a room in state, got: {state}"
    assert isinstance(
        state.get("objects", []), list
    ), f"expected 'objects' to be a list, got: {type(state.get('objects'))}"
    assert len(state.get("objects", [])) > 0, "expected at least one object in the room"
    assert state["inventory"] == [
        "helium_balloons"
    ], f"expected the starting inventory to be exactly ['helium_balloons'], got: {state['inventory']}"
    actual_object_names = {obj["name"] for obj in state["objects"]}
    missing = expected_object_names - actual_object_names
    assert not missing, (
        f"expected room objects missing from state: {sorted(missing)}; "
        f"present: {sorted(actual_object_names)}"
    )


def test_02_comi_has_verbs(comi_client: McpClient) -> None:
    """Verify Monkey Island 3 (V8) has all 5 core verbs."""
    state = comi_client.state()
    expected_verbs = {"walk to", "talk to", "pick up", "look at", "use"}
    actual_verbs = set(state.get("verbs", []))
    assert expected_verbs.issubset(
        actual_verbs
    ), f"Missing verbs: {expected_verbs - actual_verbs}"


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
    assert "walk to" in obj.get(
        "compatible_verbs", []
    ), f"expected {obj['name']!r} to support 'walk to', got: {obj.get('compatible_verbs')}"


def test_04_comi_can_walk(comi_client: McpClient) -> None:
    """Verify walking works."""
    # Find an object to walk to
    result = comi_client.act("walk_to", "rope")
    assert (
        result.get("position") is not None
    ), f"walk_to 'rope' should report the ego position, got: {result}"


def test_05_comi_can_look_at_objects(comi_client: McpClient) -> None:
    """Verify looking at objects works."""

    result = comi_client.act("look_at", "cannon_balls")
    # Look action might produce messages or change state
    messages = [msg["text"] for msg in result["messages"]]
    assert (
        "Nice cannon balls." in messages
    ), f"look_at 'cannon_balls' should say 'Nice cannon balls.', got: {messages}"


def test_06_comi_can_interact_with_objects(comi_client: McpClient) -> None:
    """Verify general interaction works."""
    result = comi_client.act("pick_up", "ramrod")
    assert result["inventory_added"] == [
        "ramrod"
    ], f"pick_up 'ramrod' should add it to inventory, got inventory_added={result.get('inventory_added')}"


def test_06a_comi_can_use_different_verbs(comi_client: McpClient) -> None:
    """Verify general interaction works."""
    result = comi_client.act("walk_to", "small_pirate")
    assert (
        result.get("position") is not None
    ), f"walk_to 'small_pirate' should report the ego position, got: {result}"

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
    assert (
        len(result["messages"][0]["text"]) > 0
    ), f"talk_to 'small_pirate' should produce a spoken line, got: {result.get('messages')}"

    assert result["question"] == {
        "choices": [
            {"id": 1, "label": "I'm Guybrush Threepwood, who are you?"},
            {"id": 2, "label": "You don't scare me, you mangy pirate!"},
            {"id": 3, "label": "Hello. Please don't kill me."},
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

    assert "It's been swell talking to you." in [
        msg["text"] for msg in result["messages"]
    ]
    state = comi_client.state()
    assert set(state["verbs"]) == {"walk to", "talk to", "pick up", "look at", "use"}


def test_07b_comi_longest_pirate_exchange_no_timeout(comi_client: McpClient) -> None:
    """Stream the longest single Wally exchange without hitting MCP timeout.

    'Is that hook for real?' is the longest dialog interaction with the small
    pirate (14 lines, ~40s of streaming) — the kind of multi-line exchange
    that previously caused the SSE stream to close prematurely (stuck-detection
    fired between lines, or the 600-frame hard timeout fired mid-exchange).
    Testing just this one exchange keeps the suite fast while still soaking
    the long-stream path; the other topics are 2-7 lines each.
    """
    result = comi_client.act("talk_to", "small_pirate")
    question = result.get("question")
    assert question

    result = comi_client.answer(1)
    question = result.get("question")
    assert question

    hook_id = next(
        (c["id"] for c in question["choices"] if "hook" in c["label"].lower()), None
    )

    result = comi_client.answer(hook_id)
    messages = [m["text"] for m in result["messages"]]
    assert messages[0] == "Is that hook for real?"
    assert len(messages) >= 10, f"expected the long hook exchange, got {messages}"
    assert (
        "Captain LeChuck says he'll cut my hand off when he gets some free time."
        in messages
    )

    # Close the conversation with the farewell so later tests see normal verbs.
    question = result.get("question")
    assert question is not None, "expected the topic list to reopen after the exchange"


def test_09_comi_can_change_rooms(comi_client: McpClient) -> None:
    """Verify changing rooms works via room transitions."""
    state = comi_client.state()
    initial_room = state.get("room", {}).get("id")

    # Look for a pathway or exit object
    pathways = [obj for obj in state.get("objects", []) if obj.get("pathway")]

    # Try to interact with the first pathway/door
    pathway = pathways[0]
    print(f"Trying to use pathway: {pathway['name']}")
    result = comi_client.act("walk_to", pathway["name"])

    # room_changed is only present when the room actually changed
    assert (
        "room_changed" in result
    ), "Room should have changed (room_changed missing from result)"
    assert result["room_changed"] != initial_room
