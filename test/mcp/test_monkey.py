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

from assertions import (
    assert_has_position,
    assert_inventory_contains,
    assert_inventory_does_not_contain,
    assert_message_present,
    assert_room,
)
from utils import McpClient, bind_verb, make_verbs


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
    assert_room(state, 55)
    objects = state.get("objects")
    assert isinstance(objects, list), f"expected 'objects' to be a list, got {objects}"
    assert len(objects) > 0, "expected at least one object in room 55"
    inventory = state.get("inventory", [])
    assert inventory == [], f"expected an empty starting inventory, got {inventory}"
    position = state.get("position")
    assert position == {"x": 235, "y": 132}, f"unexpected start position: {position}"
    messages = state.get("messages", [])
    assert messages == [], f"expected no pending messages at start, got {messages}"


def test_02_monkey_walk_to_troll(monkey_client: McpClient) -> None:
    """Walk to Troll."""
    assert_room(monkey_client.state(), 55)
    walk_to = bind_verb(monkey_client, "walk_to")

    # Walk towards the troll side of the bridge first so game scripts position
    # the troll actor and make the troll room object selectable.
    result = monkey_client.walk(120, 132)
    assert_has_position(result)
    messages = result["messages"]
    expected = [{"actor": "troll", "text": "None shall pass!"}]
    assert messages == expected, f"unexpected approach messages: {messages}"

    walk_to("Troll")

    # Guybrush should be on the troll's side of the bridge (x < 200).
    position = monkey_client.state()["position"]
    assert position["x"] < 200, f"expected Guybrush near troll (x<200), got {position}"


def test_03_monkey_talk_to_troll(monkey_client: McpClient) -> None:
    """Talk to Troll to trigger dialog."""
    assert_room(monkey_client.state(), 55)

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
                "text": (
                    "I don't care who you are or what your business is, you "
                    "snivelling slimy sliver of scumm! No one gets by me until "
                    "they say the magic words."
                ),
                "actor": "troll",
            },
        ],
    }
    assert result == expected, f"unexpected troll dialog: {result}"

    question = monkey_client.state().get("question")
    assert question is not None, "expected a dialog question after talking to Troll"


def test_04_monkey_answer_troll_dialog(monkey_client: McpClient) -> None:
    """Answer dialog choice 3."""
    assert_room(monkey_client.state(), 55)

    # Open the troll dialog first so this test stands alone.
    _talk_to_troll(monkey_client)

    result = monkey_client.answer(3)
    expected = {
        "messages": [
            {"text": "Pretty please?", "actor": "guybrush"},
            {
                "text": (
                    "Not those magic words, you pedantic putrefied pinhead, "
                    "the MAGIC words! --sigh--"
                ),
                "actor": "troll",
            },
        ]
    }
    assert result == expected, f"unexpected answer(3) result: {result}"


def test_05_monkey_walk_to_door_1(monkey_client: McpClient) -> None:
    """Walk to door (first time)."""
    assert_room(monkey_client.state(), 55)
    walk_to = bind_verb(monkey_client, "walk_to")

    result = walk_to("door")
    expected = {"position": {"x": 361, "y": 132}}
    assert result == expected, f"unexpected walk-to-door position: {result}"


def test_06_monkey_open_door_1(monkey_client: McpClient) -> None:
    """Open door (first time)."""
    assert_room(monkey_client.state(), 55)
    open_ = bind_verb(monkey_client, "open")

    result = open_("door")
    changed = result["objects_changed"]
    assert len(changed) == 1, f"door should change one object, got {changed}"
    assert changed[0]["name"] == "door", f"expected the 'door' to change, got {changed}"


def test_07_monkey_walk_to_door_2(monkey_client: McpClient) -> None:
    """Walk through the door into the SCUMM bar (room 55 -> 52)."""
    assert_room(monkey_client.state(), 55)
    open_, walk_to = make_verbs(monkey_client, "open", "walk_to")
    door = "door"

    # Open the door first so this test is self-contained.
    open_(door)
    result = walk_to(door)
    room_changed = result.get("room_changed")
    assert room_changed == 52, f"expected SCUMM bar (room 52), got {room_changed}"


def test_08_monkey_pickup_bowl(monkey_bar_client: McpClient) -> None:
    """Pick up bowl o' mints (SCUMM bar checkpoint)."""
    assert_room(monkey_bar_client.state(), 52)
    pick_up = bind_verb(monkey_bar_client, "pick_up")

    result = pick_up("bowl o' mints")
    assert_inventory_contains(result, "breath_mint")
    messages = result["messages"]
    assert messages, f"expected a pickup message, got {messages}"


def test_09_monkey_open_door_2(monkey_bar_client: McpClient) -> None:
    """Open the far door behind the bar (SCUMM bar checkpoint)."""
    assert_room(monkey_bar_client.state(), 52)
    open_ = bind_verb(monkey_bar_client, "open")

    result = open_(354)
    changed = result["objects_changed"]
    assert changed[0]["name"] == "door", f"far door should change, got {changed}"


def test_10_monkey_walk_to_door_3(monkey_bar_client: McpClient) -> None:
    """Walk through the far door into the kitchen (room 52 -> 51)."""
    assert_room(monkey_bar_client.state(), 52)
    open_, walk_to = make_verbs(monkey_bar_client, "open", "walk_to")
    door = 354

    open_(door)  # ensure the far door is open
    result = walk_to(door)
    assert result["room_changed"] == 51, f"expected the kitchen (room 51), got {result}"


def test_11_monkey_pickup_meat(monkey_kitchen_client: McpClient) -> None:
    """Pick up hunk o' meat (kitchen checkpoint)."""
    assert_room(monkey_kitchen_client.state(), 51)
    pick_up = bind_verb(monkey_kitchen_client, "pick_up")

    result = pick_up("hunk_o'_meat@@@@@@@")
    assert_inventory_contains(result, "hunk_o'_meat")


def test_12_monkey_use_meat_with_pot_o_soup(monkey_kitchen_client: McpClient) -> None:
    """Use hunk o' meat with pot o' soup (kitchen checkpoint)."""
    assert_room(monkey_kitchen_client.state(), 51)
    pick_up, use = make_verbs(monkey_kitchen_client, "pick_up", "use")

    # Pick the meat up first so this test is self-contained on the checkpoint.
    pick_up("hunk_o'_meat@@@@@@@")
    result = use("hunk o' meat@@@@@@@", "pot_o'_soup@@@@@@")
    messages = result["messages"]
    expected = [{"text": "This will take a while to cook.", "actor": "guybrush"}]
    assert messages == expected, f"unexpected use-meat-on-pot messages: {messages}"
    assert_inventory_does_not_contain(result, "hunk_o'_meat")


def test_13_monkey_give_breath_mint_to_prisoner(
    monkey_prison_client: McpClient,
) -> None:
    """Give the breath mint to the prisoner (prison checkpoint, mint in hand)."""
    assert_room(monkey_prison_client.state(), 54)
    give = bind_verb(monkey_prison_client, "give")

    result = give("breath_mint", "prisoner")
    question = result["question"]
    expected_question = {
        "choices": [
            {"id": 1, "label": "I wanted to say goodbye."},
            {"id": 2, "label": "Won't you help me now?"},
            {"id": 3, "label": "Do you know anything about a magic phrase?"},
        ]
    }
    assert question == expected_question, f"unexpected prisoner dialog: {question}"
    assert_message_present(result, "Don't mention it.")
    removed = result["inventory_removed"]
    assert len(removed) == 1, f"giving the mint should remove it, got {removed}"
    removed = removed[0]
    assert removed == "breath_mint", f"giving the mint should remove it, got {removed}"
    assert_has_position(result)
