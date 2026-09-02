"""
Integration test for the Space Quest 6 demo (SCI engine, SCI2.1/SCI32).

The same bridge as Gabriel Knight, reached through the other of SCI's two game
cycles: SCI16 games hand time back in kGameIsRestarting, SCI32 games in
GfxFrameout::throttle(). So what this covers that test_gk1 does not is that the
SCI32 path is pumped at all - that the server answers, the cast is read, and a
stream opens and closes.

Space Quest 6 writes its verbs along the bottom of the screen (FEET, EYES,
HANDS, MOUTH). The bridge reaches them by cycling the cursor rather than by
clicking the bar, because cycling needs no screen coordinates.

No save support, so this is one ordered sequence on a fresh instance.
"""

import time

import pytest

from mcp_client import McpClient

pytestmark = [pytest.mark.xdist_group("sq6")]

VERBS = ["use", "look_at", "walk_to", "talk_to"]


def _skip_into_the_game(client: McpClient, tries: int = 30) -> dict:
    """Escape pauses the introduction and offers Skip / Continue / Quit; the
    first of those buttons is Skip, and the demo then plays a little more of
    its opening before it hands over.

    The pause matters: each card takes a moment to give way, and asking again
    immediately just spends the whole budget on the first one."""
    for _ in range(tries):
        state = client.state()
        if len(state.get("objects") or []) > 1:
            return state
        try:
            client.skip()
        except RuntimeError as exc:
            if "starting up" not in str(exc) and "nothing to skip" not in str(exc):
                raise
        client.call_tool("mouse_click", {"x": 248, "y": 172})
        time.sleep(2)
    raise AssertionError("the introduction never gave way to a room")


def _wait_for_control(client: McpClient, tries: int = 60) -> dict:
    """The state once the game hands control back.

    The demo keeps playing after the room appears - the ego walks in on its
    own, and while it does the game is not accepting input. Under load that
    walk is a long wait, so anything that acts waits it out first."""
    for _ in range(tries):
        state = client.state()
        if state.get("can_act"):
            return state
        time.sleep(1)
    raise AssertionError("the demo never handed control over")


@pytest.fixture(scope="session")
def playing(sq6_client: McpClient) -> McpClient:
    _skip_into_the_game(sq6_client)
    return sq6_client


def test_01_the_sci32_cycle_is_pumped_at_all(playing: McpClient) -> None:
    """If throttle() were not pumping, nothing here would answer."""
    state = playing.state()
    assert "room" in state and isinstance(state["room"].get("id"), int)


def test_02_the_room_names_its_cast(playing: McpClient) -> None:
    names = {o["name"] for o in playing.state()["objects"]}
    assert names, "the room came back empty"
    # Every name is an identifier, not a raw script name with humps in it.
    for name in names:
        assert name == name.lower(), f"{name} was never folded"
        assert " " not in name, f"{name} has a space in it"


def test_03_the_text_objects_a_game_draws_with_are_not_targets(
    playing: McpClient,
) -> None:
    """SQ6's opening room puts four dtext objects in the cast. They are screen
    furniture, and offering them as things to look at is a distraction."""
    names = {o["name"] for o in playing.state()["objects"]}
    assert not any(n.startswith("dtext") for n in names), sorted(names)


def test_04_state_lists_this_games_verbs(playing: McpClient) -> None:
    assert playing.state()["verbs"] == VERBS


def test_05_a_verb_from_another_game_is_refused(playing: McpClient) -> None:
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="ask_about", target1="ego")
    assert "ask_about" in str(excinfo.value)


def test_06_a_stream_opens_and_closes(playing: McpClient) -> None:
    """A walk is the cheapest streaming call; what matters is that it returns
    rather than hanging on a cycle that never advances."""
    _wait_for_control(playing)
    result = playing.walk(320, 300)
    assert "room" in result, result
