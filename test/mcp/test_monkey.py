"""
Integration test for Monkey Island 1 EGA demo.

Walkthrough: Troll -> door sequence -> pick up items -> use item with pot ->
give the breath mint to the prisoner.

The demo is a deep sequential walkthrough where later steps need rooms and items
established earlier. To stay per-test independent under --dist=loadgroup:
  * the troll-clearing tests (room 55) set up their own state from save slot 1;
  * the deeper steps load checkpoint save states captured by make_save_states.py
    (the SCUMM bar / kitchen / prison fixtures), so each runs on its own fresh
    instance without replaying the whole walkthrough.
"""

from time import sleep

from assertions import assert_inventory_contains, assert_inventory_does_not_contain
from utils import McpClient


def _talk_to_troll(client: McpClient) -> dict:
    """Walk to the troll and open his dialog; return the talk result.

    Self-contained setup so the dialog tests run on their own fresh instance:
    walking near the troll first reproduces the same talk result regardless of
    Guybrush's start position.
    """
    client.walk(120, 132)
    return client.act("talk_to", "Troll")


def test_01_monkey_initial_state(monkey_client: McpClient) -> None:
    """Verify initial game state."""
    state = monkey_client.state()
    assert state.get("room") is not None, f"expected a room in state, got: {state}"
    assert (
        state["room"]["id"] == 55
    ), f"expected the troll clearing (room 55), got: {state['room']}"
    assert isinstance(state.get("objects"), list), "expected 'objects' to be a list"
    assert len(state.get("objects", [])) > 0, "expected at least one object in room 55"
    assert (
        len(state.get("inventory", [])) == 0
    ), f"expected an empty starting inventory, got: {state.get('inventory')}"
    assert state.get("position") == {
        "y": 132,
        "x": 235,
    }, f"unexpected start position: {state.get('position')}"
    assert len(state.get("messages", [])) == 0, "expected no pending messages at start"


def test_02_monkey_walk_to_troll(monkey_client: McpClient) -> None:
    """Walk to Troll."""
    state = monkey_client.state()
    assert state["room"]["id"] == 55, f"expected room 55, got: {state['room']}"

    # Walk towards the troll side of the bridge first so game scripts position
    # the troll actor and make the troll room object selectable.
    result = monkey_client.walk(120, 132)
    assert (
        "x" in result["position"] and "y" in result["position"]
    ), f"walk should report a position, got: {result}"
    assert result["messages"] == [
        {"actor": "troll", "text": "None shall pass!"}
    ], f"expected the troll's challenge on approach, got: {result.get('messages')}"

    monkey_client.act("walk_to", "Troll")

    # Guybrush should be on the troll's side of the bridge (x < 200).
    state = monkey_client.state()
    assert (
        state["position"]["x"] < 200
    ), f"Expected Guybrush near troll (x<200), got {state['position']}"


def test_03_monkey_talk_to_troll(monkey_client: McpClient) -> None:
    """Talk to Troll to trigger dialog."""
    state = monkey_client.state()
    assert state["room"]["id"] == 55, f"expected room 55, got: {state['room']}"

    result = _talk_to_troll(monkey_client)
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
    assert result == expected, f"unexpected troll dialog: {result}"

    state = monkey_client.state()
    assert (
        state.get("question") is not None
    ), "Expected a dialog question after talking to Troll"


def test_04_monkey_answer_troll_dialog(monkey_client: McpClient) -> None:
    """Answer dialog choice 3."""
    state = monkey_client.state()
    assert state["room"]["id"] == 55, f"expected room 55, got: {state['room']}"

    # Open the troll dialog first so this test stands alone.
    _talk_to_troll(monkey_client)

    result = monkey_client.answer(3)
    assert result == {
        "messages": [
            {"text": "Pretty please?", "actor": "guybrush"},
            {
                "text": "Not those magic words, you pedantic putrefied pinhead, the MAGIC words! --sigh--",
                "actor": "troll",
            },
        ]
    }, f"unexpected answer(3) result: {result}"


def test_05_monkey_walk_to_door_1(monkey_client: McpClient) -> None:
    """Walk to door (first time)."""
    state = monkey_client.state()
    assert state["room"]["id"] == 55, f"expected room 55, got: {state['room']}"

    result = monkey_client.act("walk_to", "door")
    assert result == {
        "position": {"y": 132, "x": 361}
    }, f"unexpected walk-to-door position: {result}"


def test_06_monkey_open_door_1(monkey_client: McpClient) -> None:
    """Open door (first time)."""
    state = monkey_client.state()
    assert state["room"]["id"] == 55, f"expected room 55, got: {state['room']}"

    result = monkey_client.act("open", "door")
    assert (
        len(result["objects_changed"]) == 1
    ), f"opening the door should change exactly one object, got: {result.get('objects_changed')}"
    assert (
        result["objects_changed"][0]["name"] == "door"
    ), f"expected the 'door' to change state, got: {result.get('objects_changed')}"


def test_07_monkey_walk_to_door_2(monkey_client: McpClient) -> None:
    """Walk through the door into the SCUMM bar (room 55 -> 52)."""
    state = monkey_client.state()
    assert state["room"]["id"] == 55, f"expected room 55, got: {state['room']}"

    # Open the door first so this test is self-contained.
    monkey_client.act("open", "door")
    result = monkey_client.act("walk_to", "door")
    assert (
        result.get("room_changed") == 52
    ), f"walking through the open door should enter the SCUMM bar (room 52), got: {result}"


def test_08_monkey_pickup_bowl(monkey_bar_client: McpClient) -> None:
    """Pick up bowl o' mints (SCUMM bar checkpoint)."""
    state = monkey_bar_client.state()
    assert (
        state["room"]["id"] == 52
    ), f"expected the SCUMM bar (room 52), got: {state['room']}"

    result = monkey_bar_client.act("pick_up", "bowl o' mints")
    assert_inventory_contains(result, "breath_mint")
    assert len(result["messages"]) > 0, f"expected a pickup message, got: {result}"


def test_09_monkey_open_door_2(monkey_bar_client: McpClient) -> None:
    """Open the far door behind the bar (SCUMM bar checkpoint)."""
    state = monkey_bar_client.state()
    assert (
        state["room"]["id"] == 52
    ), f"expected the SCUMM bar (room 52), got: {state['room']}"

    result = monkey_bar_client.act("open", 354)
    assert (
        result["objects_changed"][0]["name"] == "door"
    ), f"expected the far door to change state, got: {result.get('objects_changed')}"


def test_10_monkey_walk_to_door_3(monkey_bar_client: McpClient) -> None:
    """Walk through the far door into the kitchen (room 52 -> 51)."""
    state = monkey_bar_client.state()
    assert (
        state["room"]["id"] == 52
    ), f"expected the SCUMM bar (room 52), got: {state['room']}"

    monkey_bar_client.act("open", 354)  # ensure the far door is open
    result = monkey_bar_client.act("walk_to", 354)
    assert (
        result["room_changed"] == 51
    ), f"walking through the far door should enter the kitchen (room 51), got: {result}"


def test_11_monkey_pickup_meat(monkey_kitchen_client: McpClient) -> None:
    """Pick up hunk o' meat (kitchen checkpoint)."""
    state = monkey_kitchen_client.state()
    assert (
        state["room"]["id"] == 51
    ), f"expected the kitchen (room 51), got: {state['room']}"

    result = monkey_kitchen_client.act("pick_up", "hunk_o'_meat@@@@@@@")
    assert_inventory_contains(result, "hunk_o'_meat")


def test_12_monkey_use_meat_with_pot_o_soup(monkey_kitchen_client: McpClient) -> None:
    """Use hunk o' meat with pot o' soup (kitchen checkpoint)."""
    state = monkey_kitchen_client.state()
    assert (
        state["room"]["id"] == 51
    ), f"expected the kitchen (room 51), got: {state['room']}"

    # Pick the meat up first so this test is self-contained on the checkpoint.
    monkey_kitchen_client.act("pick_up", "hunk_o'_meat@@@@@@@")
    result = monkey_kitchen_client.act(
        "use", "hunk o' meat@@@@@@@", "pot_o'_soup@@@@@@"
    )
    assert result["messages"] == [
        {"text": "This will take a while to cook.", "actor": "guybrush"}
    ], f"unexpected use-meat-on-pot result: {result.get('messages')}"
    assert_inventory_does_not_contain(result, "hunk_o'_meat")


def test_13_monkey_give_breath_mint_to_prisoner(
    monkey_prison_client: McpClient,
) -> None:
    """Give the breath mint to the prisoner (prison checkpoint, mint in hand)."""
    state = monkey_prison_client.state()
    assert (
        state["room"]["id"] == 54
    ), f"expected the prison (room 54), got: {state['room']}"

    result = monkey_prison_client.act("give", "breath_mint", "prisoner")
    assert result["question"] == {
        "choices": [
            {"id": 1, "label": "I wanted to say goodbye."},
            {"id": 2, "label": "Won't you help me now?"},
            {"id": 3, "label": "Do you know anything about a magic phrase?"},
        ]
    }, f"unexpected prisoner dialog: {result.get('question')}"
    assert any(
        msg["text"] == "Don't mention it." for msg in result["messages"]
    ), f"expected the prisoner's 'Don't mention it.' line, got: {result.get('messages')}"
    assert result["inventory_removed"] == [
        "breath_mint"
    ], f"giving the mint should remove it from inventory, got: {result.get('inventory_removed')}"
    assert (
        "x" in result["position"] and "y" in result["position"]
    ), f"expected a position in the result, got: {result}"
