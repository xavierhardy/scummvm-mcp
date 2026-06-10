"""Integration tests for Curse of Monkey Island demo, save slot 3 (SCUMM V8).

Save slot 3 starts with the ramrod and plastic hook in inventory, ready for
the inventory-combine and gaff-fishing flow. Runs against its own
fixture/instance so it can execute in parallel with test_comi.py
(pytest-xdist --dist=loadfile).
"""

from utils import McpClient


def test_08a_comi_s3_use_combines_inventory_items(
    comi_s3_client: McpClient,
) -> None:
    """Save slot 3: 'use ramrod with plastic_hook' must combine them into a gaff.

    Both source items must be removed from inventory and the new 'gaff' must
    be added — all in a single act() invocation.
    """
    state = comi_s3_client.state()
    assert "ramrod" in state["inventory"]
    assert "plastic_hook" in state["inventory"]
    assert "gaff" not in state["inventory"]

    result = comi_s3_client.act("use", "ramrod", "plastic_hook")
    assert sorted(result.get("inventory_added", [])) == ["gaff"]
    assert sorted(result.get("inventory_removed", [])) == sorted(
        ["plastic_hook", "ramrod"]
    )

    after = comi_s3_client.state()
    assert "gaff" in after["inventory"]
    assert "ramrod" not in after["inventory"]
    assert "plastic_hook" not in after["inventory"]


def test_08b_comi_s3_use_gaff_on_debris(comi_s3_client: McpClient) -> None:
    """Save slot 3 (after combine): 'use gaff with debris' fishes a cutlass and
    skeleton_arm out of the water and removes the debris from the room.

    Depends on test_08a having combined the gaff (session-scoped fixture).
    """
    state = comi_s3_client.state()
    # Sanity: the combined gaff is in inventory and the debris is in the room.
    assert "gaff" in state["inventory"]
    assert "debris" in {obj["name"] for obj in state["objects"]}

    result = comi_s3_client.act("use", "gaff", "debris")
    assert sorted(result.get("inventory_added", [])) == sorted(
        ["cutlass", "skeleton_arm"]
    )

    after = comi_s3_client.state()
    inv = set(after["inventory"])
    assert {"cutlass", "skeleton_arm", "gaff"}.issubset(inv)
    # Debris is consumed by the action and disappears from the room.
    assert "debris" not in {obj["name"] for obj in after["objects"]}
