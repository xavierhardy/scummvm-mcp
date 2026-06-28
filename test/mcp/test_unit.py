"""Unit tests for the pure-Python MCP test helpers (no ScummVM required).

These cover the small utilities that test_monkey.py (and the other suites) lean
on: the verb-binding meta function and the shared assertion helpers. They run
without launching a game, so they stay fast and need no fixtures.
"""

import pytest

from assertions import (
    assert_has_position,
    assert_inventory_contains,
    assert_inventory_does_not_contain,
    assert_message_present,
    assert_messages_produced,
    assert_room,
)
from utils import make_verbs


class _FakeClient:
    """Records every act(...) call so make_verbs bindings can be asserted."""

    def __init__(self) -> None:
        self.calls: list = []

    def act(self, verb, *targets):
        self.calls.append((verb, targets))
        return {"verb": verb, "targets": targets}


def test_make_verbs_binds_each_name() -> None:
    client = _FakeClient()
    pick_up, use = make_verbs(client, "pick_up", "use")
    pick_up("bowl")
    use("meat", "pot")
    assert client.calls == [("pick_up", ("bowl",)), ("use", ("meat", "pot"))]


def test_make_verbs_single_name_returns_one_tuple() -> None:
    (give,) = make_verbs(_FakeClient(), "give")
    assert give("mint", "prisoner") == {"verb": "give", "targets": ("mint", "prisoner")}


def test_make_verbs_no_names_returns_empty_tuple() -> None:
    assert make_verbs(_FakeClient()) == ()


def test_assert_room_accepts_matching_room() -> None:
    assert_room({"room": {"id": 55}}, 55)


def test_assert_room_rejects_wrong_room() -> None:
    pytest.raises(AssertionError, assert_room, {"room": {"id": 52}}, 55)


def test_assert_room_rejects_missing_room() -> None:
    pytest.raises(AssertionError, assert_room, {}, 55)


def test_assert_has_position_accepts_xy() -> None:
    assert_has_position({"position": {"x": 1, "y": 2}})


def test_assert_has_position_rejects_missing_axis() -> None:
    pytest.raises(AssertionError, assert_has_position, {"position": {"x": 1}})


def test_assert_has_position_rejects_no_position() -> None:
    pytest.raises(AssertionError, assert_has_position, {})


def test_assert_message_present_finds_text() -> None:
    assert_message_present({"messages": [{"text": "hi"}, {"text": "bye"}]}, "bye")


def test_assert_message_present_missing_text() -> None:
    pytest.raises(
        AssertionError, assert_message_present, {"messages": [{"text": "hi"}]}, "bye"
    )


def test_assert_inventory_contains_found() -> None:
    assert_inventory_contains({"inventory_added": ["breath_mint"]}, "breath_mint")


def test_assert_inventory_contains_missing() -> None:
    pytest.raises(
        AssertionError,
        assert_inventory_contains,
        {"inventory_added": []},
        "breath_mint",
    )


def test_assert_inventory_does_not_contain_found() -> None:
    assert_inventory_does_not_contain(
        {"inventory_removed": ["hunk_o'_meat"]}, "hunk_o'_meat"
    )


def test_assert_inventory_does_not_contain_missing() -> None:
    pytest.raises(
        AssertionError,
        assert_inventory_does_not_contain,
        {"inventory_removed": []},
        "x",
    )


def test_assert_messages_produced_accepts_nonempty() -> None:
    assert_messages_produced({"messages": [{"text": "hi"}]})


def test_assert_messages_produced_rejects_empty() -> None:
    pytest.raises(AssertionError, assert_messages_produced, {"messages": []})
