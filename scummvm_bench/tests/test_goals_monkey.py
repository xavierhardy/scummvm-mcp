"""Goal engine predicates and the Monkey Island goal set."""

import pytest

from scummvm_bench.goals import get_goal_set
from scummvm_bench.goals.engine import (
    Goal,
    GoalEvent,
    GoalSet,
    all_of,
    any_of,
    in_room,
    on_call,
    on_inventory_added,
    on_inventory_removed,
    on_message_contains,
    on_object_changed,
    on_question_appeared,
    on_room_changed,
)


def ev(
    tool: str = "act",
    args: dict | None = None,
    result: dict | None = None,
    room: int | None = None,
) -> GoalEvent:
    state = {"room": {"id": room}} if room is not None else {}
    return GoalEvent(tool, args or {}, result, state, result is not None)


def test_on_room_changed() -> None:
    assert on_room_changed(52)(ev(result={"room_changed": 52})) is True
    assert on_room_changed(52)(ev(result={"room_changed": 51})) is False
    assert on_room_changed(52)(ev(result=None)) is False


def test_on_inventory_added_normalises_tokens() -> None:
    pred = on_inventory_added("breath_mint")
    assert pred(ev(result={"inventory_added": ["breath_mint"]})) is True
    assert pred(ev(result={"inventory_added": ["BREATH_MINT@@@"]})) is True
    assert pred(ev(result={"inventory_added": ["sword"]})) is False


def test_on_inventory_removed() -> None:
    pred = on_inventory_removed("breath_mint")
    assert pred(ev(result={"inventory_removed": ["breath_mint"]})) is True
    assert pred(ev(result={"inventory_removed": []})) is False


def test_on_object_changed() -> None:
    pred = on_object_changed("door")
    assert pred(ev(result={"objects_changed": [{"name": "door"}]})) is True
    assert pred(ev(result={"objects_changed": [{"name": "mug"}]})) is False


def test_on_message_contains_is_case_insensitive() -> None:
    pred = on_message_contains("None shall pass")
    assert pred(ev(result={"messages": [{"text": "NONE SHALL PASS!"}]})) is True
    assert pred(ev(result={"messages": [{"text": "hello"}]})) is False
    assert pred(ev(result=None)) is False


def test_on_question_appeared() -> None:
    assert on_question_appeared()(ev(result={"question": {"choices": []}})) is True
    assert on_question_appeared()(ev(result={})) is False


def test_on_call_matches_tool_and_args() -> None:
    pred = on_call("answer", id=3)
    assert pred(ev(tool="answer", args={"id": 3})) is True
    assert pred(ev(tool="answer", args={"id": 2})) is False
    assert pred(ev(tool="walk", args={"id": 3})) is False
    # int/str equivalence
    assert on_call("answer", id="3")(ev(tool="answer", args={"id": 3})) is True


def test_in_room_reads_state_before() -> None:
    assert in_room(52)(ev(room=52)) is True
    assert in_room(52)(ev(room=51)) is False


def test_all_of_and_any_of() -> None:
    p = all_of(in_room(52), on_room_changed(51))
    assert p(ev(result={"room_changed": 51}, room=52)) is True
    assert p(ev(result={"room_changed": 51}, room=99)) is False
    q = any_of(on_room_changed(1), on_room_changed(2))
    assert q(ev(result={"room_changed": 2})) is True
    assert q(ev(result={"room_changed": 3})) is False


def test_goalset_requires_exactly_one_stopping_goal() -> None:
    with pytest.raises(ValueError):
        GoalSet("g", None, {"a": Goal("a", "A", on_call("x"))})  # zero stopping
    with pytest.raises(ValueError):
        GoalSet(
            "g",
            None,
            {
                "a": Goal("a", "A", on_call("x"), stopping=True),
                "b": Goal("b", "B", on_call("y"), stopping=True),
            },
        )


def test_monkey_goalset_shape() -> None:
    gs = get_goal_set("monkey-ega-demo", 1)
    assert gs.total() == 29
    assert gs.stopping_goal_id == "tell_troll_phrase"
    # every goal has a human-readable label
    assert all(g.label and isinstance(g.label, str) for g in gs.goals.values())


def test_get_goal_set_unknown_raises() -> None:
    with pytest.raises(KeyError):
        get_goal_set("does-not-exist", 1)
