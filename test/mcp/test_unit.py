"""Unit tests for the pure-Python MCP test helpers (no ScummVM required).

These cover the small utilities that test_monkey.py (and the other suites) lean
on: the verb-binding meta function and the shared assertion helpers. They run
without launching a game, so they stay fast and need no fixtures.
"""

import pytest

from assertions import (
    assert_actor_spoke,
    assert_has_position,
    assert_inventory_contains,
    assert_inventory_does_not_contain,
    assert_message_contains,
    assert_message_present,
    assert_messages_contain,
    assert_messages_produced,
    assert_no_message_contains,
    assert_no_talkie_garbage,
    assert_room,
    assert_text_contains,
)
from utils import (
    bind_verb,
    choice_labels,
    find_choice_id,
    find_choice_id_containing,
    find_id,
    make_verbs,
    object_by_id,
    object_names,
    pathways,
)


class _FakeClient:
    """Records every act(...) call so make_verbs bindings can be asserted.

    Mirrors :meth:`utils.McpClient.act` (verb + up to two targets) so it
    structurally satisfies :class:`utils.VerbActor`.
    """

    def __init__(self) -> None:
        self.calls: list[tuple[str, tuple[str | int, ...]]] = []

    def act(
        self,
        verb: str,
        target1: str | int | None = None,
        target2: str | int | None = None,
    ) -> dict[str, object]:
        targets = tuple(t for t in (target1, target2) if t is not None)
        self.calls.append((verb, targets))
        return {"verb": verb, "targets": targets}


def test_make_verbs_binds_each_name() -> None:
    client = _FakeClient()
    pick_up, use = make_verbs(client, "pick_up", "use")
    pick_up("bowl")
    use("meat", "pot")
    assert client.calls == [("pick_up", ("bowl",)), ("use", ("meat", "pot"))]


def test_make_verbs_single_name_returns_one_tuple() -> None:
    give = bind_verb(_FakeClient(), "give")
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


# ---------------------------------------------------------------------------
# State/object helpers
# ---------------------------------------------------------------------------

_STATE = {
    "objects": [
        {"name": "door", "id": 10, "pathway": True},
        {"name": "key", "id": 20},
    ]
}


def test_find_id_returns_matching_id() -> None:
    assert find_id(_STATE, "key") == 20


def test_find_id_missing_returns_none() -> None:
    assert find_id(_STATE, "nope") is None


def test_object_by_id_returns_object() -> None:
    obj = object_by_id(_STATE, 10)
    assert obj is not None and obj["name"] == "door"


def test_object_by_id_missing_returns_none() -> None:
    assert object_by_id(_STATE, 999) is None


def test_object_names_returns_set() -> None:
    assert object_names(_STATE) == {"door", "key"}


def test_pathways_filters_to_pathway_objects() -> None:
    result = pathways(_STATE)
    assert len(result) == 1 and result[0]["name"] == "door"


# ---------------------------------------------------------------------------
# Dialog-choice helpers
# ---------------------------------------------------------------------------

_QUESTION = {
    "choices": [
        {"id": 1, "label": "To Henry's House"},
        {"id": 2, "label": "Cancel"},
    ]
}


def test_choice_labels_lists_labels() -> None:
    assert choice_labels(_QUESTION) == ["To Henry's House", "Cancel"]


def test_choice_labels_handles_none() -> None:
    assert choice_labels(None) == []


def test_find_choice_id_exact_match() -> None:
    assert find_choice_id(_QUESTION, "Cancel") == 2


def test_find_choice_id_no_match_returns_none() -> None:
    assert find_choice_id(_QUESTION, "henry") is None


def test_find_choice_id_containing_is_case_insensitive() -> None:
    assert find_choice_id_containing(_QUESTION, "henry") == 1


def test_find_choice_id_containing_no_match_returns_none() -> None:
    assert find_choice_id_containing(_QUESTION, "ramrod") is None


# ---------------------------------------------------------------------------
# Message-text assertion helpers
# ---------------------------------------------------------------------------

_MESSAGES = {"messages": [{"text": "Nice CANNON balls.", "actor": "guybrush"}]}


def test_assert_message_contains_is_case_insensitive() -> None:
    assert_message_contains(_MESSAGES, "cannon balls")


def test_assert_message_contains_missing_raises() -> None:
    pytest.raises(AssertionError, assert_message_contains, _MESSAGES, "anchor")


def test_assert_messages_contain_takes_a_list() -> None:
    assert_messages_contain([{"text": "hello"}], "ELLO")


def test_assert_no_message_contains_passes_when_absent() -> None:
    assert_no_message_contains(_MESSAGES, "anchor")


def test_assert_no_message_contains_raises_when_present() -> None:
    pytest.raises(AssertionError, assert_no_message_contains, _MESSAGES, "cannon")


def test_assert_text_contains_is_case_insensitive() -> None:
    assert_text_contains("ANCIENT secrets", "ancient")


def test_assert_text_contains_missing_raises() -> None:
    pytest.raises(AssertionError, assert_text_contains, "hello", "world")


def test_assert_actor_spoke_found() -> None:
    assert_actor_spoke(_MESSAGES, "guybrush")


def test_assert_actor_spoke_missing_raises() -> None:
    pytest.raises(AssertionError, assert_actor_spoke, _MESSAGES, "troll")


def test_assert_no_talkie_garbage_accepts_clean_text() -> None:
    assert_no_talkie_garbage([{"text": "A clean line."}])


def test_assert_no_talkie_garbage_rejects_nbsp() -> None:
    pytest.raises(AssertionError, assert_no_talkie_garbage, [{"text": "\u00a0xx"}])


def test_assert_no_talkie_garbage_rejects_letterless() -> None:
    pytest.raises(AssertionError, assert_no_talkie_garbage, [{"text": "1234"}])
