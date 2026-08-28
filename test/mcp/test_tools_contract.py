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

Plus the promise behind the schemas themselves: each one is a valid JSON
Schema, and what the game actually answers with matches the schema it
advertises for that tool (the client checks every result it decodes against it,
so this only has to make the check happen for each game).
"""

import base64
import os
import re

import jsonschema
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
    "discworld",
    "tinsel",
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
    # Game-specific tools a description may only point at where the game
    # registers them (the fist-fight games do; nothing else does).
    "fight",
]


def _check_tools(tools: list[dict]) -> None:
    """Apply the three rules to one game's tool table."""
    assert tools, "no tools registered at all"
    names = {tool["name"] for tool in tools}

    for tool in tools:
        assert tool.get("inputSchema"), f"{tool['name']} declares no input schema"
        assert tool.get("outputSchema"), f"{tool['name']} declares no output schema"

        # A schema a client cannot compile is no better than none at all.
        for kind in ("inputSchema", "outputSchema"):
            try:
                jsonschema.Draft202012Validator.check_schema(tool[kind])
            except jsonschema.SchemaError as exc:
                raise AssertionError(
                    f"{tool['name']} {kind} is not a valid schema: {exc.message}"
                ) from exc

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
        "indy3_client",
        "sky_client",
        "queen_client",
        "sword1_client",
        "dw1_client",
    ],
)
def test_tool_table_keeps_its_promises(fixture_name: str, request) -> None:
    """Every game's tool table is complete, consistent and game-agnostic."""
    client: McpClient = request.getfixturevalue(fixture_name)
    _check_tools(client.list_tools())

    # And what the game answers with matches the schema it just advertised:
    # state is the snapshot every other tool is read against, and the one whose
    # fields games add to (the fist-fight gauges, the team, ...).
    schema = next(
        tool["outputSchema"] for tool in client.list_tools() if tool["name"] == "state"
    )
    errors = sorted(
        jsonschema.Draft202012Validator(schema).iter_errors(client.state()),
        key=lambda e: list(e.path),
    )
    assert not errors, "state breaks its own schema — " + "; ".join(
        f"at {list(e.path)}: {e.message}" for e in errors
    )


def test_screenshot_returns_the_frame_and_files_it(monkey_client: McpClient) -> None:
    """The frame comes back as an image, and by default is written out too."""
    shots = monkey_client.screenshot_path
    assert shots is not None, "the fixture did not record a screenshot folder"
    before = set(os.listdir(shots))

    result = monkey_client.call_tool_raw("screenshot")
    images = [b for b in result["content"] if b.get("type") == "image"]
    assert len(images) == 1, f"no image in the result: {result['content']}"
    assert base64.b64decode(images[0]["data"])[:4] == b"\x89PNG"
    structured = result["structuredContent"]
    assert structured["saved"] is True, f"not written out: {structured}"
    assert structured["width"] > 0 and structured["height"] > 0, structured

    written = set(os.listdir(shots)) - before
    assert written, f"nothing appeared in {shots}"

    # Asking for the picture alone leaves the folder untouched.
    result = monkey_client.call_tool_raw("screenshot", {"save_to_disk": False})
    assert [b for b in result["content"] if b.get("type") == "image"], result
    assert "saved" not in result["structuredContent"]
    assert set(os.listdir(shots)) - before == written, "a file was written anyway"


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
