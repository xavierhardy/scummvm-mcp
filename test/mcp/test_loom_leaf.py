"""
Integration tests for the Loom segment of Passport to Adventure (SCUMM V3),
save slot 2 (pass.s02): the leaf-on-a-tree scene (room 36) with a left-side
pathway. Runs against its own fixture/instance so it can execute in parallel
with the distaff tests in test_loom.py (pytest-xdist --dist=loadgroup).
"""

from time import sleep

import pytest

from assertions import assert_messages_contain
from utils import (
    McpClient,
    find_id,
    get_state_with_retry,
    object_names,
    require_interactive,
    skip_unless,
)


def _drive_pathway(
    client: McpClient, pathway: str, initial_room: int, attempts: int = 6
) -> int | None:
    """Interact with *pathway* until the room changes; return the new room or None.

    The pathway requires multiple steps because Bobbin's walk is interrupted at
    intermediate stand points, so retry up to *attempts* times.
    """
    for _ in range(attempts):
        cur = get_state_with_retry(client)
        if cur["room"]["id"] != initial_room:
            return cur["room"]["id"]
        try:
            result = client.act("interact", pathway)
        except RuntimeError:
            sleep(1)
            continue
        if result.get("room_changed"):
            return result["room_changed"]
        sleep(1)
    cur = get_state_with_retry(client)
    if cur["room"]["id"] != initial_room:
        return cur["room"]["id"]
    return None


# ---------------------------------------------------------------------------
# Tests for save slot 2: leaf-on-a-tree scene with a left-side pathway.
# ---------------------------------------------------------------------------


def test_08_loom_leaf_fall(loom_leaf_client: McpClient) -> None:
    """Interacting with the `leaf` in room 36 makes Bobbin say his line
    and the leaf disappears from the room (state change).

    Single-cursor model: MCP "interact" issues the click + queued second
    click (Loom double-click) on the leaf's bbox center. The engine then
    runs Bobbin's walk-arrival script, which prints the actor line and
    removes the leaf from the room.
    """
    require_interactive(loom_leaf_client, "Save did not reach interactive state")

    state = get_state_with_retry(loom_leaf_client)
    room = state["room"]
    assert room["id"] == 36, f"Expected room 36, got {room}"
    leaf = find_id(state, "leaf")
    assert leaf is not None, "Expected `leaf` object in room 36"

    notes, messages, result = loom_leaf_client.call_capturing(
        "act", {"verb": "interact", "target1": leaf}
    )
    assert_messages_contain(messages, "Last leaf of the year.")

    # Wait for the falling animation to complete and the leaf to be removed.
    sleep(2)
    names_after = sorted(object_names(get_state_with_retry(loom_leaf_client)))
    assert "leaf" not in names_after, f"leaf should have fallen, got: {names_after}"


def test_09_loom_pathway_named(loom_leaf_client: McpClient) -> None:
    """Loom/PASS exclusive: pathway objects with no OBNA name are renamed
    `pathway_<id>` in the MCP state, so the agent can target them by name.
    """
    state = get_state_with_retry(loom_leaf_client)
    sorted_names = sorted(object_names(state))
    pathway_id = find_id(state, "pathway_460")
    assert "pathway_460" in sorted_names, f"pathway_460 missing: {sorted_names}"
    assert pathway_id == 460, f"`pathway_460` should map to id 460, got: {pathway_id}"


@pytest.mark.slow
def test_10_loom_pathway_room_change(loom_leaf_client: McpClient) -> None:
    """Repeated interacts with `pathway_460` walk Bobbin all the way left
    and trigger a room change from room 36 to room 39.
    """
    require_interactive(loom_leaf_client, "Save did not reach interactive state")

    state = get_state_with_retry(loom_leaf_client)
    skip_unless(
        "pathway_460" in object_names(state), "pathway_460 not present in current room"
    )

    initial_room = state["room"]["id"]
    changed_to = _drive_pathway(loom_leaf_client, "pathway_460", initial_room)
    assert changed_to == 39, f"expected room 39, got {changed_to} from {initial_room}"
