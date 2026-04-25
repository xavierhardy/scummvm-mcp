"""
Integration test for Monkey Island 1 EGA demo.
Walkthrough: Troll -> door sequence -> pick up items -> use item with pot.
"""

from utils import McpClient


def assert_inventory_does_not_contain(result: dict, item: str) -> None:
    """Assert inventory_removed contains the item (case-insensitive)."""
    removed = result.get("inventory_removed", [])
    assert any(i.lower() == item.lower() for i in removed), (
        f"Expected '{item}' in inventory_removed, got {removed}"
    )


def assert_inventory_contains(result: dict, item: str) -> None:
    """Assert inventory_added contains the item (case-insensitive)."""
    added = result.get("inventory_added", [])
    assert any(i.lower() == item.lower() for i in added), (
        f"Expected '{item}' in inventory_added, got {added}"
    )


def assert_messages_produced(result: dict) -> None:
    """Assert messages list is non-empty."""
    messages = result.get("messages", [])
    assert messages, "Expected messages to be produced"


def test_01_monkey_initial_state(monkey_client: McpClient) -> None:
    """Verify initial game state."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 55
    assert state.get("room") is not None
    assert state.get("objects") is not None
    assert isinstance(state.get("objects"), list)
    assert len(state.get("objects", [])) > 0
    assert len(state.get("inventory", [])) == 0
    assert "position" in state
    assert state["position"] == {"y": 132, "x": 235}
    assert len(state.get("messages", [])) == 0


def test_02_monkey_walk_to_troll(monkey_client: McpClient) -> None:
    """Walk to Troll."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 55

    result = monkey_client.act("walk_to", "Troll")

    assert result.get("position") or result.get("room_changed"), (
        "Expected position or room_changed after walking"
    )


def test_03_monkey_talk_to_troll(monkey_client: McpClient) -> None:
    """Talk to Troll to trigger dialog."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 55

    result = monkey_client.act("talk_to", "Troll")

    # Dialog should present a question
    state = monkey_client.state()
    assert state.get("question") is not None, (
        "Expected dialog question after talking to Troll"
    )


def test_04_monkey_answer_troll_dialog(monkey_client: McpClient) -> None:
    """Answer dialog choice 3."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 55

    result = monkey_client.answer(3)
    assert_messages_produced(result)


def test_05_monkey_walk_to_door_1(monkey_client: McpClient) -> None:
    """Walk to door (first time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 55

    result = monkey_client.act("walk_to", "door")
    assert result.get("position") or result.get("room_changed")


def test_06_monkey_open_door_1(monkey_client: McpClient) -> None:
    """Open door (first time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 55

    result = monkey_client.act("open", "door")

    # Should produce some state change
    assert len(result["objects_changed"]) == 1
    assert result["objects_changed"][0]["name"] == "door"


def test_07_monkey_walk_to_door_2(monkey_client: McpClient) -> None:
    """Walk to door (second time) - enter new room."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 55

    result = monkey_client.act("walk_to", "door")
    assert result["room_changed"] == 52


def test_08_monkey_pickup_bowl(monkey_client: McpClient) -> None:
    """Pick up bowl o' mints."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 52

    result = monkey_client.act("pick_up", "bowl o' mints")
    assert_inventory_contains(result, "breath mint")
    assert len(result["messages"]) > 0


def test_09_monkey_open_door_2(monkey_client: McpClient) -> None:
    """Open door (second time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 52

    result = monkey_client.act("open", 354)
    assert (
        result.get("messages") or result.get("position") or result.get("room_changed")
    )


def test_10_monkey_walk_to_door_3(monkey_client: McpClient) -> None:
    """Walk to door (third time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 52

    result = monkey_client.act("walk_to", 354)
    assert result["room_changed"] == 51


def test_11_monkey_pickup_meat(monkey_client: McpClient) -> None:
    """Pick up hunk o' meat."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 51

    result = monkey_client.act("pick_up", "hunk_o'_meat@@@@@@@")
    assert_inventory_contains(result, "hunk o' meat@@@@@@@")


def test_12_monkey_use_meat_with_pot_o_soup(monkey_client: McpClient) -> None:
    """Use hunk o' meat with pot o' soup."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"] == 51

    result = monkey_client.act("use", "hunk o' meat@@@@@@@", "pot_o'_soup@@@@@@")
    assert result["messages"] == [
        {"text": "This will take a while to cook.", "actor": "guybrush"}
    ]
    assert_inventory_does_not_contain(result, "hunk o' meat@@@@@@@")
