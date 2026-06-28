"""
Integration tests for the Maniac Mansion C64 demo phone (save slot 2).

Save slot 2 starts next to the phone (room 5); using it opens the dial pad
(room 43) where the `dial` tool presses the keypad buttons. Runs against its
own fixture/instance so it can execute in parallel with the walkthrough tests
in test_maniac_c64.py (pytest-xdist --dist=loadgroup).
"""

from time import sleep

import pytest

from utils import McpClient, bind_verb, message_texts, object_names

PHONE_ROOM = 5
DIAL_PAD_ROOM = 43


def _wait_for_room(
    client: McpClient, room_id: int, tries: int = 20, poll: float = 0.5
) -> bool:
    """Poll until the client is in *room_id*; return True if reached."""
    for _ in range(tries):
        if client.state()["room"]["id"] == room_id:
            return True
        sleep(poll)
    return False


def test_10_maniac_dial_requires_dial_pad(maniac_phone_client: McpClient) -> None:
    """dial() is rejected while the dial pad is not on screen."""
    state = maniac_phone_client.state()
    room = state["room"]
    names = object_names(state)
    assert room["id"] == PHONE_ROOM, f"expected phone room {PHONE_ROOM}, got {room}"
    assert "phone" in names, f"expected a phone object, got {sorted(names)}"
    with pytest.raises(RuntimeError, match="no dial pad"):
        maniac_phone_client.dial("1234")


def test_11_maniac_use_phone_then_dial(maniac_phone_client: McpClient) -> None:
    """Use the phone, wait for the dial pad, then dial a 4-digit number.

    Each keypad press echoes its digit as a message — which also validates the
    button-grid mapping — and after the 4th digit the call resolves and the
    game returns to the phone room.
    """
    use = bind_verb(maniac_phone_client, "use")
    use("phone")
    reached = _wait_for_room(maniac_phone_client, DIAL_PAD_ROOM)
    assert reached, "dial pad room never appeared after using the phone"

    result = maniac_phone_client.dial("1234")
    echoed = message_texts(result)
    room_changed = result.get("room_changed")
    assert echoed == ["1", "2", "3", "4"], f"unexpected keypad echo: {echoed}"
    assert room_changed == PHONE_ROOM, f"return room {PHONE_ROOM} != {room_changed}"


def test_12_maniac_dial_rejects_invalid_keys(maniac_phone_client: McpClient) -> None:
    """Non-keypad characters are rejected up front."""
    with pytest.raises(RuntimeError, match="invalid keypad key"):
        maniac_phone_client.dial("12a4")
