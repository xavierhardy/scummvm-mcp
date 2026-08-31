"""
Integration test for the Ween: The Prophecy demo (Gob engine).

Ween is here for two things neither of the other gob demos shows.

The first is staying alive through video. The engine's pump point is
Util::processInput, which a video's own wait loop never reaches: it sits in
Util::delay. For the whole of Ween's opening the server used to stop
answering, and a skip issued inside it could not even end itself, because the
budget that closes a stream is only ever checked from a pump.

The second is typing. This demo will not start until a number is typed: it
opens on a copy-protection screen - a row of coloured cards, four spaces
waiting for the number of the colour that belongs in each - and an agent that
can only point at things is stuck there forever. `type_text` is what gets past
it, and this is the only game instrumented here that registers the tool.

No save support, so this is one ordered sequence on a fresh instance.
"""

import time

import pytest

from mcp_client import McpClient

pytestmark = [pytest.mark.xdist_group("ween")]


@pytest.fixture(scope="session")
def playing(ween_client: McpClient) -> McpClient:
    return ween_client


def test_01_the_server_answers_while_the_opening_plays(playing: McpClient) -> None:
    """Three reads in a row, through the video the demo opens with."""
    for _ in range(3):
        state = playing.state()
        assert "room" in state, state


def test_02_a_skip_returns_rather_than_hanging(playing: McpClient) -> None:
    """It does not matter what the skip lands on - only that it comes back.
    A skip inside a video is exactly the case that used to hang forever."""
    try:
        result = playing.skip()
    except RuntimeError as exc:
        # "nothing to skip" is an answer; a timeout is not.
        assert "nothing to skip" in str(exc), exc
        return
    assert isinstance(result, dict), result


def _at_the_protection_screen(client: McpClient, tries: int = 10) -> dict:
    """Skip the opening until the screen with the coloured cards is up."""
    for _ in range(tries):
        state = client.state()
        if len(state.get("objects") or []) >= 8:
            return state
        try:
            client.skip()
        except RuntimeError as exc:
            if "nothing to skip" not in str(exc):
                raise
        time.sleep(2)
    raise AssertionError("the opening never reached the copy-protection screen")


def test_03_the_opening_gives_way_to_the_screen_that_wants_typing(
    playing: McpClient,
) -> None:
    state = _at_the_protection_screen(playing)
    # Eight cards, one per colour. They are hotspots with no label: this
    # screen paints no hover text, and a number is what it wants anyway.
    assert len(state["objects"]) >= 8, state


def test_04_typing_reaches_the_game(playing: McpClient) -> None:
    """The tool exists for exactly this screen. What it answers is what it
    typed; that the game took it is visible in the answer boxes filling in."""
    _at_the_protection_screen(playing)
    result = playing.call_tool("type_text", {"text": "1"})
    assert result == {"text": "1", "enter": True}, result


def test_05_a_line_longer_than_any_game_reads_is_refused(playing: McpClient) -> None:
    """The cap is there because this drives a 1990s parser: a line that long
    is a mistake being made, not a sentence.

    A non-streaming tool reports a refusal in its own result rather than as a
    protocol error, so this reads the answer instead of catching an exception.
    """
    result = playing.call_tool("type_text", {"text": "x" * 500})
    assert "error" in result, result
    assert "256" in result["error"], result


def test_06_the_narration_is_captured(playing: McpClient) -> None:
    """Ween narrates its opening in text drawn to a surface, which is where
    the gob bridge listens."""
    seen: list[str] = []
    for _ in range(8):
        seen += [m["text"] for m in playing.state().get("messages", [])]
        if seen:
            break
        time.sleep(2)
    assert seen, "the opening said nothing at all"
