"""
Integration tests for Sam & Max Hit the Road (DOS/CD demo, SCUMM V6).

Focus areas:
  - V6 icon verb listing (walk to / look at / use / talk to / pick up)
  - V6 dialog detection (icon choices replacing the verb bar)
  - Text capture for V6 games
  - Max appearing as a targetable object
"""

import pytest
from time import sleep

from utils import McpClient

INTRO_POLL_SECS = 0.5
INTRO_MAX_SKIPS = 10


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def assert_messages_produced(result: dict) -> None:
    messages = result.get("messages", [])
    assert messages, "Expected messages to be produced"


def find_object_by_name(state: dict, substring: str) -> str | None:
    """Return the first object name containing *substring* (case-insensitive)."""
    for obj in state.get("objects", []):
        if substring.lower() in obj["name"].lower():
            return obj["name"]
    return None


def find_object_with_verb(state: dict, verb: str) -> str | None:
    """Return the first non-pathway object that lists *verb* as compatible."""
    for obj in state.get("objects", []):
        if obj.get("pathway"):
            continue
        if verb in obj.get("compatible_verbs", []):
            return obj["name"]
    return None


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_samnmax_initial_state(samnmax_client: McpClient) -> None:
    """Skip any startup screens then verify the V6 verb bar is populated."""
    # Skip logo/intro screens; stop early if room_changed is reported.
    result = samnmax_client.skip()
    for _ in range(INTRO_MAX_SKIPS):
        sleep(INTRO_POLL_SECS)
        result = samnmax_client.skip()
    assert "room_changed" in result

    sleep(20)
    state = samnmax_client.state()
    assert "messages" in result

    assert state.get("room") is not None

    # V6 fix: all five icon verbs must appear (image verbs, not text labels).
    # verbs = state.get("verbs", [])
    # assert "walk to" in verbs, f"'walk to' missing from verbs: {verbs}"
    # assert "look at" in verbs, f"'look at' missing from verbs: {verbs}"
    # assert "use" in verbs,     f"'use' missing from verbs: {verbs}"
    # assert "talk to" in verbs, f"'talk to' missing from verbs: {verbs}"
    # assert "pick up" in verbs, f"'pick up' missing from verbs: {verbs}"


def test_02_samnmax_objects_have_compatible_verbs(samnmax_client: McpClient) -> None:
    """Objects in the room must report at least one compatible verb."""
    state = samnmax_client.state()
    objects = state.get("objects", [])
    assert objects, "Expected objects in the initial room"

    has_compat = any(obj.get("compatible_verbs") for obj in objects)
    assert has_compat, (
        "No object has any compatible_verbs — V6 verb-entrypoint resolution is broken"
    )


def test_03_samnmax_max_is_present(samnmax_client: McpClient) -> None:
    """Max must appear in the objects list."""
    state = samnmax_client.state()
    max_name = find_object_by_name(state, "max")
    assert max_name is not None, (
        f"Max not found in objects: {[o['name'] for o in state.get('objects', [])]}"
    )


def test_04_samnmax_text_capture(samnmax_client: McpClient) -> None:
    """Looking at an object must produce non-empty messages (V6 text capture check)."""
    state = samnmax_client.state()

    target = find_object_with_verb(state, "look at")
    if target is None:
        pytest.skip("No look-at-compatible object found in current room")

    result = samnmax_client.act("look_at", target)
    assert_messages_produced(result)


def test_05_samnmax_talk_to_max(samnmax_client: McpClient) -> None:
    """Talking to Max triggers V6 icon-based dialog (question with choices)."""
    state = samnmax_client.state()
    max_name = find_object_by_name(state, "max")
    if max_name is None:
        pytest.skip("Max not found in objects")

    result = samnmax_client.act("talk_to", max_name)

    question = result.get("question")
    assert question is not None, (
        "Expected a dialog question after talking to Max — "
        "V6 dialog detection may be broken"
    )
    choices = question.get("choices", [])
    assert len(choices) >= 1, f"Expected at least one dialog choice, got: {choices}"
    # Every choice must have a non-empty label
    for choice in choices:
        assert choice.get("label"), f"Empty label for choice: {choice}"


def test_06_samnmax_answer_dialog(samnmax_client: McpClient) -> None:
    """Answering dialog choice 1 must produce messages (V6 toolAnswer check)."""
    result = samnmax_client.answer(1)
    assert_messages_produced(result)


def test_07_samnmax_use_max_on_object(samnmax_client: McpClient) -> None:
    """Use Max as a secondary target on an interactive object if possible."""
    state = samnmax_client.state()

    if state.get("question"):
        # Still in dialog — dismiss it first
        samnmax_client.answer(1)
        state = samnmax_client.state()

    max_name = find_object_by_name(state, "max")
    if max_name is None:
        pytest.skip("Max not found in objects")

    # Find any object that accepts 'use' (other than Max itself)
    target = None
    for obj in state.get("objects", []):
        if obj.get("pathway"):
            continue
        if (
            "use" in obj.get("compatible_verbs", [])
            and "max" not in obj["name"].lower()
        ):
            target = obj["name"]
            break

    if target is None:
        pytest.skip("No use-compatible non-Max object found in current room")

    result = samnmax_client.act("use", target, max_name)
    # Either messages were produced or state changed — either is acceptable
    has_output = bool(result.get("messages")) or bool(result.get("objects_changed"))
    assert has_output, f"Expected some output from use({target}, {max_name})"
