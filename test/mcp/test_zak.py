"""
Integration test for Zak McKracken and the Alien Mindbenders (SCUMM V2, DOS).

Zak is a full game, so these are compatibility smoke tests rather than a
walkthrough: every test starts from the committed slot-1 save (Zak's bedroom,
room 1, right after the intro) and checks that the shared V0-V2 bridge exposes
the verb bar, the room objects and the classic verb dispatch correctly.

Note: in V0-V2 SCUMM "What is" only names an object — the engine never runs a
sentence for it, it just writes the name into the sentence line — so
act(verb="what is") answers with that name rather than with a description.
"""

from time import sleep

import pytest

from assertions import (
    assert_has_position,
    assert_inventory_contains,
    assert_message_contains,
    assert_room,
)
from utils import (
    McpClient,
    bind_verb,
    find_id,
    joined_message_text,
    make_verbs,
    message_texts,
    object_by_id,
)

# Rooms reached by the tests below: the phone's keypad close-up, and the living
# room next door (slot 2, parked there with the TV playing).
DIAL_PAD_ROOM = 52
LIVING_ROOM = 2


def _wait_for_room(client: McpClient, room_id: int, tries: int = 20) -> bool:
    """Poll until the client is in *room_id*; return True if it got there."""
    for _ in range(tries):
        if client.state()["room"]["id"] == room_id:
            return True
        sleep(0.5)
    return False


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


def test_zak_what_is_names_the_object(zak_client: McpClient) -> None:
    """'What is' answers with the object's name, the way the sentence line does.

    The verb runs no script in V0-V2, so dispatching it as a sentence would walk
    Zak across the room only to be told "That doesn't seem to work".
    """
    state = zak_client.state()
    assert "what is" in state["verbs"], f"'what is' is not on the verb bar: {state}"
    bed = object_by_id(state, find_id(state, "bed"))
    assert "what is" in bed["compatible_verbs"], f"not offered on the bed: {bed}"

    result = zak_client.act("what is", "bed")
    assert_message_contains(result, "bed")
    assert "doesn't seem to work" not in joined_message_text(result), (
        f"'what is' was dispatched as a sentence: {result}"
    )


def test_zak_walk_moves_ego(zak_client: McpClient) -> None:
    """walk() moves Zak across the bedroom."""
    start = zak_client.state()["position"]

    result = zak_client.walk(start["x"] + 20, start["y"])
    assert_has_position(result)
    end = zak_client.state()["position"]
    assert end != start, f"walk did not move Zak (still at {end})"


def test_zak_dial_requires_the_dial_pad(zak_client: McpClient) -> None:
    """dial() is rejected until the phone's keypad close-up is on screen."""
    with pytest.raises(RuntimeError, match="no dial pad"):
        zak_client.dial("555")


def test_zak_use_phone_then_dial(zak_client: McpClient) -> None:
    """Using the bedroom phone opens the keypad, which dial() then presses.

    Zak's pad is a 3x4 grid of unnamed buttons (only the '6' carries a name),
    so the digits echoed back also check that the grid was mapped correctly.
    """
    use = bind_verb(zak_client, "use")
    use("telephone")
    assert _wait_for_room(zak_client, DIAL_PAD_ROOM), "the dial pad never appeared"

    result = zak_client.dial("536")
    assert message_texts(result) == ["5", "3", "6"], f"unexpected keypad echo: {result}"


def test_zak_tv_chatter_does_not_stall_actions(zak_tv_client: McpClient) -> None:
    """The living-room TV talks forever; actions must still finish.

    Slot 2 is parked in the living room with the TV playing. Its lines used to
    keep every action's settle window open until the stream failed as "action
    timed out"; now they are captured in the background and the action ends.
    """
    assert_room(zak_tv_client.state(), LIVING_ROOM)
    chatter: list = []
    for _ in range(20):
        chatter += message_texts(zak_tv_client.state())
        if chatter:
            break
        sleep(1.0)
    assert chatter, "the TV is not playing in this save — the test proves nothing"

    open_, close = make_verbs(zak_tv_client, "open", "close")
    # Each of these raises RuntimeError("action timed out") if the TV holds the
    # stream open, so completing at all is the assertion.
    assert_has_position(open_("refrigerator"))
    close("refrigerator")
    assert_has_position(zak_tv_client.act("walk to", "stove"))
