"""
Integration test for Zak McKracken and the Alien Mindbenders (SCUMM V2, DOS).

Zak is a full game, so these are compatibility smoke tests rather than a
walkthrough: every test starts from the committed slot-1 save (Zak's bedroom,
room 1, right after the intro) and checks that the shared V0-V2 bridge exposes
the verb bar, the room objects and the classic verb dispatch correctly.

Note: in V0-V2 SCUMM "What is" only names an object (the engine never runs a
sentence for it), so object descriptions come from state.objects, not from
act(verb="what is").
"""

from assertions import (
    assert_has_position,
    assert_inventory_contains,
    assert_message_contains,
    assert_room,
)
from utils import McpClient, bind_verb, find_id, object_by_id

# The bedroom's verb bar. "Open" is verb id 1 in early SCUMM (a real bar verb,
# not the reserved sentence verb it is from V3 on), so it must be listed.
EXPECTED_VERBS = {
    "push",
    "open",
    "walk to",
    "put on",
    "turn on",
    "pull",
    "close",
    "pick up",
    "take off",
    "turn off",
    "give",
    "read",
    "what is",
    "use",
}


def test_zak_initial_state(zak_client: McpClient) -> None:
    """The save lands in Zak's bedroom with the full verb bar and its objects."""
    state = zak_client.state()
    assert_room(state, 1)

    verbs = set(state["verbs"])
    assert verbs == EXPECTED_VERBS, f"unexpected Zak verb bar: {sorted(verbs)}"

    names = {obj["name"] for obj in state["objects"]}
    for expected in ("bed", "door", "telephone", "plastic_card", "cat_clock"):
        assert expected in names, (
            f"expected '{expected}' in room 1, got {sorted(names)}"
        )

    inventory = state["inventory"]
    assert inventory == ["ticket"], f"unexpected starting inventory: {inventory}"


def test_zak_open_door(zak_client: McpClient) -> None:
    """Opening the bedroom door flips its state (verb id 1 dispatch)."""
    open_ = bind_verb(zak_client, "open")
    door = find_id(zak_client.state(), "door")
    assert door is not None, "no 'door' object in room 1"

    result = open_(door)
    changed = [obj["name"] for obj in result["objects_changed"]]
    assert "door" in changed, f"expected the door to change state, got {changed}"

    door_obj = object_by_id(zak_client.state(), door)
    assert door_obj is not None
    state_name = door_obj.get("state_name")
    assert state_name == "opened", f"expected the door to read opened, got {state_name}"


def test_zak_pick_up_sushi(zak_client: McpClient) -> None:
    """Picking up the fish bowl moves Sushi into the inventory."""
    pick_up = bind_verb(zak_client, "pick_up")

    result = pick_up("sushi_in_fish_bowl")
    assert_inventory_contains(result, "sushi_in_fish_bowl")
    assert_message_contains(result, "Sushi")


def test_zak_read_cat_clock(zak_client: McpClient) -> None:
    """The 'read' verb reaches the room script and Zak answers."""
    read = bind_verb(zak_client, "read")

    result = read("cat_clock")
    assert_message_contains(result, "late")


def test_zak_pick_up_plastic_card(zak_client: McpClient) -> None:
    """Picking up the plastic card runs its script (it slides under the desk)."""
    pick_up = bind_verb(zak_client, "pick_up")

    result = pick_up("plastic_card")
    assert_message_contains(result, "desk")
    assert_has_position(result)


def test_zak_walk_moves_ego(zak_client: McpClient) -> None:
    """walk() moves Zak across the bedroom."""
    start = zak_client.state()["position"]

    result = zak_client.walk(start["x"] + 20, start["y"])
    assert_has_position(result)
    end = zak_client.state()["position"]
    assert end != start, f"walk did not move Zak (still at {end})"
