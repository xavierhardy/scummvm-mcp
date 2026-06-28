"""
Integration test for Maniac Mansion C64 demo.
Walkthrough: door mat -> key -> use key -> front door,
then kid switching via the switch_character tool.
The phone/dial tests live in test_maniac_phone.py (save slot 2).
"""

import pytest

from assertions import assert_has_position, assert_inventory_contains
from utils import McpClient, bind_verb, make_verbs, object_names


def _take_key(client: McpClient) -> None:
    """Pull the door mat (revealing the key) and pick the key up.

    Self-contained setup: the key only exists once the mat is pulled, so tests
    that need the key run this first instead of relying on a sibling test.
    """
    if "key" not in client.state().get("inventory", []):
        client.act("pull", "door mat")
        client.act("pick_up", "key")


def _unlock_front_door(client: McpClient) -> None:
    """Take the key and use it on the front door (unlocks it)."""
    _take_key(client)
    client.act("use", "key", "front_door")


def _reach_kitchen(client: McpClient) -> None:
    """Enter the mansion and step into the kitchen (room 7)."""
    _unlock_front_door(client)
    client.act("walk_to", "front_door")  # -> entrance hall (room 10)
    client.act("open", 35)  # kitchen door (shares the name "door")
    client.act("walk_to", 35)  # -> kitchen (room 7)


def _other_character(characters: list, current: str) -> str:
    """Return a switchable kid other than *current*."""
    return next(c for c in characters if c != current)


def test_01_maniac_initial_state(maniac_client: McpClient) -> None:
    """Verify initial game state."""
    state = maniac_client.state()
    room = state.get("room")
    assert isinstance(room, dict), f"expected a room dict, got {room}"
    assert room["id"] == 1, f"expected room 1, got {room}"
    assert state.get("objects") is not None, "expected objects in state"


def test_02_maniac_walk_to_front_door(maniac_client: McpClient) -> None:
    """Walk to front door."""
    walk_to = bind_verb(maniac_client, "walk_to")
    assert_has_position(walk_to("front_door"))


def test_03_maniac_pull_door_mat(maniac_client: McpClient) -> None:
    """Pull door mat."""
    pull = bind_verb(maniac_client, "pull")
    result = pull("door mat")
    changed = result["objects_changed"]
    assert changed[0]["name"] == "door mat", f"door mat should change, got {changed}"
    assert_has_position(result)


def test_04_maniac_pickup_key(maniac_client: McpClient) -> None:
    """Pick up the key from under the door mat."""
    pull, pick_up = make_verbs(maniac_client, "pull", "pick_up")
    pull("door mat")  # reveal the key first so this test stands alone
    assert_inventory_contains(pick_up("key"), "key")


def test_05_maniac_use_key_on_door(maniac_client: McpClient) -> None:
    """Unlock front door with key."""
    _take_key(maniac_client)
    use = bind_verb(maniac_client, "use")
    changed = use("key", "front_door")["objects_changed"]
    assert changed[0]["name"] == "front door", f"front door unchanged: {changed}"


def test_06_maniac_walk_through_front_door(maniac_client: McpClient) -> None:
    """Walk through the unlocked front door into the mansion (room change)."""
    _unlock_front_door(maniac_client)
    walk_to = bind_verb(maniac_client, "walk_to")
    result = walk_to("front_door")
    assert result.get("room_changed"), f"unlocked door should change rooms: {result}"


def test_07_maniac_state_lists_characters(maniac_client: McpClient) -> None:
    """state() exposes the switchable kids and the currently controlled one."""
    state = maniac_client.state()
    characters = state.get("available_characters")
    assert characters, f"Expected switchable characters in state, got {state}"
    assert len(characters) >= 2, f"expected >=2 kids, got {characters}"
    assert state.get("controlling") in characters, f"controlling not among {characters}"


def test_08_maniac_switch_character_round_trip(maniac_client: McpClient) -> None:
    """Switch to another kid and back, verifying state tracks the change."""
    state = maniac_client.state()
    characters = state["available_characters"]
    original = state["controlling"]
    other = _other_character(characters, original)

    maniac_client.switch_character(other)
    assert maniac_client.state()["controlling"] == other, f"expected to control {other}"

    maniac_client.switch_character(original)
    controlling = maniac_client.state()["controlling"]
    assert controlling == original, f"expected to control {original}"


def test_09_maniac_switch_character_rejects_unknown_name(
    maniac_client: McpClient,
) -> None:
    """Switching to a non-switchable name fails with the available kids listed."""
    with pytest.raises(RuntimeError, match="unknown character"):
        maniac_client.switch_character("purple tentacle")


def test_10_maniac_parent_state_key_hidden_until_mat_pulled(
    maniac_client: McpClient,
) -> None:
    """The key under the door mat can't be taken before the mat is pulled.

    Mirrors the engine's findObject() parent-state gate (cf. Indy3's
    test_13_indy3_hidden_objects_not_selectable): while the mat is un-pulled the
    key is parented to it with a revealing state it hasn't reached, so the MCP
    neither lists it nor resolves it by name. Pulling the mat reveals it.
    """
    pull, pick_up = make_verbs(maniac_client, "pull", "pick_up")
    names = object_names(maniac_client.state())
    assert "key" not in names, "key leaked into state before the mat was pulled"
    with pytest.raises(RuntimeError, match="unknown target1"):
        pick_up("key")

    pull("door mat")
    names = object_names(maniac_client.state())
    assert "key" in names, "key should appear once the mat is pulled"
    assert_inventory_contains(pick_up("key"), "key")


def test_11_maniac_parent_state_fridge_contents_hidden_until_opened(
    maniac_client: McpClient,
) -> None:
    """Fridge contents can't be taken before the refrigerator is opened.

    Same parent-state gate as the key: the can of pepsi, old batteries, cheese,
    etc. are parented to the fridge with parentstate=open, so they stay hidden
    (and unresolvable by name) until it is opened.
    """
    _reach_kitchen(maniac_client)
    open_, pick_up = make_verbs(maniac_client, "open", "pick_up")
    names = object_names(maniac_client.state())
    hidden = {"can_of_pepsi", "old_batteries", "cheese", "lettuce"}
    assert hidden.isdisjoint(names), f"fridge contents leaked: {hidden & names}"
    with pytest.raises(RuntimeError, match="unknown target1"):
        pick_up("can_of_pepsi")

    open_("refrigerator")
    names = object_names(maniac_client.state())
    assert "can_of_pepsi" in names, "fridge contents should appear once it is opened"
    assert_inventory_contains(pick_up("old_batteries"), "old_batteries")
