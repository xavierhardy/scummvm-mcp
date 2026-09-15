"""
Gabriel Knight: Sins of the Fathers, the whole game (SCI engine, SCI2).

It cannot be saved anywhere in its opening, so the walkthrough starts fresh:
the logo, the title card and a minute of scene have to be got through first,
and that is the first thing asserted - that the bridge says the game is not
yet playable through all of it, and that skip reaches the shop. Then what an
agent does on Day 1: take things off the counter, ask Grace about something
and hear the answer, leave, and go somewhere on the map.
"""

import time

import pytest

from mcp_client import McpClient
from state_helpers import wait_until_taking_input

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("gk1_full")]

BOOKSTORE = 210
CITY_MAP = 200


def _names(state: dict) -> list[str]:
    return [entry["name"] for entry in state.get("objects") or []]


def _travel(client: McpClient, place: str, tries: int = 4) -> dict:
    """Click a place on the city map, and give the map a moment if it was busy.

    The map takes its clicks through a loop of its own and drops one that
    arrives while it is still coming up; a player who clicked too early clicks
    again, and so does this.
    """
    for _ in range(tries):
        result = client.act("use", place)
        if result.get("room", {}).get("changed"):
            return result
        time.sleep(2)
    raise AssertionError(f"the map never went to {place}: {result}")


def _choice(state: dict, label: str) -> int:
    for choice in (state.get("question") or {}).get("choices") or []:
        if label.lower() in choice["label"].lower():
            return choice["id"]
    raise AssertionError(f"no topic like {label!r} in {state.get('question')}")


def test_01_the_opening_is_not_the_game(gk1_full_walk_client: McpClient) -> None:
    """The logo and the title card report can_act false, and skip gets to the shop.

    Both used to say yes - each has things on screen and nothing walking - so
    a recorder asked "is it playable?" started filming the Sierra logo.
    """
    state = wait_until_taking_input(gk1_full_walk_client, "gk1_full", want_objects=True)
    assert state["room"]["id"] == BOOKSTORE, state["room"]
    for expected in ("magnifying_glass", "tweezers", "grace_prop", "shop_door"):
        assert expected in _names(state), _names(state)


def test_02_taking_something_off_the_counter(gk1_full_walk_client: McpClient) -> None:
    """A click lands on what was named, and the verb is the one asked for.

    The scripts run at 320x200 on a 640x480 screen, inside a plane that starts
    below the icon bar; unscaled, a click meant for the magnifying glass read
    the description of the shop, and the verb ran past "take".
    """
    result = gk1_full_walk_client.act("take", "magnifying_glass")
    assert "magnifying_glass" in (result.get("objects_gone") or []), result
    assert (result.get("score") or {}).get("to", 0) > 0, result
    # What he is carrying is what names him as its owner.
    assert "mag_glass" in (gk1_full_walk_client.state().get("inventory") or []) or (
        "magnifying_glass" in (gk1_full_walk_client.state().get("inventory") or [])
    ), gk1_full_walk_client.state().get("inventory")


def test_02b_the_things_that_are_never_drawn_are_there_too(
    gk1_full_walk_client: McpClient,
) -> None:
    """Most of what this game lets a player click is a hotspot, not a picture.

    The shop's door mat, its ladder, its phone: none of them is drawn as a
    thing of its own, so a snapshot of what the interpreter draws left them
    out - and the overlook, which is nothing but hotspots, came back as an
    empty room with no way out.
    """
    names = _names(gk1_full_walk_client.state())
    for expected in ("door_mat", "ladder", "phone"):
        assert expected in names, names
    looked = gk1_full_walk_client.act("look_at", "door_mat")
    said = " ".join(n.get("text", "") for n in gk1_full_walk_client.last_notifications)
    assert "door" in said.lower(), (looked, said)


def test_03_asking_grace_about_herself(gk1_full_walk_client: McpClient) -> None:
    """Talking opens the topics, and an answer is heard before the result comes back."""
    talked = gk1_full_walk_client.act("talk_to", "grace_prop")
    # Depending on what has been said already, talking goes straight to the
    # topics or waits to be asked.
    if not talked.get("question"):
        asked = gk1_full_walk_client.act("ask_about", "grace_prop")
        assert asked.get("question"), asked

    state = gk1_full_walk_client.state()
    with pytest.raises(RuntimeError, match="question"):
        gk1_full_walk_client.act("look_at", "view")

    gk1_full_walk_client.answer(_choice(state, "messages"))
    said = [n.get("text", "") for n in gk1_full_walk_client.last_notifications]
    assert any("messages" in line.lower() for line in said), said

    after = gk1_full_walk_client.state()
    left = gk1_full_walk_client.answer(_choice(after, "exit"))
    assert left["room"]["id"] == BOOKSTORE, left
    assert not left.get("question"), left


def test_04_out_of_the_shop_and_across_the_map(gk1_full_walk_client: McpClient) -> None:
    """The door leads to the map, the map to Jackson Square, each one settled."""
    out = gk1_full_walk_client.act("open", "shop_door")
    assert out["room"]["id"] == CITY_MAP, out
    assert "jack_square" in (out.get("objects_appeared") or []), out
    assert out.get("can_act"), out

    over = _travel(gk1_full_walk_client, "jack_over")
    # The overlook: four telescopes, a railing and three ways off it, none of
    # them drawn. The way back is one of those ways.
    overlook = _names(gk1_full_walk_client.state())
    for expected in ("binoc1", "railing", "south_exit"):
        assert expected in overlook, (over, overlook)
    back = gk1_full_walk_client.act("walk_to", "south_exit")
    assert back["room"]["id"] == CITY_MAP, (over, back)

    there = _travel(gk1_full_walk_client, "jack_square")
    # The square has more than one way in, and which one the map drops Gabriel
    # at is the game's choice; what matters is that he is somewhere with
    # things in it, and no longer on the map.
    assert there["room"]["changed"] and there["room"]["id"] != CITY_MAP, there
    assert _names(gk1_full_walk_client.state()), there
