"""
Integration test for the Ween: The Prophecy demo (Gob engine).

Ween opens with minutes of video, and that is the thing worth testing here.
The engine's pump point is Util::processInput, which a video's own wait loop
never reaches - it sits in Util::delay - so for the whole of that opening the
server used to stop answering, and a skip issued inside it could not even end
itself: the budget that closes a stream is only ever checked from a pump.

So these tests are mostly about staying alive: the server answers while the
opening plays, and a skip returns rather than hanging.

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


def test_03_the_opening_gives_way_to_something(playing: McpClient) -> None:
    """Each skip cuts one piece of the opening short; the pause between them
    is what lets the next piece start, rather than spending the whole budget
    inside the first."""
    rooms: set[str] = set()
    for _ in range(20):
        room = (playing.state().get("room") or {}).get("name")
        if room:
            rooms.add(room)
        if len(rooms) > 1:
            return
        try:
            playing.skip()
        except RuntimeError as exc:
            if "nothing to skip" not in str(exc):
                raise
        time.sleep(2)
    assert len(rooms) > 1, f"the demo never moved on: {sorted(rooms)}"


def test_04_the_narration_is_captured(playing: McpClient) -> None:
    """Ween narrates its opening in text drawn to a surface, which is where
    the gob bridge listens."""
    seen: list[str] = []
    for _ in range(20):
        seen += [m["text"] for m in playing.state().get("messages", [])]
        if seen:
            break
        try:
            playing.skip()
        except RuntimeError:
            pass
        time.sleep(2)
    assert seen, "the opening said nothing at all"
