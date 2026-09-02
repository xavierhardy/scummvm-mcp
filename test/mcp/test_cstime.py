"""
Integration test for Where in Time is Carmen Sandiego? (Mohawk engine).

A pointer game with no verb bar and no verbs at all: a scene is a picture with
regions marked on it, and the whole vocabulary is clicking one. What a click
does is the region's own business - some are ways out, some start a
conversation, some pick something up - so the bridge offers one verb and says
so, rather than inventing a set that would go nowhere.

Names come from the one place a player ever sees them: the line the game writes
when the cursor rests on a region. Each region carries the number of that line
and the case holds the lines, so nothing has to be swept for with a cursor.

No save support, so this is one ordered sequence on a fresh instance.
"""

import time

import pytest

from mcp_client import McpClient

pytestmark = [
    pytest.mark.xdist_group("cstime"),
    # The bridge works and the harness now starts the game (ScummVM's own
    # "not yet fully supported" dialog, which used to block the launch before
    # the engine existed, is answered from the ini). The game itself is what
    # is skipped: this one is flagged unstable upstream, so what it does is
    # not something to hold the bridge to. Drop the mark to exercise it.
    pytest.mark.skip(reason="the game is flagged unstable by its own engine"),
]


def _playable(client: McpClient, tries: int = 20) -> dict:
    for _ in range(tries):
        state = client.state()
        if state.get("objects"):
            return state
        try:
            client.skip()
        except RuntimeError as exc:
            if "starting up" not in str(exc) and "nothing to skip" not in str(exc):
                raise
        time.sleep(2)
    raise AssertionError("the demo never showed a scene with anything in it")


@pytest.fixture(scope="session")
def playing(cstime_client: McpClient) -> McpClient:
    _playable(cstime_client)
    return cstime_client


def test_01_the_scene_has_things_to_point_at(playing: McpClient) -> None:
    state = playing.state()
    assert state["objects"], f"nothing to point at: {state}"
    assert isinstance(state["room"]["id"], int)


def test_02_things_are_named_the_way_the_game_names_them(playing: McpClient) -> None:
    """The name is the game's own rollover line, folded into an identifier."""
    names = {o["name"] for o in playing.state()["objects"]}
    for name in names:
        assert name == name.lower(), f"{name} was never folded"
        assert " " not in name and "'" not in name, name
    # Some of them are real words rather than the numbered fallback, which is
    # the whole point of reading the rollover text at all.
    assert any(not n.startswith("hotspot_") for n in names), sorted(names)


def test_03_everything_has_somewhere_to_click(playing: McpClient) -> None:
    for obj in playing.state()["objects"]:
        assert obj["x"] > 0 and obj["y"] > 0, f"{obj} has nowhere to click"


def test_04_there_is_one_verb_and_it_says_so(playing: McpClient) -> None:
    assert playing.state()["verbs"] == ["use"]


def test_05_a_verb_from_another_game_is_refused_with_why(playing: McpClient) -> None:
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="look_at", target1=playing.state()["objects"][0]["name"])
    message = str(excinfo.value)
    assert "look_at" in message
    assert "no verbs" in message


def test_06_a_target_that_is_not_here_is_refused_with_the_ones_that_are(
    playing: McpClient,
) -> None:
    here = playing.state()["objects"][0]["name"]
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="use", target1="a_thing_that_is_not_here")
    message = str(excinfo.value)
    assert "a_thing_that_is_not_here" in message
    assert here in message


def test_07_clicking_something_comes_back(playing: McpClient) -> None:
    """What a click does is the game's business; what matters here is that the
    stream opens and closes rather than hanging."""
    name = playing.state()["objects"][0]["name"]
    result = playing.act(verb="use", target1=name)
    assert "room" in result, result
