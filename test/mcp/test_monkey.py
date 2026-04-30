"""
Integration test for Monkey Island 1 EGA demo.
Walkthrough: Troll -> door sequence -> pick up items -> use item with pot.
"""

from time import sleep

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
    assert state["room"]["id"] == 55
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
    assert state["room"]["id"] == 55

    # Walk towards the troll side of the bridge first so game scripts position
    # the troll actor and make the troll room object selectable.
    result = monkey_client.walk(120, 132)
    assert "x" in result["position"]
    assert "y" in result["position"]
    assert result["messages"] == [{"actor": "troll", "text": "None shall pass!"}]

    monkey_client.act("walk_to", "Troll")

    # Guybrush should be on the troll's side of the bridge (x < 200).
    # walk_to may return {} if already at the troll's position, which is fine.
    state = monkey_client.state()
    assert state["position"]["x"] < 200, (
        f"Expected Guybrush near troll, got {state['position']}"
    )


def test_03_monkey_talk_to_troll(monkey_client: McpClient) -> None:
    """Talk to Troll to trigger dialog."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 55

    result = monkey_client.act("talk_to", "Troll")
    expected = {
        "question": {
            "choices": [
                {"id": 1, "label": "But I want to be a pirate!"},
                {"id": 2, "label": "Why not?"},
                {"id": 3, "label": "Pretty please?"},
            ]
        },
        "messages": [
            {"text": "Hi. I'm Guybrush Threepwood and--", "actor": "guybrush"},
            {
                "text": "I don't care who you are or what your business is, you snivelling slimy sliver of scumm! No one gets by me until they say the magic words.",
                "actor": "troll",
            },
        ],
    }

    assert result == expected

    # Dialog should present a question
    state = monkey_client.state()

    assert state.get("question") is not None, (
        "Expected dialog question after talking to Troll"
    )


def test_04_monkey_answer_troll_dialog(monkey_client: McpClient) -> None:
    """Answer dialog choice 3."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 55

    result = monkey_client.answer(3)
    assert result == {
        "messages": [
            {"text": "Pretty please?", "actor": "guybrush"},
            {
                "text": "Not those magic words, you pedantic putrefied pinhead, the MAGIC words! --sigh--",
                "actor": "troll",
            },
        ]
    }


def test_05_monkey_walk_to_door_1(monkey_client: McpClient) -> None:
    """Walk to door (first time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 55

    result = monkey_client.act("walk_to", "door")
    assert result == {"position": {"y": 132, "x": 361}}


def test_06_monkey_open_door_1(monkey_client: McpClient) -> None:
    """Open door (first time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 55

    result = monkey_client.act("open", "door")

    # Should produce some state change
    assert len(result["objects_changed"]) == 1
    assert result["objects_changed"][0]["name"] == "door"


def test_07_monkey_walk_to_door_2(monkey_client: McpClient) -> None:
    """Walk to door (second time) - enter new room."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 55

    result = monkey_client.act("walk_to", "door")
    assert result["room_changed"] == 52


def test_08_monkey_pickup_bowl(monkey_client: McpClient) -> None:
    """Pick up bowl o' mints."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 52

    result = monkey_client.act("pick_up", "bowl o' mints")
    assert_inventory_contains(result, "breath mint")
    assert len(result["messages"]) > 0


def test_09_monkey_open_door_2(monkey_client: McpClient) -> None:
    """Open door (second time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 52

    result = monkey_client.act("open", 354)
    assert result["objects_changed"][0]["name"] == "door"


def test_10_monkey_walk_to_door_3(monkey_client: McpClient) -> None:
    """Walk to door (third time)."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 52

    result = monkey_client.act("walk_to", 354)
    assert result["room_changed"] == 51


def test_11_monkey_pickup_meat(monkey_client: McpClient) -> None:
    """Pick up hunk o' meat."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 51

    result = monkey_client.act("pick_up", "hunk_o'_meat@@@@@@@")
    assert_inventory_contains(result, "hunk o' meat")


def test_12_monkey_use_meat_with_pot_o_soup(monkey_client: McpClient) -> None:
    """Use hunk o' meat with pot o' soup."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 51

    result = monkey_client.act("use", "hunk o' meat@@@@@@@", "pot_o'_soup@@@@@@")
    assert result["messages"] == [
        {"text": "This will take a while to cook.", "actor": "guybrush"}
    ]
    assert_inventory_does_not_contain(result, "hunk o' meat")


def test_13_monkey_give_breath_mint_to_prisoner(monkey_client: McpClient) -> None:
    """Walk to prison and give breath mint to prisoner."""
    state = monkey_client.state()
    assert "room" in state
    assert state["room"]["id"] == 51

    result = monkey_client.act("walk", 305)
    assert result["room_changed"] == 52

    result = monkey_client.act("walk", 353)
    assert result["room_changed"] == 55

    result = monkey_client.act("walk", "archway")
    assert result["room_changed"] == 57

    retries = 4
    while result["room_changed"] != 54 and retries > 0:
        sleep(0.5)
        try:
            result = monkey_client.act("walk", "jail_entrance")
        except RuntimeError:
            continue

        retries -= 1
    assert result["room_changed"] == 54

    result = monkey_client.act("give", "breath_mint", "prisoner")
    assert result["question"] == {
        "choices": [
            {"id": 1, "label": "I wanted to say goodbye."},
            {"id": 2, "label": "Won't you help me now?"},
            {"id": 3, "label": "Do you know anything about a magic phrase?"},
        ]
    }
    assert any(
        msg["text"] == "Ooooh! Grog-o-mint! How refreshing! Thanks."
        for msg in result["messages"]
    )

    assert result["inventory_removed"] == ["breath mint"]
    assert "x" in result["position"]
    assert "y" in result["position"]
