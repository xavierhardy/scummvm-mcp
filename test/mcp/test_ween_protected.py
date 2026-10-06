"""
The Ween demo with the game's copy_protection option on.

By default the bridge answers Ween's copy-protection screen itself (see
test_ween.py). ScummVM's copy_protection option is how a player asks for the
protection a game would otherwise have bypassed, and the bridge honours it:
the screen stays up - a row of coloured cards, waiting for the digit of the
right one - and getting past it is the agent's to type. `type_text` is what
does that.

No save support, so this is one ordered sequence on a fresh instance.
"""

import time

import pytest

from mcp_client import McpClient

pytestmark = [pytest.mark.xdist_group("ween_protected")]


@pytest.fixture(scope="session")
def playing(ween_protected_client: McpClient) -> McpClient:
    return ween_protected_client


def _at_the_protection_screen(client: McpClient, tries: int = 20) -> dict:
    """Skip the opening until the screen with the coloured cards is up."""
    for _ in range(tries):
        state = client.state()
        if state["room"]["name"] == "intro" and len(state.get("objects") or []) >= 8:
            return state
        try:
            client.skip()
        except RuntimeError as exc:
            if "nothing to skip" not in str(exc):
                raise
        time.sleep(2)
    raise AssertionError("the opening never reached the copy-protection screen")


def test_01_the_screen_stays_until_something_is_typed(playing: McpClient) -> None:
    _at_the_protection_screen(playing)
    # Given time to answer it, the bridge does not: the cards are still there.
    time.sleep(3)
    state = playing.state()
    assert state["room"]["name"] == "intro", state
    # Eight cards, one per colour. They are hotspots with no label: this
    # screen paints no hover text, and a number is what it wants anyway.
    assert len(state["objects"]) >= 8, state


def test_02_typing_reaches_the_game(playing: McpClient) -> None:
    """The tool exists for exactly this screen. What it answers is what it
    typed; that the game took it is visible in the answer boxes filling in."""
    _at_the_protection_screen(playing)
    result = playing.call_tool("type_text", {"text": "1"})
    assert result == {"text": "1", "enter": True}, result
