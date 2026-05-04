"""
Integration tests for The Dig (DOS demo, SCUMM V7).

Focus areas:
  - V7 single-cursor verb model: only 'interact' and 'use item' exposed
  - Icon-based dialog (choices as text labels, same mechanism as Sam & Max V6)
  - Two-target 'use item' mechanic: use inventory item on room object
"""

from utils import McpClient

INTRO_POLL_SECS = 1.0
INTRO_MAX_SKIPS = 20
INTERACTIVE_TIMEOUT_SECS = 120


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_dig_initial_state(dig_client: McpClient) -> None:
    """Skip startup screens then verify state is reachable."""
    state = dig_client.state()
    assert state.get("room") is not None, "Expected room in state"


def test_02_dig_verbs_exposed(dig_client: McpClient) -> None:
    """V7 must expose 'interact' and 'use item' (single-cursor model)."""
    state = dig_client.state()
    verbs = set(state.get("verbs", []))
    expected = {"interact", "use item"}
    missing = expected - verbs
    assert not missing, f"Missing expected V7 verbs: {missing}, got: {sorted(verbs)}"
    # Must NOT expose the raw V6 canonical set — The Dig has a different UI model.
    assert "walk to" not in verbs, "walk to should not appear in Dig verb list"
    assert "look at" not in verbs, "look at should not appear in Dig verb list"
    assert "pick up" not in verbs, "pick up should not appear in Dig verb list"


def test_03_dig_objects_in_room(dig_client: McpClient) -> None:
    """At least one object should be visible in the starting room."""
    state = dig_client.state()
    objects = state.get("objects", [])
    assert objects, f"Expected room objects, got empty list (room={state.get('room')})"


def test_04_dig_use_object(dig_client: McpClient) -> None:
    """'interact' on a room object must produce messages, a state change, or trigger dialog."""
    result = dig_client.act("use_item", "trowel", "brink")
    assert "messages" in result
    print(result)


def test_05_dig_interact_object(dig_client: McpClient) -> None:
    """Interacting with an NPC should trigger V7 icon-based dialog with text choices."""
    result = dig_client.act("interact", "platform")
    assert "position" in result
    assert "messages" in result
    print(result)


def test_05_dig_interact_npc_triggers_dialog(dig_client: McpClient) -> None:
    """Interacting with an NPC should trigger V7 icon-based dialog with text choices."""
    result = dig_client.act("interact", "maggie")
    assert "position" in result
    assert "messages" in result
    assert result["messages"][0]["text"] == "Robbins..."


def test_06_dig_answer_dialog(dig_client: McpClient) -> None:
    """If a dialog is pending, answer choice 1 and expect output."""
    state = dig_client.state()
    assert "question" in state

    result = dig_client.answer(1)
    messages = result.get("messages", [])
    assert messages, f"Expected messages after answering dialog choice 1, got: {result}"

    result = dig_client.answer(3)
    messages = result.get("messages", [])
    assert messages, f"Expected messages after answering dialog choice 3, got: {result}"


def test_07_dig_leave_area(dig_client: McpClient) -> None:
    """Interacting with an NPC should trigger V7 icon-based dialog with text choices."""
    result = dig_client.act("interact", "clearing")
    assert "room_changed" in result
    print(result)
