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

from assertions import assert_has_position, assert_room
from utils import McpClient, bind_verb, make_verbs, object_by_id, object_names


def _talk_to_troll(client: McpClient) -> dict:
    """Walk to the troll and open his dialog; return the talk result.

    Self-contained setup so the dialog tests run on their own fresh instance:
    walking near the troll first (as a player would) reproduces the same talk
    result regardless of Guybrush's start position.
    """
    client.walk(120, 132)
    return client.act("rede mit", "troll")


def _navigate_to_dock(client: McpClient) -> dict:
    """From the troll clearing (room 55), open the doors and walk through the
    SCUMM bar and kitchen onto the back dock where the red herring (306) sits.

    Self-contained setup for the dock/red-herring tests. Returns the dock state.
    """
    client.act("öffne", "tür")
    client.act("geh zu", "tür")  # -> SCUMM bar (room 52)
    client.act("öffne", 354)  # far door behind the bar
    client.act("geh zu", 354)  # -> kitchen (room 51)
    client.act("öffne", 304)  # back door onto the dock
    client.act("geh zu", 304)  # same room — scrolls to the dock
    return client.state()


def _ensure_door_open(client: McpClient) -> dict | None:
    """Open the troll-clearing door (id 422) if it is closed; return the door."""
    door = object_by_id(client.state(), 422)
    if door and door["state"] == 0:
        client.act("öffne", "tür")
        door = object_by_id(client.state(), 422)
    return door


def _bounce_plank(client: McpClient, times: int = 3) -> None:
    """Step on the loose plank (307) *times*, pausing for each bounce script."""
    for _ in range(times):
        client.act("geh zu", 307)
        sleep(1.5)  # let the plank-bounce script finish before the next step


def test_01_de_initial_state(monkey_de_client: McpClient) -> None:
    """Verify initial game state and that the German door surfaces as 'tür'."""
    state = monkey_de_client.state()
    assert_room(state, 55)
    objects = state.get("objects")
    assert isinstance(objects, list), f"expected 'objects' to be a list, got {objects}"
    assert len(objects) > 0, "expected at least one object in room 55"
    inventory = state.get("inventory", [])
    assert inventory == [], f"expected an empty starting inventory, got {inventory}"
    position = state.get("position")
    assert position == {"x": 235, "y": 132}, f"unexpected start position: {position}"
    messages = state.get("messages", [])
    assert messages == [], f"expected no pending messages at start, got {messages}"

    # German object names with umlauts must transit MCP as UTF-8 (no U+FFFD).
    names = object_names(state)
    sorted_names = sorted(names)
    assert names == {"torbogen", "tür"}, f"unexpected objects: {sorted_names}"

    # German verb bar must round-trip ß and ü intact.
    verbs = state["verbs"]
    assert "schließe" in verbs, f"missing 'schließe' verb: {verbs}"
    assert "drücke" in verbs, f"missing 'drücke' verb: {verbs}"
    assert "rede mit" in verbs, f"missing 'rede mit' verb: {verbs}"


def test_02_de_walk_to_troll(monkey_de_client: McpClient) -> None:
    """Walk to Troll. The troll greets in German."""
    assert_room(monkey_de_client.state(), 55)
    geh_zu = bind_verb(monkey_de_client, "geh zu")

    result = monkey_de_client.walk(120, 132)
    assert_has_position(result)
    messages = result["messages"]
    expected = [{"actor": "troll", "text": "Keiner kommt vorbei!"}]
    assert messages == expected, f"unexpected approach messages: {messages}"

    geh_zu("troll")
    position = monkey_de_client.state()["position"]
    assert position["x"] < 200, f"expected Guybrush near troll (x<200), got {position}"


def test_03_de_talk_to_troll(monkey_de_client: McpClient) -> None:
    """Talk to Troll to trigger German dialog with eszett and umlauts."""
    assert_room(monkey_de_client.state(), 55)

    result = _talk_to_troll(monkey_de_client)
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
    assert result == expected, f"unexpected troll dialog: {result}"

    # heißt contains an eszett — confirm at the byte level.
    troll_line = result["messages"][1]["text"]
    assert "heißt" in troll_line, f"expected 'heißt' in {troll_line!r}"
    assert "ß".encode() in troll_line.encode("utf-8"), "eszett byte missing"

    question = monkey_de_client.state().get("question")
    assert question is not None, "expected a dialog question after talking to Troll"


def test_04_de_answer_troll_dialog(monkey_de_client: McpClient) -> None:
    """Answer dialog choice 3 (Bitte, bitte?)."""
    assert_room(monkey_de_client.state(), 55)

    # Open the troll dialog first so this test stands alone.
    _talk_to_troll(monkey_de_client)

    result = monkey_de_client.answer(3)
    expected = {
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
    assert result == expected, f"unexpected answer(3) result: {result}"

    # Höflichkeit carries an umlaut — confirm round-trip.
    troll_line = result["messages"][1]["text"]
    assert "Höflichkeit" in troll_line, f"expected 'Höflichkeit' in {troll_line!r}"


def test_05_de_walk_to_door(monkey_de_client: McpClient) -> None:
    """Walk to the door — target name 'tür' is sent as UTF-8 and matched
    against the game's CP-850 'Tür' label."""
    assert_room(monkey_de_client.state(), 55)
    geh_zu = bind_verb(monkey_de_client, "geh zu")

    result = geh_zu("tür")
    expected = {"position": {"x": 361, "y": 132}}
    assert result == expected, f"unexpected walk-to-door position: {result}"


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
    assert_room(monkey_de_client.state(), 55)
    oeffne = bind_verb(monkey_de_client, "öffne")

    door = object_by_id(monkey_de_client.state(), 422)
    assert door is not None, "expected the door object (id 422) in the state"
    assert door["name"] == "tür", f"expected the door named 'tür', got {door}"
    assert door["state"] == 0, "door should start closed in a fresh demo"

    # Must not raise "unknown verb 'öffne'": the leading umlaut is folded so
    # the lowercase client verb matches the game's "Öffne" label.
    changed = oeffne("tür")["objects_changed"]
    expected = [{"name": "tür", "old_state": 0, "new_state": 1}]
    assert changed == expected, f"unexpected door change: {changed}"


def test_07_de_close_door(monkey_de_client: McpClient) -> None:
    """Dispatch the 'schließe' verb (with ß) on the German door. This
    proves the inbound UTF-8 verb name resolves to the correct verb id."""
    assert_room(monkey_de_client.state(), 55)
    schliesse = bind_verb(monkey_de_client, "schließe")

    # Open the door first so this test is self-contained (the closed door does
    # not advertise "schließe"; only an open one can be closed).
    door = _ensure_door_open(monkey_de_client)
    assert door is not None, "expected the door object (id 422) in the state"
    assert door["name"] == "tür", f"expected the door named 'tür', got {door}"
    assert "schließe" in door["compatible_verbs"], f"missing 'schließe': {door}"

    # Closing the open door is a real state change (1 -> 0) and proves the
    # eszett-bearing "Schließe" label resolves.
    changed = schliesse("tür")["objects_changed"]
    expected = [{"name": "tür", "old_state": 1, "new_state": 0}]
    assert changed == expected, f"unexpected door change: {changed}"


# ---------------------------------------------------------------------------
# Red herring timing puzzle (dock behind the SCUMM bar)
# ---------------------------------------------------------------------------


def test_08_de_navigate_to_scumm_bar_dock(monkey_de_client: McpClient) -> None:
    """Walk from the troll clearing through the bar and kitchen onto the back
    dock, where the seagull guards the red herring (object 306)."""
    assert_room(monkey_de_client.state(), 55)
    oeffne, geh_zu = make_verbs(monkey_de_client, "öffne", "geh zu")

    oeffne("tür")  # door starts closed in a fresh demo
    result = geh_zu("tür")
    assert result.get("room_changed") == 52, f"expected the SCUMM bar, got {result}"

    oeffne(354)  # far door behind the bar
    result = geh_zu(354)
    assert result.get("room_changed") == 51, f"expected the kitchen, got {result}"

    oeffne(304)  # back door onto the dock
    geh_zu(304)  # same room — it scrolls to the dock

    # The game pads the name with trailing '@' bytes ("roter Hering@@@@@...");
    # the MCP server must emit it padding-free.
    herring = object_by_id(monkey_de_client.state(), 306)
    assert herring is not None, "red herring (306) not in view"
    name = herring["name"]
    assert name == "roter_hering", f"name not padding-free: {name!r}"


def test_09_de_seagull_blocks_red_herring(monkey_de_client: McpClient) -> None:
    """Grabbing the herring without bouncing the seagull away must fail.

    Targets the herring by its padding-free name — the matcher must resolve
    "roter_hering" against the game's '@'-padded label.
    """
    _navigate_to_dock(monkey_de_client)
    nimm = bind_verb(monkey_de_client, "nimm")

    result = nimm("roter_hering")
    inventory = monkey_de_client.state()["inventory"]
    assert "roter_hering" not in inventory, "grabbing the guarded herring must fail"
    assert result.get("messages"), f"expected a seagull comment, got {result}"


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
    _navigate_to_dock(client)
    nimm = bind_verb(client, "nimm")

    _bounce_plank(client, 3)
    nimm("roter_hering")
    inventory = client.state()["inventory"]
    assert "roter_hering" in inventory, "herring not in inventory after bounces"


# ---------------------------------------------------------------------------
# Door state names and the English 'open' verb
# ---------------------------------------------------------------------------


def test_11_de_door_state_name(monkey_de_client: McpClient) -> None:
    """The troll-clearing door reports opened/closed state names and advertises
    the open verb; opening it flips the state to 'opened'."""
    client = monkey_de_client
    assert_room(client.state(), 55)

    door = object_by_id(client.state(), 422)
    assert door is not None, "door (422) not in room 55"
    assert door["state_name"] == "closed", f"door not 'closed': {door}"
    assert "Öffne" in door["compatible_verbs"], f"door missing open verb: {door}"

    client.act("öffne", "tür")
    door = object_by_id(client.state(), 422)
    assert door is not None, "door (422) vanished after opening"
    assert door["state_name"] == "opened", f"door not 'opened' after open: {door}"

    # The archway is not an openable, so it has no state_name.
    arch = object_by_id(client.state(), 421)
    assert arch is not None and "state_name" not in arch, (
        f"archway got a state_name: {arch}"
    )


def test_12_de_english_open_verb(monkey_de_client: McpClient) -> None:
    """The canonical English 'open' verb drives the German 'Öffne' door label."""
    client = monkey_de_client
    assert_room(client.state(), 55)
    changed = client.act("open", "tür")["objects_changed"]
    assert changed == [{"name": "tür", "old_state": 0, "new_state": 1}], (
        f"'open' did not open the German door: {changed}"
    )


# ---------------------------------------------------------------------------
# Kitchen plank (surfaced from an untouchable, unnamed hotspot)
# ---------------------------------------------------------------------------


def test_13_de_kitchen_plank_walkable(monkey_de_client: McpClient) -> None:
    """The loose plank surfaces as the named object 'planke' with a walk_to
    (geh zu) verb, and can be bounced by name."""
    client = monkey_de_client
    _navigate_to_dock(client)

    plank = object_by_id(client.state(), 307)
    assert plank is not None, "plank (307) not surfaced in the kitchen"
    assert plank["name"] == "planke", f"plank not named 'planke': {plank}"
    assert "geh zu" in plank["compatible_verbs"], f"plank not walkable: {plank}"

    # Walking onto it by name catapults the seagull (a real state change).
    changed = client.act("geh zu", "planke")["objects_changed"]
    assert any(c["name"] == "obj-307" for c in changed), (
        f"walking the plank by name did nothing: {changed}"
    )
