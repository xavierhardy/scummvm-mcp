"""
Integration tests for the Loom segment of Passport to Adventure (SCUMM V3),
save slot 2 (pass.s02): the leaf-on-a-tree scene (room 36) with a left-side
pathway. Runs against its own fixture/instance so it can execute in parallel
with the distaff tests in test_loom.py (pytest-xdist --dist=loadfile).
"""

import pytest
from time import sleep

from utils import (
    wait_for_interactive,
    get_state_with_retry,
)


def _find_id(state: dict, name: str) -> int | None:
    for obj in state.get("objects", []):
        if obj["name"] == name:
            return obj["id"]
    return None


# ---------------------------------------------------------------------------
# Tests for save slot 2: leaf-on-a-tree scene with a left-side pathway.
# ---------------------------------------------------------------------------


def test_08_loom_leaf_fall(loom_leaf_client) -> None:
    """Interacting with the `leaf` in room 36 makes Bobbin say his line
    and the leaf disappears from the room (state change).

    Single-cursor model: MCP "interact" issues the click + queued second
    click (Loom double-click) on the leaf's bbox center. The engine then
    runs Bobbin's walk-arrival script, which prints the actor line and
    removes the leaf from the room.
    """
    if not wait_for_interactive(loom_leaf_client):
        pytest.skip("Save did not reach interactive state")

    state = get_state_with_retry(loom_leaf_client)
    assert state["room"]["id"] == 36, f"Expected room 36, got {state['room']}"
    leaf = _find_id(state, "leaf")
    assert leaf is not None, "Expected `leaf` object in room 36"

    notes, messages, result = loom_leaf_client.call_capturing(
        "act", {"verb": "interact", "target1": leaf}
    )

    texts = [m.get("text") for m in messages]
    assert "Last leaf of the year." in texts, (
        f"Expected Bobbin's leaf line, got messages: {messages}"
    )

    # Wait for the falling animation to complete and the leaf to be removed.
    sleep(2)
    new_state = get_state_with_retry(loom_leaf_client)
    names_after = [o["name"] for o in new_state.get("objects", [])]
    assert "leaf" not in names_after, (
        f"leaf should have fallen and left the room object list, got: {names_after}"
    )


def test_09_loom_pathway_named(loom_leaf_client) -> None:
    """Loom/PASS exclusive: pathway objects with no OBNA name are renamed
    `pathway_<id>` in the MCP state, so the agent can target them by name.
    """
    state = get_state_with_retry(loom_leaf_client)
    names = {o["name"]: o["id"] for o in state.get("objects", [])}
    assert "pathway_460" in names, (
        f"Expected `pathway_460` (renamed from unnamed obj 460) in room 36, got: {list(names)}"
    )
    assert names["pathway_460"] == 460, (
        f"`pathway_460` should map to object id 460, got: {names['pathway_460']}"
    )


def test_10_loom_pathway_room_change(loom_leaf_client) -> None:
    """Repeated interacts with `pathway_460` walk Bobbin all the way left
    and trigger a room change from room 36 to room 39. The pathway
    requires multiple steps because Bobbin's walk is interrupted at
    intermediate stand points.
    """
    if not wait_for_interactive(loom_leaf_client):
        pytest.skip("Save did not reach interactive state")

    state = get_state_with_retry(loom_leaf_client)
    if not any(o["name"] == "pathway_460" for o in state.get("objects", [])):
        pytest.skip("pathway_460 not present in current room")

    initial_room = state["room"]["id"]
    changed_to: int | None = None
    for _ in range(6):
        cur = get_state_with_retry(loom_leaf_client)
        if cur["room"]["id"] != initial_room:
            changed_to = cur["room"]["id"]
            break
        try:
            result = loom_leaf_client.act("interact", "pathway_460")
        except RuntimeError:
            sleep(1)
            continue
        if result.get("room_changed"):
            changed_to = result["room_changed"]
            break
        sleep(1)

    if changed_to is None:
        cur = get_state_with_retry(loom_leaf_client)
        if cur["room"]["id"] != initial_room:
            changed_to = cur["room"]["id"]

    assert changed_to == 39, (
        f"Expected pathway to lead to room 39, got room {changed_to} from {initial_room}"
    )
