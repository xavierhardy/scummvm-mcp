"""
Integration test for the Discworld II demo (Tinsel engine, V2).

The sequel runs on the same engine and plays the same way as its predecessor
(see ``test_dw1.py`` for what that means and how the bridge drives it), so this
suite checks the same contract without pinning it to one scene's furniture: the
demo is a guided slice of the game and drops the player somewhere different
from the first game's bedroom. What is asserted here is what holds for any
scene — the three verbs, named targets with a spot to point at, an action that
reports what it changed, a walk that ends where it says it does — plus the one
thing that really is different, the engine version the snapshot is built from.

Like the first game's demo there is no save to load, so the whole run is one
ordered sequence on a single fresh instance, skipping the intro first.

Note on data: only the Windows demo runs under ScummVM. The DOS demo
(dw2-dos-demo-en) is flagged unsupported by the engine — its scripts use a
library-call numbering the interpreter does not have a table for — so it will
not start, and `dw2-demo` should not be pointed at it.
"""

import time

import pytest

from mcp_client import McpClient

# One ordered walkthrough on a single instance — pin it to one xdist worker.
pytestmark = [pytest.mark.xdist_group("dw2")]


def _wait_idle(client: McpClient, tries: int = 20) -> dict:
    """The state once the game is accepting input again."""
    for _ in range(tries):
        state = client.state()
        if state.get("can_act"):
            return state
        time.sleep(0.5)
    raise AssertionError("the game never went back to accepting input")


def _position(client: McpClient) -> dict:
    return client.state()["position"]


def _scenery(client: McpClient) -> list[dict]:
    """The fixed things in the scene — characters walk off, scenery does not."""
    return [obj for obj in client.state()["objects"] if obj["kind"] == "scenery"]


def _skip_intro(client: McpClient, max_skips: int = 25) -> dict:
    """Escape through the intro until a playable scene is up; return its state."""
    for _ in range(max_skips):
        state = client.state()
        if state.get("can_act") and state.get("objects"):
            return state
        try:
            client.skip()
        except RuntimeError as exc:  # a skip with nothing to cut short
            if "starting up" not in str(exc):
                raise
    raise AssertionError("the intro never gave way to a playable scene")


@pytest.fixture(scope="session")
def playing(dw2_client: McpClient) -> McpClient:
    """The demo, past the intro and accepting input."""
    _skip_intro(dw2_client)
    return dw2_client


def test_01_dw2_state_describes_the_scene(playing: McpClient) -> None:
    """The snapshot is the scene as the game labels it, not as pixels."""
    state = playing.state()

    assert state["room"]["name"], f"the scene has no name: {state['room']}"
    assert state["can_act"] is True, f"the game is not accepting input: {state}"
    assert state["verbs"] == ["walk_to", "look_at", "use"], f"unexpected verbs: {state}"

    assert state["objects"], f"nothing to point at in this scene: {state}"
    for obj in state["objects"]:
        assert obj["name"], f"an unnamed target: {obj}"
        assert obj["kind"] in ("character", "scenery"), obj
        # Scene coordinates, not screen ones: a scene here is often several
        # screens wide, so only the vertical extent is bounded.
        assert obj["x"] >= 0 and 0 <= obj["y"] < 480, f"outside the scene: {obj}"

    # Names are unique, so an agent can echo one back unambiguously.
    names = [obj["name"] for obj in state["objects"]]
    assert len(names) == len(set(names)), f"two targets share a name: {names}"

    assert state["position"]["x"] > 0 and state["position"]["y"] > 0, state


def test_02_dw2_look_at_reports_what_is_said(playing: McpClient) -> None:
    """`look_at` is the right click: the character examines the thing aloud."""
    for obj in playing.state()["objects"]:
        result = playing.act("look_at", obj["name"])
        lines = result.get("messages", [])
        if lines:
            assert all(line["text"] for line in lines), lines
            assert playing.state()["messages"] == [], "the lines were reported twice"
            return
    pytest.fail("nothing in this scene had anything to say")


def test_03_dw2_walk_moves_the_character(playing: McpClient) -> None:
    """walk() takes a point and the call waits for the walk to finish."""
    _wait_idle(playing)
    before = _position(playing)

    # Aim along the band the character is already standing in, at the x of
    # something in the scene: not every point of a scene is walkable, and where
    # he stands is by definition part of a path. Several are tried because he
    # may already be at the first one.
    for obj in _scenery(playing):
        result = playing.walk(obj["x"], before["y"])
        assert "position" in result, f"walk reported no position: {result}"
        after = _position(playing)
        if after != before:
            assert result["position"] == after, (
                f"walk returned {result['position']} but he stands at {after}"
            )
            return
        _wait_idle(playing)
    pytest.fail(f"he never moved from {before}")


def test_04_dw2_act_rejects_what_it_cannot_do(playing: McpClient) -> None:
    """The errors say what to do instead, rather than failing silently.

    They are also decided before the game's own readiness is: a target that
    does not exist is wrong whatever the game is doing, and saying so is more
    use than "not right now"."""
    with pytest.raises(RuntimeError, match="unknown verb"):
        playing.act("juggle", _scenery(playing)[0]["name"])
    with pytest.raises(RuntimeError, match="unknown target"):
        playing.act("look_at", "hippopotamus")
    with pytest.raises(RuntimeError, match="must be an item you are carrying"):
        playing.act("use", _scenery(playing)[0]["name"], "hippopotamus")
    with pytest.raises(RuntimeError, match="no conversation question is pending"):
        playing.call_tool("answer", {"id": 1})


def test_05_dw2_debug_reads_the_engine(playing: McpClient) -> None:
    """The debug tool reports what the engine thinks, for diagnosis."""
    _wait_idle(playing)
    system = playing.call_tool("debug")["system"]

    assert system["version"] == 2, f"not the version this demo runs on: {system}"
    assert system["scene_name"] == playing.state()["room"]["name"], system
    assert system["control"] is True, system

    objects = playing.call_tool("debug", {"objects": True, "system": False})["objects"]
    assert objects, f"the scene has no pointable things: {objects}"


def test_06_dw2_screenshot_returns_the_frame(playing: McpClient) -> None:
    """A screenshot comes back as an image, at the sequel's own resolution."""
    structured = playing.call_tool_raw("screenshot")["structuredContent"]
    assert structured["width"] == 640, f"unexpected frame size: {structured}"
    assert structured["height"] in (432, 480), f"unexpected frame size: {structured}"
