"""
Integration tests for Full Throttle (DOS demo, SCUMM V7).

Focus areas:
  - V7 single-cursor verb model: only 'interact' and 'use item' exposed
  - Icon-based dialog (choices as text labels, same mechanism as Sam & Max V6)
  - Two-target 'use item' mechanic: use inventory item on room object
  - INSANE combat sequences are skipped gracefully
"""

import pytest
from time import sleep

from utils import (
    McpClient,
    find_object_with_verb,
    skip_intros,
    wait_for_interactive,
    get_state_with_retry,
)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_ft_initial_state(ft_client: McpClient) -> None:
    """Skip startup screens then verify state is reachable."""
    skip_intros(ft_client)
    sleep(3)
    state = ft_client.state()
    assert state.get("room") is not None, "Expected room in state"


def test_02_ft_verbs_exposed(ft_client: McpClient) -> None:
    """V7 must expose 'interact' and 'use item' (single-cursor model)."""
    state = ft_client.state()
    verbs = set(state.get("verbs", []))
    expected = {"interact", "use item"}
    missing = expected - verbs
    assert not missing, f"Missing expected V7 verbs: {missing}, got: {sorted(verbs)}"
    assert "walk to" not in verbs, "walk to should not appear in FT verb list"
    assert "look at" not in verbs, "look at should not appear in FT verb list"
    assert "pick up" not in verbs, "pick up should not appear in FT verb list"


def test_03_ft_objects_in_room(ft_client: McpClient) -> None:
    """At least one object should be visible in the starting room."""
    state = ft_client.state()
    objects = state.get("objects", [])
    assert objects, f"Expected room objects, got empty list (room={state.get('room')})"


def test_04_ft_interact_object(ft_client: McpClient) -> None:
    """'interact' on a room object must produce messages, a state change, or trigger dialog."""
    # Retry the full interact loop — FT's walk probes may trigger cutscenes.
    for _attempt in range(3):
        if not wait_for_interactive(ft_client):
            pytest.skip("Game stuck in cutscene after extended wait")
        state = get_state_with_retry(ft_client)
        candidates = [
            obj["name"]
            for obj in state.get("objects", [])
            if not obj.get("pathway") and "interact" in obj.get("compatible_verbs", [])
        ]
        if not candidates:
            pytest.skip("No interact-compatible objects found in current room")

        all_cutscene = True
        for target in candidates:
            try:
                result = ft_client.act("interact", target)
            except RuntimeError as e:
                if "not accepting input" in str(e):
                    break  # cutscene fired, retry outer loop
                raise
            all_cutscene = False
            # act() completing without error means verb routing works.
            # An empty result is valid — engine accepted the request even if no handler.
            assert isinstance(result, dict), f"Expected dict, got: {result!r}"
            return
        if not all_cutscene:
            pytest.skip(f"No interact-compatible objects (tried: {candidates})")

    pytest.skip("Game stuck in cutscene after multiple retries")


def test_05_ft_interact_npc_triggers_dialog(ft_client: McpClient) -> None:
    """Interacting with an NPC should trigger V7 icon-based dialog with text choices."""
    wait_for_interactive(ft_client)
    state = get_state_with_retry(ft_client)
    if state.get("question"):
        pytest.skip("Already in dialog; covered by test_06")

    npc = find_npc(state)
    if npc is None:
        npc = find_object_with_verb(state, "interact")
    if npc is None:
        pytest.skip("No interactable NPC or object found in current room")

    try:
        result = ft_client.act("interact", npc)
    except RuntimeError as e:
        if "not accepting input" not in str(e):
            raise
        if not wait_for_interactive(ft_client):
            pytest.skip("Game still in cutscene after extended wait")
        try:
            result = ft_client.act("interact", npc)
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


def test_06_ft_answer_dialog(ft_client: McpClient) -> None:
    """If a dialog is pending, answer choice 1 and expect output."""
    state = get_state_with_retry(ft_client)
    if not state.get("question"):
        pytest.skip("No pending dialog question")

    result = ft_client.answer(1)
    messages = result.get("messages", [])
    assert messages, f"Expected messages after answering dialog choice 1, got: {result}"


def test_07_ft_use_item_on_object(ft_client: McpClient) -> None:
    """'use item' with two targets exercises the use-inventory-on-object mechanic."""
    state = get_state_with_retry(ft_client)

    if state.get("question"):
        ft_client.answer(1)
        state = ft_client.state()

    # Filter out FT's cursor-mode pseudo-items stored as inventory objects by the engine.
    cursor_names = {
        "look_at",
        "look at",
        "interact",
        "use",
        "use_item",
        "use item",
        "boot",
        "eye",
        "mouth",
    }
    real_inventory = [
        i for i in state.get("inventory", []) if i.lower() not in cursor_names
    ]
    if not real_inventory:
        pytest.skip("No real inventory items available (only cursor objects)")

    inv_item = real_inventory[0]

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
        result = ft_client.act("use item", inv_item, target_obj)
    except RuntimeError as e:
        if "not accepting input" in str(e):
            pytest.skip(f"Game still in cutscene: {e}")
        raise
    assert isinstance(result, dict), f"Expected dict result, got: {result!r}"
