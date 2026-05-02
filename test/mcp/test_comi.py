"""Integration tests for Curse of Monkey Island demo (SCUMM V8)."""

import pytest

from utils import McpClient


def test_01_comi_state_reachable(comi_client: McpClient) -> None:
    state = comi_client.state()
    assert state.get("room") is not None
    assert isinstance(state.get("objects", []), list)


def test_02_comi_has_dialog_or_can_trigger_it(comi_client: McpClient) -> None:
    state = comi_client.state()
    question = state.get("question")

    if question is None:
        talkers = [
            obj["name"]
            for obj in state.get("objects", [])
            if "talk to" in obj.get("compatible_verbs", []) and not obj.get("pathway")
        ]
        if not talkers:
            pytest.skip("No talk_to target in current room")
        result = comi_client.act("talk_to", talkers[0])
        question = result.get("question")

    if question is None:
        pytest.skip("No dialog question available in current scene")

    choices = question.get("choices", [])
    assert choices, f"Expected choices, got: {question}"
    # V8: choices are sentence-like labels, not generic Topic N placeholders.
    assert any(" " in c.get("label", "") for c in choices)
    assert not any(c.get("label", "").lower().startswith("topic ") for c in choices)
