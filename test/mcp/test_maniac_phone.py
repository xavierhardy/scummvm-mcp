"""
Integration tests for the Maniac Mansion C64 demo phone (save slot 2).

Save slot 2 starts next to the phone (room 5); using it opens the dial pad
(room 43) where the `dial` tool presses the keypad buttons. Runs against its
own fixture/instance so it can execute in parallel with the walkthrough tests
in test_maniac_c64.py (pytest-xdist --dist=loadfile).
"""

from time import sleep

import pytest

from utils import McpClient

PHONE_ROOM = 5
DIAL_PAD_ROOM = 43


def test_10_maniac_dial_requires_dial_pad(maniac_phone_client: McpClient) -> None:
    """dial() is rejected while the dial pad is not on screen."""
    state = maniac_phone_client.state()
    assert state["room"]["id"] == PHONE_ROOM
    assert any(o["name"] == "phone" for o in state["objects"])
    with pytest.raises(RuntimeError, match="no dial pad"):
        maniac_phone_client.dial("1234")


def test_11_maniac_use_phone_then_dial(maniac_phone_client: McpClient) -> None:
    """Use the phone, wait for the dial pad, then dial a 4-digit number.

    Each keypad press echoes its digit as a message — which also validates the
    button-grid mapping — and after the 4th digit the call resolves and the
    game returns to the phone room.
    """
    maniac_phone_client.act("use", "phone")
    for _ in range(20):
        if maniac_phone_client.state()["room"]["id"] == DIAL_PAD_ROOM:
            break
        sleep(0.5)
    else:
        raise AssertionError("dial pad room never appeared after using the phone")

    result = maniac_phone_client.dial("1234")
    assert [m["text"] for m in result["messages"]] == ["1", "2", "3", "4"]
    assert result.get("room_changed") == PHONE_ROOM


def test_12_maniac_dial_rejects_invalid_keys(maniac_phone_client: McpClient) -> None:
    """Non-keypad characters are rejected up front."""
    with pytest.raises(RuntimeError, match="invalid keypad key"):
        maniac_phone_client.dial("12a4")
