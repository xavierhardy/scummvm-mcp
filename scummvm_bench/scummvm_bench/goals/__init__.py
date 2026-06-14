"""Goal definitions and the per-(game, save) registry.

Add a game by writing a module that builds a :class:`GoalSet` and registering it
in ``GOAL_SETS`` keyed by ``(game_id, save_slot)``.
"""

from .engine import Goal, GoalEvent, GoalSet
from .monkey_ega_demo import MONKEY_EGA_DEMO_GOALSET

# (game_id, save_slot) -> GoalSet. save_slot None means "any slot".
GOAL_SETS: dict[tuple[str, int | None], GoalSet] = {
    ("monkey-ega-demo", 1): MONKEY_EGA_DEMO_GOALSET,
}


def get_goal_set(game_id: str, save_slot: int | None) -> GoalSet:
    """Return the goal set for a game/save, falling back to a slot-agnostic entry."""
    if (game_id, save_slot) in GOAL_SETS:
        return GOAL_SETS[(game_id, save_slot)]
    if (game_id, None) in GOAL_SETS:
        return GOAL_SETS[(game_id, None)]
    raise KeyError(
        f"no goal set registered for game={game_id!r} save_slot={save_slot!r}"
    )


__all__ = [
    "GOAL_SETS",
    "Goal",
    "GoalEvent",
    "GoalSet",
    "get_goal_set",
    "MONKEY_EGA_DEMO_GOALSET",
    "get_goal_set",
]
