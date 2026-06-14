"""Per-game objective prompts handed to the agent harness."""

from ..models import RunSpec

_GENERIC_RULES = (
    "You control ScummVM only through the provided MCP tools "
    "(state, act, walk, answer, skip). "
    "Actions MUST be executed sequentially. Call `state` to observe the room, "
    "its objects and your inventory. If two objects share a `name`, pass the "
    "integer `id` instead. Once a door is opened/unlocked, `walk` to it (by "
    "`name` or integer `id`) to go through. Comment briefly on each step. "
    "NEVER STOP until you have finished. You are on your own."
)

_GAME_OBJECTIVES = {
    "monkey-ega-demo": (
        "Play The Secret of Monkey Island (EGA demo). Reach the troll on the "
        "bridge and learn that a magic phrase is needed to pass. Explore the "
        "SCUMM Bar and its back room, cross the dock, pick up what you can, "
        "reach the city, visit the jail and the fortune teller, then return to "
        "the troll and tell him the magic phrase."
    ),
}


def build_prompt(spec: RunSpec) -> str:
    """Return the full prompt (objective + MCP rules) for ``spec``'s game."""
    objective = _GAME_OBJECTIVES.get(
        spec.game_id,
        f"Play the ScummVM game `{spec.game_id}` as far as you can.",
    )
    return f"{objective}\n\n{_GENERIC_RULES}"
