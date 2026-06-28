"""Integration tests for Curse of Monkey Island demo, save slot 3 (SCUMM V8).

Save slot 3 starts with the ramrod and plastic hook in inventory, ready for
the inventory-combine and gaff-fishing flow. Runs against its own
fixture/instance so it can execute in parallel with test_comi.py
(pytest-xdist --dist=loadgroup).

Each test sets up the state it needs (the gaff combine is idempotent: it is
only performed if the gaff is not already in inventory), so the tests do not
depend on each other's ordering.
"""

from utils import McpClient, bind_verb, object_names


def _ensure_gaff(client: McpClient) -> None:
    """Make sure the combined 'gaff' is in inventory, combining it if needed.

    Keeps the gaff-fishing test self-contained: it works whether or not the
    combine test has already run on the shared session fixture.
    """
    if "gaff" not in client.state().get("inventory", []):
        client.act("use", "ramrod", "plastic_hook")


def test_08a_comi_s3_use_combines_inventory_items(comi_s3_client: McpClient) -> None:
    """Save slot 3: 'use ramrod with plastic_hook' must combine them into a gaff.

    Both source items must be removed from inventory and the new 'gaff' must
    be added — all in a single act() invocation.
    """
    use = bind_verb(comi_s3_client, "use")
    inv = comi_s3_client.state().get("inventory", [])
    assert "ramrod" in inv, f"setup: 'ramrod' should start in inventory: {inv}"
    assert "plastic_hook" in inv, f"setup: 'plastic_hook' should start in: {inv}"
    assert "gaff" not in inv, f"setup: 'gaff' present before combine: {inv}"

    result = use("ramrod", "plastic_hook")
    added = sorted(result.get("inventory_added", []))
    removed = sorted(result.get("inventory_removed", []))
    assert added == ["gaff"], f"combining should add exactly 'gaff', got {added}"
    assert removed == ["plastic_hook", "ramrod"], f"should consume both: {removed}"

    after = comi_s3_client.state().get("inventory", [])
    assert "gaff" in after, f"'gaff' missing after combine: {after}"
    assert "ramrod" not in after, f"'ramrod' not consumed: {after}"
    assert "plastic_hook" not in after, f"'plastic_hook' not consumed: {after}"


def test_08b_comi_s3_use_gaff_on_debris(comi_s3_client: McpClient) -> None:
    """Save slot 3: 'use gaff with debris' fishes a cutlass and skeleton_arm out
    of the water and removes the debris from the room.

    Self-contained: combines the gaff first if it is not already in inventory,
    so it does not depend on test_08a running first.
    """
    _ensure_gaff(comi_s3_client)
    use = bind_verb(comi_s3_client, "use")

    state = comi_s3_client.state()
    inv = state.get("inventory", [])
    room_list = sorted(object_names(state))
    assert "gaff" in inv, f"setup: 'gaff' not in inventory before fishing: {inv}"
    assert "debris" in room_list, f"setup: 'debris' not in the room: {room_list}"

    result = use("gaff", "debris")
    added = sorted(result.get("inventory_added", []))
    assert added == ["cutlass", "skeleton_arm"], f"fishing should add both, got {added}"

    after = comi_s3_client.state()
    inv = set(after.get("inventory", []))
    inv_list = sorted(inv)
    assert {"cutlass", "skeleton_arm", "gaff"}.issubset(inv), f"got {inv_list}"
    # Debris is consumed by the action and disappears from the room.
    after_list = sorted(object_names(after))
    assert "debris" not in after_list, f"'debris' not removed: {after_list}"
