"""
Integration test for Maniac Mansion C64 demo.
Walkthrough: mailbox -> flags -> bushes -> front door -> key -> use key.
"""

from utils import McpClient


def assert_inventory_contains(result: dict, item: str) -> None:
    """Assert inventory_added contains the item (case-insensitive)."""
    added = result.get("inventory_added", [])
    assert any(i.lower() == item.lower() for i in added), (
        f"Expected '{item}' in inventory_added, got {added}"
    )


def test_01_maniac_initial_state(maniac_client: McpClient) -> None:
    """Verify initial game state."""
    state = maniac_client.state()
    assert "room" in state
    assert isinstance(state.get("room"), dict)
    assert state["room"]["id"] == 1
    assert state.get("objects") is not None


def test_02_maniac_open_mailbox(maniac_client: McpClient) -> None:
    """Open mailbox."""
    result = maniac_client.act("open", "mailbox")
    assert result["objects_changed"][0]["name"] == "mailbox"
    assert "x" in result["position"]
    assert "y" in result["position"]


def test_03_maniac_pull_flag(maniac_client: McpClient) -> None:
    """Pull flag."""
    result = maniac_client.act("pull", "flag")
    assert result["objects_changed"][0]["name"] == "flag"


def test_04_maniac_pull_bushes(maniac_client: McpClient) -> None:
    """Pull bushes."""
    result = maniac_client.act("pull", "bushes")
    assert result["objects_changed"][0]["name"] == "bushes"
    assert "x" in result["position"]
    assert "y" in result["position"]


def test_05_maniac_walk_to_front_door(maniac_client: McpClient) -> None:
    """Walk to front door."""
    result = maniac_client.act("walk_to", "front_door")
    assert "x" in result["position"]
    assert "y" in result["position"]


def test_06_maniac_pull_door_mat(maniac_client: McpClient) -> None:
    """Pull door mat."""
    result = maniac_client.act("pull", "door mat")
    assert result["objects_changed"][0]["name"] == "door mat"
    assert "x" in result["position"]
    assert "y" in result["position"]


def test_07_maniac_pickup_key(maniac_client: McpClient) -> None:
    """Pick up key."""
    result = maniac_client.act("pick_up", "key")
    assert_inventory_contains(result, "key")


def test_08_maniac_use_key_on_door(maniac_client: McpClient) -> None:
    """Unlock front door with key."""
    result = maniac_client.act("use", "key", "front_door")
    assert result["objects_changed"][0]["name"] == "front_door"


def test_09_maniac_walk_through_front_door(maniac_client: McpClient) -> None:
    """Walk to front door (should enter new room)."""
    result = maniac_client.act("walk_to", "front_door")
    assert result.get("room_changed")
