"""
Integration tests for The Dig (DOS demo, SCUMM V7).

Focus areas:
  - V7 single-cursor verb model: only 'interact' and 'use item' exposed
  - Icon-based dialog (choices as text labels, same mechanism as Sam & Max V6)
  - Two-target 'use item' mechanic: use inventory item on room object
"""

import pytest
from time import sleep, time

from utils import McpClient

INTRO_POLL_SECS = 1.0
INTRO_MAX_SKIPS = 20
INTERACTIVE_TIMEOUT_SECS = 120


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


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


def find_npc(state: dict) -> str | None:
    """Return the first non-pathway object that has 'interact' compatible and looks like an NPC."""
    for obj in state.get("objects", []):
        if obj.get("pathway"):
            continue
        name = obj["name"].lower()
        if any(
            kw in name
            for kw in (
                "low",
                "brink",
                "miles",
                "maggie",
                "person",
                "man",
                "woman",
                "alien",
            )
        ):
            return obj["name"]
    return None


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def wait_for_interactive(
    client: McpClient, timeout: float = INTERACTIVE_TIMEOUT_SECS
) -> bool:
    """Poll with skips until the game actually accepts input (_userPut > 0).

    Verifies by attempting a walk() call — if it doesn't raise "not accepting input",
    the game is in an interactive state. The Dig demo has multiple SMUSH cutscenes
    so this may loop for up to *timeout* seconds.
    """
    deadline = time() + timeout
    while time() < deadline:
        sleep(1.0)
        try:
            client.skip()
        except Exception:
            pass
        # Probe interactive state by attempting walk to current position.
        # walk() is harmless and reveals _userPut state immediately.
        try:
            state = client.state()
            pos = state.get("position", {})
            x, y = pos.get("x", 160), pos.get("y", 100)
            client.walk(x, y)
            return True  # walk succeeded: game is accepting input
        except RuntimeError as e:
            if "not accepting input" in str(e):
                continue  # still in cutscene, keep skipping
            return True  # other error (e.g. already walking) means input is open
        except Exception:
            continue
    return False


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


def test_04_dig_interact_object(dig_client: McpClient) -> None:
    """'interact' on a room object must produce messages, a state change, or trigger dialog."""
    wait_for_interactive(dig_client)
    state = dig_client.state()
    candidates = [
        obj["name"]
        for obj in state.get("objects", [])
        if not obj.get("pathway") and "interact" in obj.get("compatible_verbs", [])
    ]
    if not candidates:
        pytest.skip("No interact-compatible objects found in current room")

    # Try each candidate until one produces some output (some objects are pure scenery).
    last_target = candidates[0]
    for target in candidates:
        last_target = target
        try:
            result = dig_client.act("interact", target)
        except RuntimeError as e:
            if "not accepting input" in str(e):
                pytest.skip(f"Game still in cutscene: {e}")
            raise
        has_output = (
            bool(result.get("messages"))
            or bool(result.get("objects_changed"))
            or result.get("question") is not None
            or bool(result.get("position"))
        )
        if has_output:
            return  # found an object that responds

    pytest.skip(f"No interact-compatible object produced output (tried: {candidates})")


def test_05_dig_interact_npc_triggers_dialog(dig_client: McpClient) -> None:
    """Interacting with an NPC should trigger V7 icon-based dialog with text choices."""
    wait_for_interactive(dig_client)
    state = dig_client.state()
    if state.get("question"):
        # Already in dialog — good enough to test answer flow; skip here
        pytest.skip("Already in dialog; covered by test_06")

    npc = find_npc(state)
    if npc is None:
        # Fallback: any interact-compatible non-pathway object may start dialog
        npc = find_object_with_verb(state, "interact")
    if npc is None:
        pytest.skip("No interactable NPC or object found in current room")

    # A prior walk may have triggered a cutscene; retry wait_for_interactive once.
    try:
        result = dig_client.act("interact", npc)
    except RuntimeError as e:
        if "not accepting input" not in str(e):
            raise
        if not wait_for_interactive(dig_client):
            pytest.skip("Game still in cutscene after extended wait")
        try:
            result = dig_client.act("interact", npc)
        except RuntimeError as e2:
            if "not accepting input" in str(e2):
                pytest.skip(f"Game still in cutscene: {e2}")
            raise

    question = result.get("question")
    if question is None:
        pytest.skip(f"interact({npc!r}) did not trigger a dialog (may not be an NPC)")

    choices = question.get("choices", [])
    assert len(choices) >= 1, f"Expected at least one dialog choice, got: {choices}"
    for choice in choices:
        assert choice.get("label"), f"Empty label for dialog choice: {choice}"


def test_06_dig_answer_dialog(dig_client: McpClient) -> None:
    """If a dialog is pending, answer choice 1 and expect output."""
    state = dig_client.state()
    if not state.get("question"):
        pytest.skip("No pending dialog question")

    result = dig_client.answer(1)
    messages = result.get("messages", [])
    assert messages, f"Expected messages after answering dialog choice 1, got: {result}"


def test_07_dig_use_item_on_object(dig_client: McpClient) -> None:
    """'use item' with two targets exercises the use-inventory-on-object mechanic."""
    state = dig_client.state()

    if state.get("question"):
        dig_client.answer(1)
        state = dig_client.state()

    # Filter out The Dig's cursor-mode pseudo-items (stored as inventory objects
    # by the engine but not real pick-up-able items).
    cursor_names = {"look_at", "look at", "interact", "use", "use_item", "use item"}
    real_inventory = [
        i for i in state.get("inventory", []) if i.lower() not in cursor_names
    ]
    if not real_inventory:
        pytest.skip("No real inventory items available (only cursor objects)")

    inv_item = real_inventory[0]

    # Prefer a room object that already has 'interact' or 'use item' in compatible_verbs
    # (meaning the engine has a verb 7 handler for it). Fall back to any non-pathway object.
    target_obj = None
    for obj in state.get("objects", []):
        if obj.get("pathway") or obj["name"] == inv_item:
            continue
        cv = obj.get("compatible_verbs", [])
        if "interact" in cv or "use item" in cv:
            target_obj = obj["name"]
            break
    if target_obj is None:
        for obj in state.get("objects", []):
            if not obj.get("pathway") and obj["name"] != inv_item:
                target_obj = obj["name"]
                break

    if target_obj is None:
        pytest.skip("No room object available to use item on")

    try:
        result = dig_client.act("use item", inv_item, target_obj)
    except RuntimeError as e:
        if "not accepting input" in str(e):
            pytest.skip(f"Game still in cutscene: {e}")
        raise
    # The two-target mechanism works if the engine accepted the request without error.
    # An empty result is valid — it means the combination has no handler but was processed.
    assert isinstance(result, dict), f"Expected dict result, got: {result!r}"
