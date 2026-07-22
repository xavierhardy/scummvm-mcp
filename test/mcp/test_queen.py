"""MCP integration tests for Flight of the Amazon Queen.

Engine: queen (non-SCUMM). Save slot 1 sits in room 70 ("d1") — Joe locked in
the hotel room, right after the intro, at the start of the walkthrough's first
section. Data is looked up via ``QUEEN_PATH`` and the tests skip when it is
absent. The game also needs ``queen.tbl``, served from the repository's
``dists/engine-data`` via the launcher's ``extrapath``.

FOTAQ is a verb-panel game, so ``act`` maps one-to-one onto the game's own
command machinery (open/close/move/give/use/pick_up/talk_to/look_at plus
walk_to). Dialogues surface as ``question`` and are answered with ``answer``.
"""

from __future__ import annotations

import pytest

from assertions import assert_message_contains
from queen_helpers import (
    QUEEN_VERBS,
    descend_to_basement,
    rig_sheet_rope,
    wait_for_can_act,
)
from utils import McpClient


def test_01_state_reports_room_and_objects(queen_client: McpClient) -> None:
    state = wait_for_can_act(queen_client)
    assert state["room"]["id"] == 70
    assert state["room"].get("name") == "d1"
    assert state["can_act"] is True
    names = {o["name"] for o in state["objects"]}
    assert {"chest", "door", "radiator", "curtain_cord"} <= names
    for obj in state["objects"]:
        assert obj["name"]
        assert obj["kind"] in {"object", "person"}


def test_02_verbs_are_the_panel_set(queen_client: McpClient) -> None:
    state = wait_for_can_act(queen_client)
    assert set(state["verbs"]) == QUEEN_VERBS


def test_03_inventory_lists_the_start_items(queen_client: McpClient) -> None:
    state = wait_for_can_act(queen_client)
    names = [item["name"] for item in state["inventory"]]
    # Joe starts with his baseball bat and his journal.
    assert "bat" in names and "journal" in names


def test_04_look_at_speaks_a_description(queen_client: McpClient) -> None:
    wait_for_can_act(queen_client)
    result = queen_client.act("look_at", "chest")
    # "Wow, it could be a pirate chest full of treasure! ..."
    assert_message_contains(result, "pirate")


def test_05_move_curtain_reveals_the_wig(queen_client: McpClient) -> None:
    wait_for_can_act(queen_client)
    result = queen_client.act("move", "curtain_cord")
    changed = {c["name"]: c["new_state"] for c in result.get("objects_changed", [])}
    assert changed.get("wig") == "visible"
    result = queen_client.act("pick_up", "wig")
    assert "wig" in result.get("inventory_added", [])


def test_05b_walk_moves_joe(queen_client: McpClient) -> None:
    state = wait_for_can_act(queen_client)
    start = state["position"]
    # The hotel room's walkable band sits around y 90..150.
    tx = 180 if start["x"] > 220 else 260
    result = queen_client.walk(tx, 125)
    assert isinstance(result, dict)
    after = wait_for_can_act(queen_client)["position"]
    moved = abs(after["x"] - start["x"]) + abs(after["y"] - start["y"])
    assert moved > 15, f"Joe did not move (start={start}, after={after})"


def test_06_unknown_target_errors_cleanly(queen_client: McpClient) -> None:
    wait_for_can_act(queen_client)
    with pytest.raises(RuntimeError):
        queen_client.act("look_at", "definitely_not_a_thing")
    assert queen_client.state()["room"]["id"] == 70


def test_07_debug_reports_engine_state(queen_client: McpClient) -> None:
    wait_for_can_act(queen_client)
    sysd = queen_client.call_tool("debug", {"system": True})["system"]
    assert sysd["room"] == 70
    assert sysd["can_act"] is True
    assert "joe_x" in sysd and "cutaway" in sysd
    dump = queen_client.call_tool("debug", {"objects": True, "system": False})
    assert dump["objects"], "expected object records for room 70"
    for obj in dump["objects"]:
        assert "rel" in obj and "abs" in obj and "name" in obj


@pytest.mark.slow
def test_08_sheet_rope_escape(queen_client: McpClient) -> None:
    wait_for_can_act(queen_client)
    # Walkthrough: pick up the two sheets, tie them together, tie the rope to
    # the radiator, then climb down the laundry chute to the basement.
    result = rig_sheet_rope(queen_client)
    assert "sheet_rope" in result.get("inventory_removed", [])
    result = queen_client.act("use", "radiator_with_sheet_rope")
    assert result.get("room_changed") == 71
    state = wait_for_can_act(queen_client)
    assert state["room"]["id"] == 71


@pytest.mark.slow
def test_09_bellboy_key_dialogue(queen_client: McpClient) -> None:
    wait_for_can_act(queen_client)
    # Down to the basement, grab the crowbar, then up to the bellboy's floor.
    descend_to_basement(queen_client)
    wait_for_can_act(queen_client)
    queen_client.act("pick_up", "box_of_crowbars")
    result = queen_client.act("walk_to", "door_2")
    assert result.get("room_changed") == 73

    # The key belongs to Miss Lola; the bellboy stops a straight grab.
    result = queen_client.act("pick_up", "key")
    assert_message_contains(result, "Lola")

    # Talk him into it: about the key → borrow it (refused) → about the key
    # again → "I'm Lola's friend" (accepted; the dialogue ends by itself).
    result = queen_client.act("talk_to", "bellboy")
    labels = [c["label"] for c in result["question"]["choices"]]
    assert any("key" in label for label in labels), labels
    result = queen_client.answer(_choice(result, "key"))
    result = queen_client.answer(_choice(result, "borrow"))
    result = queen_client.answer(_choice(result, "key"))
    result = queen_client.answer(_choice(result, "friend"))
    joined = " ".join(m.get("text", "") for m in result.get("messages", []))
    assert "take the key" in joined, joined

    # Now the key is Joe's for the taking.
    result = queen_client.act("pick_up", "key")
    assert "key" in result.get("inventory_added", [])


def _choice(result: dict, needle: str) -> int:
    """The id of the first pending choice whose label contains *needle*."""
    question = result.get("question") or {}
    for choice in question.get("choices", []):
        if needle.lower() in choice["label"].lower():
            return choice["id"]
    raise AssertionError(f"no choice containing {needle!r} in {question}")
