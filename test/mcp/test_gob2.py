"""
Integration test for the Gobliins 2 interactive demo (Gob engine).

Three gob-engine games are covered by one bridge and they are not alike.
Gobliiins has no hover text and one screen-wide click zone, so its snapshot is
built from the engine's own tables. Gobliins 2 is the other shape - the one
Woodruff has - with a hotspot per object, each naming itself in the status bar
when the cursor rests on it. This checks that the second shape works for a
game the bridge was not originally written for: that the hotspots come back,
that the sweep puts names on them, and that a click reaches the game.

No save support, so this is one ordered sequence on a fresh instance.
"""

import time

import pytest

from mcp_client import McpClient

pytestmark = [pytest.mark.xdist_group("gob2")]


def _playable(client: McpClient, tries: int = 30, want: int = 3) -> dict:
    """Wait for a screen with hotspots on it.

    The pause matters: the opening is a run of videos, and asking again
    immediately just spends the whole budget inside the first one.
    """
    for _ in range(tries):
        state = client.state()
        if len(state.get("objects") or []) >= want:
            return state
        try:
            client.skip()
        except RuntimeError as exc:
            if "nothing to skip" not in str(exc) and "starting up" not in str(exc):
                raise
        time.sleep(2)
    raise AssertionError("the demo never showed a screen with hotspots on it")


@pytest.fixture(scope="session")
def playing(gob2_client: McpClient) -> McpClient:
    _playable(gob2_client)
    return gob2_client


def test_01_the_screen_has_hotspots(playing: McpClient) -> None:
    state = playing.state()
    assert state["objects"], f"nothing to point at: {state}"
    for obj in state["objects"]:
        assert obj["x"] >= 0 and obj["y"] >= 0, f"{obj} has nowhere to click"


def test_02_the_sweep_names_what_the_game_names(playing: McpClient) -> None:
    """A hotspot's name is the label the game paints for it, and the sweep is
    what collects those. Until it has run, a hotspot is only a number."""
    labelled: set[str] = set()
    for _ in range(40):
        state = playing.state()
        labelled |= {o["name"] for o in state["objects"] if o.get("label")}
        if labelled and not state.get("naming_pending"):
            break
        time.sleep(1)
    # How many a screen has is the screen's business - the first one here is
    # mostly the two goblins. What matters is that the sweep ran and that what
    # it produced are names, not numbers.
    assert labelled, "the sweep named nothing at all"
    for name in labelled:
        assert name == name.lower() and " " not in name, name


def test_03_the_verbs_are_the_two_this_engine_has(playing: McpClient) -> None:
    assert playing.state()["verbs"] == ["interact", "use"]


def test_04_a_target_that_is_not_here_is_refused(playing: McpClient) -> None:
    with pytest.raises(RuntimeError) as excinfo:
        playing.act(verb="interact", target1="not_a_thing_here")
    assert "not_a_thing_here" in str(excinfo.value)


def test_05_a_stream_opens_and_closes(playing: McpClient) -> None:
    """What matters is that a streaming call comes back at all.

    Where it lands is the screen's business: a point covered by a hotspot is
    not open ground, and the game refusing to walk there is a correct answer -
    what would be wrong is a stream that never closes.
    """
    try:
        result = playing.walk(160, 140)
    except RuntimeError as exc:
        assert "walk" in str(exc).lower(), exc
        return
    assert isinstance(result, dict), result
