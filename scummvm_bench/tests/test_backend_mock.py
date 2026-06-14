"""MockBackend scripted responses, sequential consumption, and rejections."""

import pytest
from mock_harness import monkey_backend

from scummvm_bench.backend import BackendInvalidRequest, MockBackend, ScriptStep


def test_initial_state_room() -> None:
    backend = monkey_backend()
    assert backend.state()["room"] == {"id": 55}


def test_walk_returns_troll_message_without_changing_room() -> None:
    backend = monkey_backend()
    result = backend.call("walk", {"x": 120, "y": 132})
    assert result["messages"][0]["text"] == "None shall pass!"
    assert backend.state()["room"] == {"id": 55}


def test_sequential_consumption_of_same_key() -> None:
    backend = monkey_backend()
    assert (
        backend.call("act", {"verb": "walk_to", "target1": "door"})["room_changed"]
        == 52
    )
    assert backend.state()["room"] == {"id": 52}
    assert (
        backend.call("act", {"verb": "walk_to", "target1": "door"})["room_changed"]
        == 51
    )
    assert (
        backend.call("act", {"verb": "walk_to", "target1": "door"})["room_changed"]
        == 58
    )
    # exhausted -> last matching step is replayed
    assert (
        backend.call("act", {"verb": "walk_to", "target1": "door"})["room_changed"]
        == 58
    )


def test_unknown_call_is_invalid_request() -> None:
    backend = monkey_backend()
    with pytest.raises(BackendInvalidRequest):
        backend.call("act", {"verb": "frobnicate"})


def test_inventory_tracking() -> None:
    backend = MockBackend(
        [
            ScriptStep("act", {"verb": "take"}, {"inventory_added": ["sword"]}),
            ScriptStep("act", {"verb": "drop"}, {"inventory_removed": ["sword"]}),
        ],
        initial_room=1,
    )
    backend.call("act", {"verb": "take"})
    assert backend.state()["inventory"] == ["sword"]
    backend.call("act", {"verb": "drop"})
    assert backend.state()["inventory"] == []


def test_state_tool_call_returns_snapshot() -> None:
    backend = monkey_backend()
    assert backend.call("state", {}) == backend.state()


def test_start_stop_are_noops() -> None:
    backend = monkey_backend()
    backend.start()
    backend.stop()
    assert backend.state()["room"] == {"id": 55}
