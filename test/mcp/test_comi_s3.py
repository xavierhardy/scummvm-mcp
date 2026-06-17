"""Integration tests for Curse of Monkey Island demo, save slot 3 (SCUMM V8).

Save slot 3 starts with the ramrod and plastic hook in inventory, ready for
the inventory-combine and gaff-fishing flow. Runs against its own
fixture/instance so it can execute in parallel with test_comi.py
(pytest-xdist --dist=loadfile).

Each test sets up the state it needs (the gaff combine is idempotent: it is
only performed if the gaff is not already in inventory), so the tests do not
depend on each other's ordering.
"""

from utils import McpClient


def _ensure_gaff(client: McpClient) -> None:
    """Make sure the combined 'gaff' is in inventory, combining it if needed.

    Keeps the gaff-fishing test self-contained: it works whether or not the
    combine test has already run on the shared session fixture.
    """
    if "gaff" in client.state().get("inventory", []):
        return
    client.act("use", "ramrod", "plastic_hook")


def test_08a_comi_s3_use_combines_inventory_items(
    comi_s3_client: McpClient,
) -> None:
    """Save slot 3: 'use ramrod with plastic_hook' must combine them into a gaff.

    Both source items must be removed from inventory and the new 'gaff' must
    be added — all in a single act() invocation.
    """
    state = comi_s3_client.state()
    inv = state.get("inventory", [])
    assert "ramrod" in inv, f"setup: 'ramrod' should start in inventory, got: {inv}"
    assert "plastic_hook" in inv, f"setup: 'plastic_hook' should start in inventory, got: {inv}"
    assert "gaff" not in inv, f"setup: 'gaff' should not exist before combining, got: {inv}"

    result = comi_s3_client.act("use", "ramrod", "plastic_hook")
    assert sorted(result.get("inventory_added", [])) == ["gaff"], (
        f"combining ramrod+plastic_hook should add exactly 'gaff', "
        f"got inventory_added={result.get('inventory_added')}"
    )
    assert sorted(result.get("inventory_removed", [])) == ["plastic_hook", "ramrod"], (
        f"combining should consume both source items, "
        f"got inventory_removed={result.get('inventory_removed')}"
    )

    after = comi_s3_client.state().get("inventory", [])
    assert "gaff" in after, f"'gaff' should be in inventory after combining, got: {after}"
    assert "ramrod" not in after, f"'ramrod' should be consumed by the combine, got: {after}"
    assert "plastic_hook" not in after, f"'plastic_hook' should be consumed by the combine, got: {after}"


def test_08b_comi_s3_use_gaff_on_debris(comi_s3_client: McpClient) -> None:
    """Save slot 3: 'use gaff with debris' fishes a cutlass and skeleton_arm out
    of the water and removes the debris from the room.

    Self-contained: combines the gaff first if it is not already in inventory,
    so it does not depend on test_08a running first.
    """
    _ensure_gaff(comi_s3_client)

    state = comi_s3_client.state()
    inv = state.get("inventory", [])
    room_names = {obj["name"] for obj in state.get("objects", [])}
    assert "gaff" in inv, f"setup: 'gaff' should be in inventory before fishing, got: {inv}"
    assert "debris" in room_names, f"setup: 'debris' should be in the room, got: {sorted(room_names)}"

    result = comi_s3_client.act("use", "gaff", "debris")
    assert sorted(result.get("inventory_added", [])) == ["cutlass", "skeleton_arm"], (
        f"fishing the debris should add 'cutlass' and 'skeleton_arm', "
        f"got inventory_added={result.get('inventory_added')}"
    )

    after = comi_s3_client.state()
    inv = set(after.get("inventory", []))
    assert {"cutlass", "skeleton_arm", "gaff"}.issubset(inv), (
        f"after fishing, inventory should hold cutlass, skeleton_arm and gaff, got: {sorted(inv)}"
    )
    # Debris is consumed by the action and disappears from the room.
    after_names = {obj["name"] for obj in after.get("objects", [])}
    assert "debris" not in after_names, (
        f"'debris' should be removed from the room after fishing, got: {sorted(after_names)}"
    )
