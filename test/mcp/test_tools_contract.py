"""
What tools/list promises, across games.

The tool table is not fixed: which tools exist, and what they say, depend on
the game and on which optional settings are on. These tests hold that promise
to three rules, whatever the game:

  1. Every tool declares both an input and an output schema, so a client knows
     what to send and what it will get back.
  2. No description names a tool that is not registered — an agent must never
     be pointed at something it cannot call.
  3. No description names a game or an engine, or describes how the server is
     built. It says what the tool does and what comes back.
"""

import re

import pytest

from mcp_client import McpClient

# Names that must never appear in what an agent reads: game titles, engine
# names, and the vocabulary of the implementation.
FORBIDDEN_WORDS = [
    "scumm",
    "gob engine",
    "maniac mansion",
    "zak mckracken",
    "monkey island",
    "loom",
    "indiana jones",
    "fate of atlantis",
    "day of the tentacle",
    "sam & max",
    "full throttle",
    "the dig",
    "broken sword",
    "beneath a steel sky",
    "amazon queen",
    "woodruff",
    "gobliiins",
    "scummvm",
    "confman",
    "bridge",
]

# Tools that only exist when an optional setting is on. A description may only
# name one of these when it is actually registered.
OPTIONAL_TOOLS = [
    "skip",
    "debug",
    "keystroke",
    "mouse_move",
    "mouse_click",
    "screenshot",
    "save_state",
    "set_talk_speed",
    "answer",
]


def _check_tools(tools: list[dict]) -> None:
    """Apply the three rules to one game's tool table."""
    assert tools, "no tools registered at all"
    names = {tool["name"] for tool in tools}

    for tool in tools:
        assert tool.get("inputSchema"), f"{tool['name']} declares no input schema"
        assert tool.get("outputSchema"), f"{tool['name']} declares no output schema"

        description = tool.get("description", "")
        assert description, f"{tool['name']} has no description"
        lowered = description.lower()

        for word in FORBIDDEN_WORDS:
            assert word not in lowered, (
                f"{tool['name']} mentions '{word}': {description}"
            )

        for other in OPTIONAL_TOOLS:
            if other in names or other == tool["name"]:
                continue
            assert not re.search(rf"\b{other}\b", lowered), (
                f"{tool['name']} points at '{other}', which is not registered: "
                f"{description}"
            )


@pytest.mark.parametrize(
    # A spread wide enough to reach every extra tool a game can add: the
    # phone/team tools, the note player, the cannon, and each engine's own
    # snapshot wording.
    "fixture_name",
    [
        "monkey_client",
        "maniac_client",
        "zak_client",
        "loom_client",
        "comi_client",
        "gob1_client",
        "sky_client",
        "queen_client",
        "sword1_client",
    ],
)
def test_tool_table_keeps_its_promises(fixture_name: str, request) -> None:
    """Every game's tool table is complete, consistent and game-agnostic."""
    client: McpClient = request.getfixturevalue(fixture_name)
    _check_tools(client.list_tools())


def test_optional_tools_are_absent_when_turned_off(
    plain_tools_client: McpClient,
) -> None:
    """With the optional settings off, their tools are gone — and unmentioned."""
    tools = plain_tools_client.list_tools()
    names = {tool["name"] for tool in tools}

    assert "state" in names and "act" in names, f"core tools missing: {names}"
    for absent in ("skip", "debug", "keystroke", "mouse_click", "screenshot"):
        assert absent not in names, f"{absent} is registered with its option off"

    _check_tools(tools)

    # And calling one is refused rather than silently doing nothing.
    refused = plain_tools_client.call_tool_raw("skip")
    assert "error" in refused.get("structuredContent", {}), (
        f"a tool that is not registered answered anyway: {refused}"
    )
