"""MCP integration tests for Beneath a Steel Sky.

Engine: sky (non-SCUMM). Save slot 1 sits on screen 0 — the top of the
recycling-plant walkway, right after the intro, at the start of the
walkthrough's first section ("Out With the Trash"). Data is looked up via
``SKY_PATH`` and the tests skip when it is absent.

BASS is a two-button game with no verb bar: the bridge exposes a fixed verb
set; ``look_at`` maps onto the examine button and every other verb onto the
action button. Inventory interactions are played out through the game's own
top icon bar by the bridge's click machine. Coordinates are game/compact
coordinates throughout (visible area 128..447 x 136..327).
"""

from __future__ import annotations

import pytest

from assertions import assert_message_contains
from sky_helpers import (
    SKY_VERBS,
    activate_joey,
    any_floor_point,
    wait_for_can_act,
)
from utils import McpClient


def test_01_state_reports_screen_and_objects(sky_client: McpClient) -> None:
    state = wait_for_can_act(sky_client)
    assert state["room"]["id"] == 0
    assert state["can_act"] is True
    names = {o["name"] for o in state["objects"]}
    assert {"rung", "door", "stairs", "notice"} <= names
    for obj in state["objects"]:
        assert obj["name"]
        assert obj["kind"] in {"floor", "character", "hotspot"}
        assert "position" in obj and "x" in obj["position"] and "y" in obj["position"]
        assert len(obj["box"]) == 4


def test_02_player_position_is_reported(sky_client: McpClient) -> None:
    state = wait_for_can_act(sky_client)
    pos = state["position"]
    # Game/compact coordinates: Foster starts on the upper walkway.
    assert isinstance(pos["x"], int) and isinstance(pos["y"], int)
    assert 128 < pos["x"] < 448 and 136 < pos["y"] < 328
    assert "dir" in pos


def test_03_verbs_are_the_fixed_sky_set(sky_client: McpClient) -> None:
    state = wait_for_can_act(sky_client)
    assert set(state["verbs"]) == SKY_VERBS


def test_04_inventory_lists_the_circuit_board(sky_client: McpClient) -> None:
    state = wait_for_can_act(sky_client)
    names = [item["name"] for item in state["inventory"]]
    # Foster starts the game carrying Joey's circuit board.
    assert "circuit_board" in names


def test_05_look_at_speaks_a_description(sky_client: McpClient) -> None:
    wait_for_can_act(sky_client)
    result = sky_client.act("look_at", "rung")
    # Foster examines the loose rung: "I could make USE of that."
    assert_message_contains(result, "use")


def test_06_walk_moves_foster(sky_client: McpClient) -> None:
    state = wait_for_can_act(sky_client)
    start = state["position"]
    point = any_floor_point(state)
    if point is None:
        pytest.skip("no floor object on this screen")
    tx = start["x"] + 90 if point[0] >= start["x"] else start["x"] - 90
    result = sky_client.walk(tx, start["y"])
    assert isinstance(result, dict)
    after = wait_for_can_act(sky_client)["position"]
    moved = abs(after["x"] - start["x"]) + abs(after["y"] - start["y"])
    assert moved > 15, f"Foster did not move (start={start}, after={after})"


def test_07_walk_outside_floor_errors(sky_client: McpClient) -> None:
    wait_for_can_act(sky_client)
    # Nothing is walkable at the top-left corner of the game area.
    with pytest.raises(RuntimeError):
        sky_client.walk(130, 140)
    # The server must still answer afterwards.
    assert "room" in sky_client.state()


def test_08_unknown_target_errors_cleanly(sky_client: McpClient) -> None:
    wait_for_can_act(sky_client)
    with pytest.raises(RuntimeError):
        sky_client.act("look_at", "definitely_not_a_thing")
    assert sky_client.state()["room"]["id"] == 0


def test_09_pick_up_adds_the_metal_bar(sky_client: McpClient) -> None:
    wait_for_can_act(sky_client)
    result = sky_client.act("pick_up", "rung")
    assert "metal_bar" in result.get("inventory_added", [])
    # The rung vanished from the scene.
    changed = {c["name"]: c["new_state"] for c in result.get("objects_changed", [])}
    assert changed.get("rung") == "hidden"
    names = {o["name"] for o in sky_client.state()["objects"]}
    assert "rung" not in names


def test_10_debug_reports_engine_state(sky_client: McpClient) -> None:
    wait_for_can_act(sky_client)
    sysd = sky_client.call_tool("debug", {"system": True})["system"]
    assert sysd["screen"] == 0
    assert sysd["can_act"] is True
    assert "mouse_status" in sysd and "foster_x" in sysd
    dump = sky_client.call_tool("debug", {"compacts": True, "system": False})
    assert dump["compacts"], "expected a non-empty compact dump on screen 0"
    for cpt in dump["compacts"]:
        assert "id" in cpt and "box" in cpt and "name" in cpt


@pytest.mark.slow
def test_11_force_the_door_with_the_bar(sky_client: McpClient) -> None:
    wait_for_can_act(sky_client)
    sky_client.act("pick_up", "rung")
    # "use it on the door at the right end": the guard hears the noise, charges
    # up, and Foster steps out onto the overhang — a long scripted sequence
    # ending on screen 1. The bridge plays the icon-bar clicks itself.
    result = sky_client.act("use", "metal_bar", "door")
    assert result.get("room_changed") == 1
    state = wait_for_can_act(sky_client, timeout=120.0)
    assert state["room"]["id"] == 1


@pytest.mark.slow
def test_12_joey_dialog_chooser(sky_client: McpClient) -> None:
    wait_for_can_act(sky_client)
    # Play the opening through to fitting Joey's circuit board into the shell.
    result = activate_joey(sky_client)
    assert "circuit_board" in result.get("inventory_removed", [])
    state = wait_for_can_act(sky_client)
    names = {o["name"]: o["kind"] for o in state["objects"]}
    assert names.get("joey") == "character"

    # Talking to Joey opens the text chooser.
    result = sky_client.act("talk_to", "joey")
    question = result.get("question") or {}
    labels = [c["label"] for c in question.get("choices", [])]
    assert len(labels) >= 2, f"expected chooser options, got {result}"

    # Answering speaks the chosen line and re-offers the chooser.
    result = sky_client.answer(1)
    follow_up = result.get("question") or sky_client.state().get("question")
    assert follow_up and follow_up.get("choices")

    # The last option ("Forget it...") ends the conversation.
    last_id = len(follow_up["choices"])
    sky_client.answer(last_id)
    state = wait_for_can_act(sky_client)
    assert "question" not in state
