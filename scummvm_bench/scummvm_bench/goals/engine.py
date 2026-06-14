"""The goal-evaluation engine: goals, goal sets, events, and predicates.

A *goal* is a human-readable, latching objective scored 1 point. Its predicate
fires either on a tool **call** (the verb/target the agent invoked, possibly in a
given state) or on what the call **returned** (a new room, object, inventory item
or message). Exactly one goal per set is the *stopping* goal that ends the run.
"""

from collections.abc import Callable
from dataclasses import dataclass


@dataclass(frozen=True)
class GoalEvent:
    """Everything a predicate may inspect for a single tool call."""

    tool: str
    args: dict[str, object]
    result: dict[str, object] | None
    state_before: dict[str, object]
    ok: bool


Predicate = Callable[[GoalEvent], bool]


@dataclass(frozen=True)
class Goal:
    """A single latching objective."""

    goal_id: str
    label: str
    predicate: Predicate
    stopping: bool = False
    kind: str = "result"  # "call" | "result", documentation only


@dataclass
class GoalSet:
    """An ordered collection of goals for one game/save.

    The insertion order of ``goals`` defines the scoring denominator and the
    column order in reports. Exactly one goal must be marked ``stopping``.
    """

    game_id: str
    save_slot: int | None
    goals: dict[str, Goal]

    def __post_init__(self) -> None:
        stopping = [g.goal_id for g in self.goals.values() if g.stopping]
        if len(stopping) != 1:
            raise ValueError(
                f"goal set {self.game_id!r} must have exactly one stopping goal, "
                f"found {stopping}"
            )

    @property
    def stopping_goal_id(self) -> str:
        return next(g.goal_id for g in self.goals.values() if g.stopping)

    def total(self) -> int:
        return len(self.goals)


# ---------------------------------------------------------------------------
# Narrowing helpers (keep predicates free of ``Any``)
# ---------------------------------------------------------------------------


def _as_list(value: object) -> list[object]:
    if isinstance(value, list):
        return [item for item in value]
    return []


def _as_dict(value: object) -> dict[str, object]:
    if isinstance(value, dict):
        return {str(key): val for key, val in value.items()}
    return {}


def _norm(value: object) -> str:
    """Normalise a name/token for lenient comparison."""
    return str(value).strip().rstrip("@").lower()


def _result_list(event: GoalEvent, key: str) -> list[object]:
    if not event.result:
        return []
    return _as_list(event.result.get(key))


def _current_room(state: dict[str, object]) -> object:
    return _as_dict(state.get("room")).get("id")


# ---------------------------------------------------------------------------
# Predicate constructors
# ---------------------------------------------------------------------------


def on_room_changed(room_id: int) -> Predicate:
    """Result-based: the call moved the player into ``room_id``."""

    def predicate(event: GoalEvent) -> bool:
        return bool(event.result) and event.result.get("room_changed") == room_id

    return predicate


def on_inventory_added(item: str) -> Predicate:
    """Result-based: ``item`` appeared in ``inventory_added``."""
    target = _norm(item)

    def predicate(event: GoalEvent) -> bool:
        return any(target in _norm(i) for i in _result_list(event, "inventory_added"))

    return predicate


def on_inventory_removed(item: str) -> Predicate:
    """Result-based: ``item`` appeared in ``inventory_removed``."""
    target = _norm(item)

    def predicate(event: GoalEvent) -> bool:
        return any(target in _norm(i) for i in _result_list(event, "inventory_removed"))

    return predicate


def on_object_changed(name: str) -> Predicate:
    """Result-based: an object named ``name`` changed state."""
    target = _norm(name)

    def predicate(event: GoalEvent) -> bool:
        return any(
            target in _norm(_as_dict(o).get("name"))
            for o in _result_list(event, "objects_changed")
        )

    return predicate


def on_message_contains(substr: str) -> Predicate:
    """Result-based: a returned message text contains ``substr``."""
    needle = substr.lower()

    def predicate(event: GoalEvent) -> bool:
        for message in _result_list(event, "messages"):
            text = _as_dict(message).get("text")
            if isinstance(text, str) and needle in text.lower():
                return True
        return False

    return predicate


def on_question_appeared() -> Predicate:
    """Result-based: the call surfaced a multiple-choice dialog question."""

    def predicate(event: GoalEvent) -> bool:
        return bool(event.result) and bool(event.result.get("question"))

    return predicate


def on_call(tool: str, **arg_match: object) -> Predicate:
    """Call-based: the agent invoked ``tool`` with matching arguments."""
    wanted = {k: _norm(v) for k, v in arg_match.items()}

    def predicate(event: GoalEvent) -> bool:
        if event.tool != tool:
            return False
        return all(_norm(event.args.get(k)) == v for k, v in wanted.items())

    return predicate


def in_room(room_id: int) -> Predicate:
    """State guard: the player was in ``room_id`` *before* the call."""

    def predicate(event: GoalEvent) -> bool:
        return _current_room(event.state_before) == room_id

    return predicate


def all_of(*predicates: Predicate) -> Predicate:
    """Combinator: every predicate must hold for the same event."""

    def predicate(event: GoalEvent) -> bool:
        return all(p(event) for p in predicates)

    return predicate


def any_of(*predicates: Predicate) -> Predicate:
    """Combinator: at least one predicate must hold."""

    def predicate(event: GoalEvent) -> bool:
        return any(p(event) for p in predicates)

    return predicate
