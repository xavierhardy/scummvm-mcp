"""
Integration test for the Ween: The Prophecy demo (Gob engine).

Ween is here for two things neither of the other gob demos shows.

The first is staying alive through video. The engine's pump point is
Util::processInput, which a video's own wait loop never reaches: it sits in
Util::delay. For the whole of Ween's opening the server used to stop
answering, and a skip issued inside it could not even end itself, because the
budget that closes a stream is only ever checked from a pump.

The second is the copy-protection screen. The demo will not start until it is
answered: a code is shown with a row of coloured cards, and the right card is
the one the manual's table gives for the code. An agent has no manual, so the
bridge answers it itself - and this checks that the opening runs straight
into the game. (With the game's copy_protection option on, the screen is left
alone; test_ween_protected.py covers that.)

No save support, so this is one ordered sequence on a fresh instance.
"""

import time

import pytest

from mcp_client import McpClient

pytestmark = [pytest.mark.xdist_group("ween")]

#: The demo's screens up to and including the copy-protection one.
_OPENING = {"intro", "intro0"}


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


def test_03_the_narration_is_captured(playing: McpClient) -> None:
    """Ween narrates its opening in text drawn to a surface, which is where
    the gob bridge listens."""
    # Given a wide budget on purpose: the narration is paced by the game, and
    # on a machine running several of these at once the line takes its time.
    seen: list[str] = []
    for _ in range(30):
        seen += [m["text"] for m in playing.state().get("messages", [])]
        if seen:
            break
        time.sleep(2)
    assert seen, "the opening said nothing at all"


def test_04_the_opening_runs_into_the_game_with_nothing_typed(
    playing: McpClient,
) -> None:
    """The copy-protection screen comes and goes by itself: skipping through
    the opening is all it takes to reach the game."""
    rooms: list[str] = []
    for _ in range(20):
        room = playing.state()["room"]["name"]
        if not rooms or rooms[-1] != room:
            rooms.append(room)
        if room not in _OPENING:
            return
        try:
            playing.skip()
        except RuntimeError as exc:
            if "nothing to skip" not in str(exc):
                raise
        time.sleep(2)
    raise AssertionError(f"the opening never gave way to the game: {rooms}")


def test_05_a_line_longer_than_any_game_reads_is_refused(playing: McpClient) -> None:
    """The cap is there because this drives a 1990s parser: a line that long
    is a mistake being made, not a sentence.

    A non-streaming tool reports a refusal in its own result rather than as a
    protocol error, so this reads the answer instead of catching an exception.
    """
    result = playing.call_tool("type_text", {"text": "x" * 500})
    assert "error" in result, result
    assert "256" in result["error"], result
