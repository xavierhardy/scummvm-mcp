"""
Integration tests for the German Monkey Island 1 EGA demo.

Mirrors the structure of test_monkey.py but exercises the same walkthrough
through the German release. Validates that the MCP server transports
non-ASCII labels and dialog text (umlauts, eszett) as UTF-8 in both
directions: state output is UTF-8 encoded, and verb/target arguments
containing UTF-8 multibyte characters are matched against the game's
native code page (CP-850) entries.

The German "Öffne" (open) verb starts with an uppercase umlaut. Verb names
are matched case-insensitively, but ScummVM's Common::String::toLowercase()
only folds ASCII, so the lowercase "öffne" sent by MCP clients used to miss
the "Öffne" label and fail with "unknown verb". The fix makes the matcher
fold Latin-1 uppercase letters too; test_06 guards it. The other
encoding-critical steps — the door labelled "tür", the verb "schließe",
and the dialog answer with the eszett-bearing troll line — round-trip ß and ü.
"""

from time import sleep

from assertions import assert_inventory_does_not_contain, assert_inventory_contains
from utils import McpClient


def test_01_de_initial_state(monkey_de_client: McpClient) -> None:
    """Verify initial game state and that the German door surfaces as 'tür'."""
    state = monkey_de_client.state()
    assert "room" in state
    assert state["room"]["id"] == 55
    assert state.get("objects") is not None
    assert isinstance(state.get("objects"), list)
    assert len(state.get("objects", [])) > 0
    assert len(state.get("inventory", [])) == 0
    assert "position" in state
    assert state["position"] == {"y": 132, "x": 235}
    assert len(state.get("messages", [])) == 0

    # German object names with umlauts must transit MCP as UTF-8 (no U+FFFD).
    names = {o["name"] for o in state["objects"]}
    assert names == {"torbogen", "tür"}, f"Unexpected objects: {sorted(names)}"

    # German verb bar must round-trip ß and ü intact.
    assert "schließe" in state["verbs"]
    assert "drücke" in state["verbs"]
    assert "rede mit" in state["verbs"]


def test_02_de_walk_to_troll(monkey_de_client: McpClient) -> None:
    """Walk to Troll. The troll greets in German."""
    state = monkey_de_client.state()
    assert state["room"]["id"] == 55

    result = monkey_de_client.walk(120, 132)
    assert "x" in result["position"]
    assert "y" in result["position"]
    assert result["messages"] == [{"actor": "troll", "text": "Keiner kommt vorbei!"}]

    monkey_de_client.act("geh zu", "troll")

    state = monkey_de_client.state()
    assert state["position"]["x"] < 200, (
        f"Expected Guybrush near troll, got {state['position']}"
    )


def test_03_de_talk_to_troll(monkey_de_client: McpClient) -> None:
    """Talk to Troll to trigger German dialog with eszett and umlauts."""
    state = monkey_de_client.state()
    assert state["room"]["id"] == 55

    result = monkey_de_client.act("rede mit", "troll")
    expected = {
        "question": {
            "choices": [
                {"id": 1, "label": "Aber ich will Pirat werden!"},
                {"id": 2, "label": "Warum?"},
                {"id": 3, "label": "Bitte, bitte?"},
            ]
        },
        "messages": [
            {"text": "Hi. Ich bin Guybrush Threepwood und--", "actor": "guybrush"},
            {
                "text": (
                    "Ist mir egal, wie du heißt oder was du willst, du "
                    "schlubbriger Schlobber von schlabbrigem Schleim! "
                    "Niemand kommt ohne den Zauberspruch hier durch."
                ),
                "actor": "troll",
            },
        ],
    }
    assert result == expected

    # heißt contains an eszett — confirm at the byte level.
    troll_line = result["messages"][1]["text"]
    assert "heißt" in troll_line
    assert "ß".encode("utf-8") in troll_line.encode("utf-8")

    state = monkey_de_client.state()
    assert state.get("question") is not None


def test_04_de_answer_troll_dialog(monkey_de_client: McpClient) -> None:
    """Answer dialog choice 3 (Bitte, bitte?)."""
    state = monkey_de_client.state()
    assert state["room"]["id"] == 55

    result = monkey_de_client.answer(3)
    assert result == {
        "messages": [
            {"text": "Bitte, bitte?", "actor": "guybrush"},
            {
                "text": (
                    "Heh, nicht diesen Zauberspruch, du vor Höflichkeit "
                    "stinkender Traum-Schwiegersohn, den anderen Zauberspruch!"
                    " --seufz--"
                ),
                "actor": "troll",
            },
        ]
    }

    # Höflichkeit carries an umlaut — confirm round-trip.
    assert "Höflichkeit" in result["messages"][1]["text"]


def test_05_de_walk_to_door(monkey_de_client: McpClient) -> None:
    """Walk to the door — target name 'tür' is sent as UTF-8 and matched
    against the game's CP-850 'Tür' label."""
    state = monkey_de_client.state()
    assert state["room"]["id"] == 55

    result = monkey_de_client.act("geh zu", "tür")
    assert result == {"position": {"y": 132, "x": 361}}


def test_06_de_open_door_lowercase(monkey_de_client: McpClient) -> None:
    """Open the door with the *lowercase* 'öffne' verb.

    Regression test for the umlaut-folding bug: the game's verb label is
    "Öffne" (leading uppercase Ö). MCP clients send verbs lowercased, so they
    pass "öffne". The matcher folds case before comparing, but the old
    ASCII-only fold left the leading Ö untouched, so "öffne" never matched
    "Öffne" and the call failed with "unknown verb 'öffne'" — even though the
    capitalised "Öffne" resolved fine. The fix folds Latin-1 uppercase letters
    too, so the lowercase form now resolves and the door opens (state 0 -> 1).
    """
    state = monkey_de_client.state()
    assert state["room"]["id"] == 55

    door = next(o for o in state["objects"] if o["id"] == 422)
    assert door["name"] == "tür"
    assert door["state"] == 0, "door should start closed in a fresh demo"

    # Must not raise "unknown verb 'öffne'": the leading umlaut is folded so
    # the lowercase client verb matches the game's "Öffne" label.
    result = monkey_de_client.act("öffne", "tür")
    assert result["objects_changed"] == [
        {"name": "tür", "old_state": 0, "new_state": 1}
    ]


def test_07_de_close_door(monkey_de_client: McpClient) -> None:
    """Dispatch the 'schließe' verb (with ß) on the German door. This
    proves the inbound UTF-8 verb name resolves to the correct verb id."""
    state = monkey_de_client.state()
    assert state["room"]["id"] == 55

    # The German EGA demo door exposes only "schließe" and "geh zu" as
    # compatible verbs — confirm the listing and that the verb dispatches
    # without an "unknown verb" error.
    door = next(o for o in state["objects"] if o["id"] == 422)
    assert door["name"] == "tür"
    assert "schließe" in door["compatible_verbs"]

    # Door was opened by test_06; closing it now is a real state change
    # (1 -> 0) and proves the eszett-bearing "Schließe" label resolves.
    result = monkey_de_client.act("schließe", "tür")
    assert result["objects_changed"] == [
        {"name": "tür", "old_state": 1, "new_state": 0}
    ]


# ---------------------------------------------------------------------------
# Red herring timing puzzle (dock behind the SCUMM bar)
# ---------------------------------------------------------------------------


def test_08_de_navigate_to_scumm_bar_dock(monkey_de_client: McpClient) -> None:
    """Walk from the troll clearing through the bar and kitchen onto the back
    dock, where the seagull guards the red herring (object 306)."""
    state = monkey_de_client.state()
    assert state["room"]["id"] == 55

    monkey_de_client.act("öffne", "tür")  # test_07 closed it again
    result = monkey_de_client.act("geh zu", "tür")
    assert result.get("room_changed") == 52, f"expected the SCUMM bar, got {result}"

    monkey_de_client.act("öffne", 354)  # far door behind the bar
    result = monkey_de_client.act("geh zu", 354)
    assert result.get("room_changed") == 51, f"expected the kitchen, got {result}"

    monkey_de_client.act("öffne", 304)  # back door onto the dock
    monkey_de_client.act("geh zu", 304)  # same room — it scrolls to the dock

    objects = {o["id"]: o["name"] for o in monkey_de_client.state()["objects"]}
    assert 306 in objects, f"red herring not in view, objects: {objects}"
    # The game pads the name with trailing '@' bytes ("roter Hering@@@@@...");
    # the MCP server must emit it padding-free.
    assert objects[306] == "roter_hering", f"name not padding-free: {objects[306]!r}"


def test_09_de_seagull_blocks_red_herring(monkey_de_client: McpClient) -> None:
    """Grabbing the herring without bouncing the seagull away must fail.

    Targets the herring by its padding-free name — the matcher must resolve
    "roter_hering" against the game's '@'-padded label.
    """
    result = monkey_de_client.act("nimm", "roter_hering")
    assert "roter_hering" not in monkey_de_client.state()["inventory"]
    assert result.get("messages"), (
        f"expected Guybrush to comment on the seagull, got {result}"
    )


def test_10_de_plank_bounce_frees_red_herring(monkey_de_client: McpClient) -> None:
    """Walk the loose plank 3 times, then immediately grab the red herring.

    Each step on the loose plank (object 307 at the end of the dock)
    catapults the seagull into the air for a short moment; three bounces buy
    enough time to walk over and take the fish before it lands again. The
    timing is the client's responsibility: the bridge keeps the Monkey
    Island 1 inter-action settle minimal (5 frames), so each bounce needs a
    short pause for its plank script to finish before the next step — and
    the final grab must follow immediately, with no pause at all.
    """
    client = monkey_de_client
    for _ in range(3):
        client.act("geh zu", 307)
        sleep(1.5)  # let the plank-bounce script finish before the next step
    client.act("nimm", "roter_hering")
    assert "roter_hering" in client.state()["inventory"], (
        "red herring should be in inventory after 3 plank bounces + quick grab"
    )
