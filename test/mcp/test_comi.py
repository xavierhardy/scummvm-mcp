"""Integration tests for Curse of Monkey Island demo (SCUMM V8)."""

import pytest
from assertions import assert_message_present
from utils import (
    McpClient,
    find_choice_id_containing,
    make_verbs,
    message_texts,
    object_names,
    pathways,
)

EXPECTED_OBJECTS = {
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
CORE_VERBS = {"walk to", "talk to", "pick up", "look at", "use"}


def _objects_with_multiple_verbs(state: dict) -> list:
    """Return room objects advertising at least two compatible verbs."""
    return [
        o for o in state.get("objects", []) if len(o.get("compatible_verbs", [])) >= 2
    ]


def test_01_comi_state_reachable(comi_client: McpClient) -> None:
    state = comi_client.state()
    objects = state.get("objects", [])
    assert state.get("room") is not None, f"expected a room in state, got: {state}"
    assert isinstance(objects, list), f"expected 'objects' to be a list, got: {objects}"
    assert len(objects) > 0, "expected at least one object in the room"
    inventory = state["inventory"]
    assert inventory == ["helium_balloons"], f"unexpected inventory: {inventory}"
    missing = EXPECTED_OBJECTS - object_names(state)
    assert not missing, f"expected room objects missing from state: {sorted(missing)}"


def test_02_comi_has_verbs(comi_client: McpClient) -> None:
    """Verify Monkey Island 3 (V8) has all 5 core verbs."""
    actual_verbs = set(comi_client.state().get("verbs", []))
    assert CORE_VERBS.issubset(actual_verbs), f"missing: {CORE_VERBS - actual_verbs}"


def test_03_comi_objects_have_verbs(comi_client: McpClient) -> None:
    """Verify objects support verbs."""
    multiverb = _objects_with_multiple_verbs(comi_client.state())
    assert multiverb, "no objects with multiple verbs found"
    verbs = multiverb[0].get("compatible_verbs", [])
    assert "walk to" in verbs, f"first multi-verb object can't walk: {verbs}"


def test_04_comi_can_walk(comi_client: McpClient) -> None:
    """Verify walking works."""
    (walk_to,) = make_verbs(comi_client, "walk_to")
    position = walk_to("rope").get("position")
    assert position is not None, f"walk_to 'rope' reported no position: {position}"


def test_05_comi_can_look_at_objects(comi_client: McpClient) -> None:
    """Verify looking at objects works."""
    (look_at,) = make_verbs(comi_client, "look_at")
    assert_message_present(look_at("cannon_balls"), "Nice cannon balls.")


def test_06_comi_can_interact_with_objects(comi_client: McpClient) -> None:
    """Verify general interaction works."""
    (pick_up,) = make_verbs(comi_client, "pick_up")
    added = pick_up("ramrod")["inventory_added"]
    assert added == ["ramrod"], f"pick_up 'ramrod' should add it, got {added}"


def test_06a_comi_can_use_different_verbs(comi_client: McpClient) -> None:
    """Verify general interaction works."""
    walk_to, pick_up, look_at = make_verbs(comi_client, "walk_to", "pick_up", "look_at")
    position = walk_to("small_pirate").get("position")
    assert position is not None, f"no position from walk_to small_pirate: {position}"
    assert_message_present(
        pick_up("small_pirate"), "If I rough him up, he may shoot me."
    )
    assert_message_present(
        look_at("small_pirate"), "I don't think I've ever seen a cuter pirate."
    )


def test_07_comi_can_talk_to_pirate_and_get_dialog(comi_client: McpClient) -> None:
    """Verify talking to small pirate triggers a dialog."""
    (talk_to,) = make_verbs(comi_client, "talk_to")
    result = talk_to("small_pirate")
    first_line = result["messages"][0]["text"]
    assert first_line, "talk_to 'small_pirate' produced no spoken line"

    first_question = {
        "choices": [
            {"id": 1, "label": "I'm Guybrush Threepwood, who are you?"},
            {"id": 2, "label": "You don't scare me, you mangy pirate!"},
            {"id": 3, "label": "Hello. Please don't kill me."},
            {"id": 4, "label": "Aaargh!"},
            {"id": 5, "label": "I'm selling these fine leather jackets."},
            {"id": 6, "label": "Aren't you a little short for a pirate!"},
        ]
    }
    question = result["question"]
    assert question == first_question, f"unexpected first question: {question}"

    result = comi_client.answer(6)
    expected_lines = [
        "Aren't you a little short for a pirate!",
        "Hold yer tongue, captive!",
        "Or I'll be holdin' it fer ya!",
        "Eeewww!",
    ]
    lines = message_texts(result)
    assert lines == expected_lines, f"unexpected exchange: {lines}"

    second_question = {
        "choices": [
            {"id": 1, "label": "You sound pretty tough."},
            {"id": 2, "label": "Are you wearing a fake beard?"},
            {"id": 3, "label": "Is that a real eyepatch?"},
            {"id": 4, "label": "Is that hook for real?"},
            {"id": 5, "label": "Can I borrow your cannon for a second?"},
            {"id": 6, "label": "It's been swell talking to you."},
        ]
    }
    question = result["question"]
    assert question == second_question, f"unexpected second question: {question}"

    result = comi_client.answer(6)
    assert_message_present(result, "It's been swell talking to you.")
    verbs = set(comi_client.state()["verbs"])
    assert verbs == CORE_VERBS, f"expected the core verb bar to return, got {verbs}"


@pytest.mark.slow
def test_07b_comi_longest_pirate_exchange_no_timeout(comi_client: McpClient) -> None:
    """Stream the longest single Wally exchange without hitting MCP timeout.

    'Is that hook for real?' is the longest dialog interaction with the small
    pirate (14 lines, ~40s of streaming) — the kind of multi-line exchange
    that previously caused the SSE stream to close prematurely (stuck-detection
    fired between lines, or the 600-frame hard timeout fired mid-exchange).
    Testing just this one exchange keeps the suite fast while still soaking
    the long-stream path; the other topics are 2-7 lines each.
    """
    (talk_to,) = make_verbs(comi_client, "talk_to")
    question = talk_to("small_pirate").get("question")
    assert question, "talking to the pirate should open the topic list"

    question = comi_client.answer(1).get("question")
    assert question, "the first topic should reopen the menu"

    hook_id = find_choice_id_containing(question, "hook")
    result = comi_client.answer(hook_id)
    messages = message_texts(result)
    assert messages[0] == "Is that hook for real?", f"unexpected first line: {messages}"
    assert len(messages) >= 10, f"expected the long hook exchange, got {messages}"
    assert_message_present(
        result,
        "Captain LeChuck says he'll cut my hand off when he gets some free time.",
    )

    # Close the conversation with the farewell so later tests see normal verbs.
    reopened = result.get("question")
    assert reopened is not None, "expected the topic list to reopen after the exchange"


def test_09_comi_can_change_rooms(comi_client: McpClient) -> None:
    """Verify changing rooms works via room transitions."""
    state = comi_client.state()
    initial_room = state.get("room", {}).get("id")
    (walk_to,) = make_verbs(comi_client, "walk_to")

    pathway = pathways(state)[0]
    result = walk_to(pathway["name"])

    # room_changed is only present when the room actually changed.
    assert "room_changed" in result, "room should have changed (room_changed missing)"
    new_room = result["room_changed"]
    assert new_room != initial_room, f"room did not change from {initial_room}"
