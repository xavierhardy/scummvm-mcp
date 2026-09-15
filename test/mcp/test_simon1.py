"""Simon the Sorcerer (Amiga): the first room, played through the tools.

Calypso's cottage has one of everything that went wrong for an agent: a thing
to pick up, carried things with icons of their own, a note that asks a
question, a postcard that opens a menu over the game, the verbs that want a
second thing ("Use magnet with"), and a door out.
"""

from __future__ import annotations

import pytest

from mcp_client import McpClient
from state_helpers import wait_until_taking_input


def _ready(client: McpClient) -> dict:
    return wait_until_taking_input(client, "simon1", want_objects=True)


def _names(entries: list[dict] | None) -> list[str]:
    return [entry["name"] for entry in entries or []]


def test_the_room_names_its_scenery_and_not_the_inventory(
    simon1_client: McpClient,
) -> None:
    """The door and the fridge are things to act on; the map is carried.

    Scenery has no item behind it, only a name, and was left out; the icons on
    the inventory strip have items behind them, and were listed as if they
    stood in the room.
    """
    state = _ready(simon1_client)
    objects = _names(state.get("objects"))
    carried = _names(state.get("inventory"))
    for expected in ("outside", "fridge", "magnet"):
        assert expected in objects, f"{expected} missing from {objects}"
    assert "map" in carried, carried
    for item in carried:
        assert item not in objects, f"{item} is carried and listed in the room too"


def test_taking_the_magnet_waits_for_simon_to_take_it(
    simon1_client: McpClient,
) -> None:
    """The result is read once he has it, not the moment the click lands."""
    _ready(simon1_client)
    result = simon1_client.act("take", "magnet")
    assert "magnet" in (result.get("items_gained") or []), result
    assert "magnet" in (result.get("objects_gone") or []), result
    state = simon1_client.state()
    assert "magnet" in _names(state.get("inventory"))
    assert "magnet" not in _names(state.get("objects"))


def test_the_note_asks_and_is_answered(simon1_client: McpClient) -> None:
    """Looking at the note offers two lines, and answering one goes back to the game."""
    _ready(simon1_client)
    result = simon1_client.act("look_at", "calypsos_note")
    choices = (result.get("question") or {}).get("choices") or []
    assert [choice["label"] for choice in choices] == ["Yes please.", "No thanks."], (
        result
    )
    assert simon1_client.state().get("question"), "the question went missing from state"

    with pytest.raises(RuntimeError, match="question"):
        simon1_client.act("walk_to", "outside")

    after = simon1_client.answer(2)
    assert not after.get("question"), after
    assert after.get("can_act"), after


def test_the_postcard_menu_is_closed_by_skip(simon1_client: McpClient) -> None:
    """The postcard opens Load / Save / Quit / Continue over the game.

    A click meant for the room lands on one of those words - "outside" is
    where Quit is written - so nothing but skip is taken until it is closed.
    """
    _ready(simon1_client)
    opened = simon1_client.act("use", "postcard")
    assert opened.get("menu_open") is True, opened

    with pytest.raises(RuntimeError, match="menu"):
        simon1_client.act("walk_to", "outside")

    closed = simon1_client.skip()
    assert closed.get("menu_open") is False, closed
    assert simon1_client.state().get("can_act"), (
        "the game did not come back after the menu"
    )


def test_use_and_give_take_their_second_thing(simon1_client: McpClient) -> None:
    """Use with one thing waits for the second; a target2, or the next act, is it."""
    _ready(simon1_client)
    simon1_client.act("take", "magnet")

    half = simon1_client.act("use", "magnet")
    assert half.get("awaiting_second_target") is True, half
    finished = simon1_client.act("use", "fridge")
    assert finished.get("awaiting_second_target") is False, finished
    assert finished.get("can_act"), finished

    given = simon1_client.act("give", "magnet", "doggie")
    assert given.get("awaiting_second_target") is False, given
    assert given.get("can_act"), given


def test_the_door_leads_out_of_the_cottage(simon1_client: McpClient) -> None:
    """Leaving reports the room that was reached, with its things in it."""
    before = _ready(simon1_client)
    result = simon1_client.act("walk_to", "outside")
    room = result.get("room") or {}
    assert room.get("changed") is True, result
    assert room.get("id") != (before.get("room") or {}).get("id"), result
    appeared = result.get("objects_appeared") or []
    assert appeared, f"the new room named nothing: {result}"
    assert sorted(_names(simon1_client.state().get("objects"))) == sorted(appeared)
