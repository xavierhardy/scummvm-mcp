"""MCP integration tests for Broken Sword 1 (The Shadow of the Templars) demo.

Engine: sword1 (non-SCUMM). Save slot 1 sits in screen 1 — rue Jarry, outside
the Café de la Chandelle Verte — right after the intro. Data is looked up via
``SWORD1_DEMO_PATH`` and the tests skip when it is absent.

Broken Sword is a one-click game with no verb bar: the bridge exposes a fixed
verb set and maps everything but ``look_at`` (right click) onto the interaction
(left click) path. Coordinates are world/compact coordinates throughout.
"""

from __future__ import annotations

import pytest

from sword1_helpers import (
    SWORD_VERBS,
    any_floor_point,
    wait_for_can_act,
)
from utils import McpClient


def test_01_state_reports_screen_and_objects(sword1_client: McpClient) -> None:
    state = wait_for_can_act(sword1_client)
    assert state["room"]["id"] == 1
    assert state["room"].get("name") == "rue_jarry"
    assert state["can_act"] is True
    assert state["objects"], "expected clickable objects on screen 1"
    for obj in state["objects"]:
        assert obj["name"]
        assert obj["kind"] in {"floor", "character", "object", "hotspot"}
        assert "position" in obj and "x" in obj["position"] and "y" in obj["position"]
        assert len(obj["box"]) == 4


def test_02_player_position_is_reported(sword1_client: McpClient) -> None:
    state = wait_for_can_act(sword1_client)
    pos = state["position"]
    # World/compact coordinates. George settles on rue Jarry near where the save
    # left him (world ~487, 413), but the exact resting spot varies with a short
    # post-load idle, so assert a plausible on-floor position, not an exact one.
    assert isinstance(pos["x"], int) and isinstance(pos["y"], int)
    assert 200 < pos["x"] < 900 and 300 < pos["y"] < 560
    assert "dir" in pos


def test_03_verbs_are_the_fixed_sword_set(sword1_client: McpClient) -> None:
    state = wait_for_can_act(sword1_client)
    assert set(state["verbs"]) == SWORD_VERBS


def test_04_look_at_scene_object_returns_changes(sword1_client: McpClient) -> None:
    state = wait_for_can_act(sword1_client)
    # Pick any named, mouseable object and look at it. The result is a
    # structured state-change object; the interaction may move George, speak, or
    # even change screen (some hotspots are exits), so assert only that the call
    # completes and the server stays responsive.
    target = next((o["name"] for o in state["objects"] if o["kind"] != "floor"), None)
    if target is None:
        pytest.skip("no non-floor object to look at on this screen")
    result = sword1_client.act("look_at", target)
    assert isinstance(result, dict)
    assert "room" in sword1_client.state()


def test_05_walk_moves_george(sword1_client: McpClient) -> None:
    state = wait_for_can_act(sword1_client)
    start = state["position"]
    point = any_floor_point(sword1_client)
    if point is None:
        pytest.skip("no floor compact found on this screen")
    # Aim well away from the current position so movement is unambiguous.
    tx = start["x"] + 120 if point[0] >= start["x"] else start["x"] - 120
    result = sword1_client.walk(tx, start["y"])
    assert isinstance(result, dict)
    after = wait_for_can_act(sword1_client)["position"]
    moved = abs(after["x"] - start["x"]) + abs(after["y"] - start["y"])
    assert moved > 15, f"George did not move (start={start}, after={after})"


def test_06_walk_outside_floor_errors(sword1_client: McpClient) -> None:
    wait_for_can_act(sword1_client)
    # Nothing is walkable at the very top-left corner of world space.
    with pytest.raises(RuntimeError):
        sword1_client.walk(-500, -500)
    # The server must still answer afterwards (the screen is not asserted: the
    # demo can advance George between the fixture load and here).
    assert "room" in sword1_client.state()


def test_07_raw_id_and_fallback_names_resolve(sword1_client: McpClient) -> None:
    state = wait_for_can_act(sword1_client)
    obj = next((o for o in state["objects"] if o["kind"] != "floor"), None)
    if obj is None:
        pytest.skip("no non-floor object on this screen")
    # Both the fallback/authored name and the raw numeric id name the same
    # object. Resolution is exercised here without dispatching an interaction
    # (which could change screen): a look at each form must not raise "unknown
    # target". A fresh save is loaded per test, so the object is present.
    for target in (obj["name"], obj["id"]):
        try:
            sword1_client.act("look_at", target)
        except RuntimeError as exc:
            assert "unknown target" not in str(exc)
        wait_for_can_act(sword1_client)


def test_08_unknown_target_errors_cleanly(sword1_client: McpClient) -> None:
    wait_for_can_act(sword1_client)
    with pytest.raises(RuntimeError):
        sword1_client.act("look_at", "definitely_not_a_thing")
    # No crash: the game is still there.
    assert sword1_client.state()["room"]["id"] == 1


def test_09_debug_dumps_compacts(sword1_client: McpClient) -> None:
    wait_for_can_act(sword1_client)
    dump = sword1_client.call_tool("debug", {"compacts": True})
    assert dump["compacts"], "expected a non-empty compact dump on screen 1"
    for cpt in dump["compacts"]:
        assert "id" in cpt and "box" in cpt and "type" in cpt
        assert "mouse_click" in cpt  # the field used to author names


def test_10_debug_system_reports_engine_state(sword1_client: McpClient) -> None:
    wait_for_can_act(sword1_client)
    sysd = sword1_client.call_tool("debug", {"system": True})["system"]
    assert sysd["screen"] == 1
    assert sysd["can_act"] is True
    assert "mouse_status" in sysd and "player_x" in sysd
