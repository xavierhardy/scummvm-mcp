"""
Integration test for the Gabriel Knight demo (SCI engine, SCI1.1).

SCI is unlike the pointer games the other bridges cover. It does not label what
the player points at, so there is no hover text to sweep for; what it has
instead is a script-level name on every object - the identifier the game's own
author typed - which the bridge reads straight out of the cast. So the thing
worth asserting here is that a room comes back named the way its author named
it: coffee_pot, graces_coat, magnifying_glass, shop_door.

The other half is verbs. There is no verb bar to read and no list of verbs in
the script state: the cursor is the verb, and the right mouse button cycles it.
The bridge reaches one the way a player does, and `state` says which verbs
there are and which is showing.

The demo has no save support - it answers "game cannot be saved in the current
state" wherever it is asked - so the whole run is one ordered sequence on a
single fresh instance, clicked in from the title screen.
"""

import time

import pytest

from mcp_client import McpClient

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("gk1")]

BOOKSTORE = 210
# The verbs the bridge's table gives this game, in the order it lists them.
VERBS = ["walk_to", "look_at", "talk_to", "ask_about", "take", "use", "open", "move"]


def _click_into_the_bookstore(client: McpClient, tries: int = 30) -> dict:
    """Click past the title card and the DAY 1 card into the shop.

    The pause matters: each card takes a moment to give way, and asking again
    immediately just spends the whole budget on the first one.
    """
    for _ in range(tries):
        state = client.state()
        if (state.get("room") or {}).get("id") == BOOKSTORE:
            return state
        client.call_tool("mouse_click", {"x": 160, "y": 100})
        time.sleep(2)
    raise AssertionError("the opening never gave way to the bookstore")


@pytest.fixture(scope="session")
def playing(gk1_client: McpClient) -> McpClient:
    """The demo, in the bookstore."""
    _click_into_the_bookstore(gk1_client)
    return gk1_client


def test_01_the_room_is_named_by_its_own_script(playing: McpClient) -> None:
    state = playing.state()
    room = state["room"]
    assert room["id"] == BOOKSTORE, f"not in the bookstore: {room}"
    # SCI rooms are numbered, not named; the room object's name is the
    # readable half and the bridge reports both.
    assert room.get("name") == "bookstore", f"unexpected room name: {room}"


def test_02_the_things_in_the_room_carry_their_authors_names(playing: McpClient) -> None:
    names = {o["name"] for o in playing.state()["objects"]}
    # A handful that the shop demonstrably contains. Not the whole list: the
    # cast changes as the scene plays, and a test that pinned all of it would
    # fail on a frame rather than on a fault.
    for expected in ("coffee_pot", "magnifying_glass", "shop_door"):
        assert expected in names, f"{expected} missing from {sorted(names)}"


def test_03_the_bookkeeping_objects_are_not_offered(playing: McpClient) -> None:
    """A cast holds the interpreter's movers and timers as well as the actors."""
    names = {o["name"] for o in playing.state()["objects"]}
    for internal in ("a_mover", "cycler", "sound", "dtext"):
        assert internal not in names, f"{internal} should not be a target"


def test_04_everything_named_can_be_aimed_at(playing: McpClient) -> None:
    for obj in playing.state()["objects"]:
        assert obj["x"] >= 0 and obj["y"] >= 0, f"{obj} has nowhere to click"


def test_05_state_lists_the_verbs_this_game_has(playing: McpClient) -> None:
    assert playing.state()["verbs"] == VERBS


def test_06_a_verb_the_game_does_not_have_is_refused_by_name(playing: McpClient) -> None:
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="dance", target1="shop_door")
    message = str(excinfo.value)
    assert "dance" in message
    # The refusal says what *is* possible rather than only what is not.
    assert "look_at" in message


def test_07_a_target_that_is_not_here_is_refused_with_the_ones_that_are(
    playing: McpClient,
) -> None:
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="look_at", target1="starship")
    message = str(excinfo.value)
    assert "starship" in message
    assert "shop_door" in message


def test_08_looking_at_something_reaches_the_game(playing: McpClient) -> None:
    """The verb is the cursor, so a look has to cycle the cursor first."""
    playing.state()  # drain what the opening said
    playing.act(verb="look_at", target1="magnifying_glass")
    # Whatever the game answers, the cursor is the one that was asked for:
    # that is the part the bridge is responsible for.
    assert playing.state().get("current_verb") in (None, "look_at"), (
        "the cursor moved off the verb the act asked for"
    )


def test_09_the_lines_the_game_says_are_captured(playing: McpClient) -> None:
    """SCI draws its subtitles through a text control that blits separately;
    a bridge that only watched the immediate-draw path would hear none of it."""
    seen: list[str] = []
    for _ in range(12):
        seen += [m["text"] for m in playing.state().get("messages", [])]
        # A line said while an action is running arrives on that action's own
        # stream rather than in the next snapshot.
        seen += [m.get("text", "") for m in playing.last_notifications]
        if any(len(line.strip()) > 4 for line in seen):
            return
        playing.call_tool("mouse_click", {"x": 160, "y": 180})
        time.sleep(2)
    raise AssertionError(f"the game said nothing through the whole scene: {seen}")
