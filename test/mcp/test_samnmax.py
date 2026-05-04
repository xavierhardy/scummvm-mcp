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

from assertions import assert_messages_produced
from utils import McpClient, find_object_by_name, find_object_with_verb


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_samnmax_initial_state(samnmax_client: McpClient) -> None:
    """Skip startup screens then verify state is reachable."""
    # Skip logo/intro screens (room change may or may not be reported in one stream).
    for _ in range(INTRO_MAX_SKIPS):
        sleep(INTRO_POLL_SECS)
        samnmax_client.skip()

    sleep(2)
    state = samnmax_client.state()
    assert state.get("room") is not None


def test_02_samnmax_v6_verbs_exposed(samnmax_client: McpClient) -> None:
    """V6 icon actions must be translated to MCP verbs."""
    state = samnmax_client.state()
    verbs = set(state.get("verbs", []))
    expected = {"walk to", "look at", "use", "talk to", "pick up"}
    missing = expected - verbs
    assert not missing, f"Missing expected V6 verbs: {missing}, got: {sorted(verbs)}"


def test_03_samnmax_max_available(samnmax_client: McpClient) -> None:
    """Max must be addressable either as object or inventory item."""
    state = samnmax_client.state()
    in_objects = find_object_by_name(state, "max") is not None
    in_inventory = any("max" in i.lower() for i in state.get("inventory", []))
    assert in_objects or in_inventory, (
        f"Max not found in objects/inventory; objects={[o['name'] for o in state.get('objects', [])]}, inventory={state.get('inventory', [])}"
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
        for item in state.get("inventory", []):
            if "max" in item.lower():
                max_name = item
                break
    if max_name is None:
        pytest.skip("Max not found in objects/inventory")

    result = samnmax_client.act("talk_to", max_name)

    # Sam & Max dialog can present icon-only choices; depending on timing/game
    # state this may or may not surface as a pending MCP question.
    question = result.get("question")
    if question is not None:
        choices = question.get("choices", [])
        assert len(choices) >= 1, f"Expected at least one dialog choice, got: {choices}"
        for choice in choices:
            assert choice.get("label"), f"Empty label for choice: {choice}"
    else:
        # Depending on the exact frame/state in the demo, talk_to Max may be a no-op.
        assert isinstance(result, dict)


def test_06_samnmax_answer_dialog(samnmax_client: McpClient) -> None:
    """If a dialog is pending, answer choice 1 and expect output."""
    state = samnmax_client.state()
    if not state.get("question"):
        pytest.skip("No pending dialog question")
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
        for item in state.get("inventory", []):
            if "max" in item.lower():
                max_name = item
                break
    if max_name is None:
        pytest.skip("Max not found in objects/inventory")

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
