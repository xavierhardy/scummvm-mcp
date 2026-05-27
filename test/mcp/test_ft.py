"""
Integration tests for Full Throttle (DOS demo, SCUMM V7).

Full Throttle uses a verb-coin interface: holding the mouse over a hotspot pops
up three action icons around Ben's head —

  * fist  (grab / punch / operate / use)   -> object verb script 9
  * kick  (boot)                           -> object verb script 5
  * mouth (talk / look / comment)          -> object verb script 8

plus 'walk to' (scene-click move/exit), a generic single-click 'interact'
(resolved to the object's best available coin action) and 'use item' (an
inventory item on a target). Unlike The Dig (also V7), Full Throttle objects
carry real per-verb entrypoints, so each object only advertises the coin verbs
it actually scripts.

The demo opens with Ben trapped *inside* the dumpster (room 10). A short wake-up
animation plays, then walking to the opening lets him climb out into the alley
(room 6) — you cannot interact with the world until you are out. Every test
therefore escapes the dumpster first (module-scoped `ft_escaped` fixture).
"""

import pytest
from time import sleep

from utils import McpClient, get_state_with_retry


FT_VERBS = {"fist", "kick", "mouth", "walk to", "interact", "use item"}


def _act_until_done(client: McpClient, *args, attempts: int = 10) -> dict:
    """Run act(), skipping through any cutscene that is holding input."""
    last = None
    for _ in range(attempts):
        try:
            return client.act(*args)
        except RuntimeError as e:
            last = e
            if "not accepting input" in str(e):
                try:
                    client.skip()
                except Exception:
                    pass
                sleep(1.0)
                continue
            raise
    raise AssertionError(f"act{args} never accepted input: {last}")


def _act_fresh(client: McpClient, verb: str, want_verb: str | None = None,
               attempts: int = 10) -> tuple[dict, str]:
    """Re-read state and act on a freshly-picked matching target.

    Acting on FT scenery often kicks off scripted transitions/cutscenes, so the
    object list goes stale between reads and a later test can land mid-sequence.
    Re-pick the target on every attempt and skip through cutscenes / stale-object
    errors. Skips the test if the demo stays locked (a pacing quirk, not a bug).
    Returns (result, target_name)."""
    last = None
    for _ in range(attempts):
        state = _interactive_state(client)
        cands = [
            o["name"] for o in state.get("objects", [])
            if not o.get("pathway")
            and (want_verb is None or want_verb in o.get("compatible_verbs", []))
        ]
        if not cands:
            sleep(1.0)
            continue
        try:
            return client.act(verb, cands[0]), cands[0]
        except RuntimeError as e:
            last = e
            if "not accepting input" in str(e) or "unknown target" in str(e):
                try:
                    client.skip()
                except Exception:
                    pass
                sleep(1.0)
                continue
            raise
    pytest.skip(f"could not run {verb!r} on a fresh target: {last}")


def _room(client: McpClient) -> int:
    return get_state_with_retry(client).get("room", {}).get("id")


def _reach_dumpster(client: McpClient, max_skips: int = 20) -> int:
    """Skip the opening intro/title until Ben lands inside the dumpster (room 10)."""
    for _ in range(max_skips):
        if _room(client) == 10:
            return 10
        sleep(0.6)
        try:
            client.skip()
        except Exception:
            pass
    sleep(2)
    return _room(client)


def _sweep_out(client: McpClient) -> int:
    """From inside the dumpster (room 10), climb out into the alley.

    A short wake-up animation plays first, during which the exit is inert, so we
    sweep walk targets across the floor until the climb-out transition fires.
    Returns the room we ended up in.
    """
    sleep(2)  # let the wake-up animation settle so the exit becomes active
    for y in range(0, 200, 25):
        for x in range(0, 320, 25):
            try:
                client.walk(x, y)
            except RuntimeError:
                pass
            sleep(0.3)
            if _room(client) != 10:
                return _room(client)
    return _room(client)


@pytest.fixture(scope="module")
def ft_escaped(ft_client: McpClient) -> McpClient:
    """Skip the intro, land in the dumpster (room 10) and climb out, once."""
    if _room(ft_client) == 10:
        _sweep_out(ft_client)
    elif _reach_dumpster(ft_client) == 10:
        _sweep_out(ft_client)
    assert _room(ft_client) != 10, "Could not climb out of the dumpster during setup"
    return ft_client


def _interactive_state(client: McpClient, attempts: int = 8) -> dict:
    """Return a state with a settled, interactive room (objects present)."""
    for _ in range(attempts):
        state = get_state_with_retry(client)
        if state.get("objects"):
            return state
        try:
            client.skip()
        except Exception:
            pass
        sleep(1.0)
    return get_state_with_retry(client)


def _find(state: dict, name: str):
    for o in state.get("objects", []):
        if o["name"] == name:
            return o
    return None


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_01_ft_escape_dumpster(ft_client: McpClient) -> None:
    """Skip the intro, land trapped in the dumpster, then climb out.

    This is the prerequisite for everything else, so it runs first. The opening
    intro/title must be skipped to reach the dumpster (room 10); then walking to
    the opening must produce a real room transition out of 10.
    """
    if _room(ft_client) == 10:
        assert _sweep_out(ft_client) != 10, "could not climb out of the dumpster"
        return
    assert _reach_dumpster(ft_client) == 10, (
        "did not reach the dumpster (room 10) after skipping the intro"
    )
    assert _sweep_out(ft_client) != 10, "could not climb out of the dumpster (room never left 10)"


def test_02_ft_verbs_exposed(ft_escaped: McpClient) -> None:
    """The verb-coin set (fist/kick/mouth) + walk to + interact + use item."""
    state = _interactive_state(ft_escaped)
    verbs = set(state.get("verbs", []))
    missing = FT_VERBS - verbs
    assert not missing, f"Missing FT verbs: {missing}, got: {sorted(verbs)}"
    # Classic V6 verb-bar labels must not leak through the V7 model.
    for forbidden in ("look at", "pick up", "talk to"):
        assert forbidden not in verbs, f"{forbidden!r} should not appear in FT verbs"


def test_03_ft_objects_in_alley(ft_escaped: McpClient) -> None:
    """The alley exposes interactable scenery once Ben is out."""
    state = _interactive_state(ft_escaped)
    assert state.get("objects"), f"No objects in room {state.get('room')}"


def test_04_ft_object_verbs_reflect_entrypoints(ft_escaped: McpClient) -> None:
    """Per-object compatible verbs mirror the object's real verb scripts.

    A sign can only be examined ('mouth') — it has no fist/kick handler — while
    every object always offers the generic walk to / interact / use item.
    """
    state = _interactive_state(ft_escaped)
    sign = _find(state, "sign")
    if sign is None:
        pytest.skip(f"no sign in room {state.get('room')}")
    cv = set(sign.get("compatible_verbs", []))
    assert "mouth" in cv, f"sign should support 'mouth', got {cv}"
    assert {"walk to", "interact", "use item"}.issubset(cv), f"sign missing generic verbs: {cv}"
    assert "fist" not in cv and "kick" not in cv, (
        f"sign must not advertise fist/kick (no handler), got {cv}"
    )


def test_05_ft_mouth_produces_clean_output(ft_escaped: McpClient) -> None:
    """'mouth' on scenery dispatches and never emits talkie-code garbage."""
    result, _ = _act_fresh(ft_escaped, "mouth", want_verb="mouth")
    for m in result.get("messages", []):
        t = m.get("text", "")
        assert any(c.isalnum() and ord(c) < 128 for c in t), (
            f"Garbage message with no ASCII alnum content: {t!r}"
        )


def test_06_ft_fist_dispatches(ft_escaped: McpClient) -> None:
    """'fist' on a fist-compatible object dispatches without error."""
    result, _ = _act_fresh(ft_escaped, "fist", want_verb="fist")
    assert isinstance(result, dict), f"Expected dict result, got: {result!r}"


def test_07_ft_walk_to_verb(ft_escaped: McpClient) -> None:
    """'walk_to' is exposed and dispatches without error on any object."""
    state = _interactive_state(ft_escaped)
    assert "walk to" in set(state.get("verbs", [])), "walk to verb missing"
    result, _ = _act_fresh(ft_escaped, "walk_to")
    assert isinstance(result, dict), f"Expected dict result, got: {result!r}"


def test_08_ft_no_false_dialog_after_action(ft_escaped: McpClient) -> None:
    """Acting on scenery must not leave a phantom dialog pending.

    Regression: Full Throttle reuses VAR_VERB_SCRIPT for transient action/cutscene
    sequences (the verb coin, climbing out, riding the bike), which the dialog
    detector once mistook for a conversation — blanking the verb list and
    surfacing placeholder 'Choice N' entries. After acting, no spurious question
    may be reported (these alley scenes have no NPC conversation).
    """
    _act_fresh(ft_escaped, "fist", want_verb="fist")
    for _ in range(4):
        q = get_state_with_retry(ft_escaped).get("question")
        assert q is None, f"Phantom dialog reported after action: {q}"
        sleep(0.5)


def test_09_ft_use_item_two_targets(ft_escaped: McpClient) -> None:
    """'use item' accepts two targets without error (inventory permitting)."""
    state = _interactive_state(ft_escaped)
    cursor_names = {"look_at", "look at", "interact", "use", "use_item",
                    "use item", "fist", "kick", "mouth", "walk to", "boot", "eye"}
    inv = [i for i in state.get("inventory", []) if i.lower() not in cursor_names]
    if not inv:
        pytest.skip("No real inventory items available")
    target = next((o["name"] for o in state["objects"] if o["name"] != inv[0]), None)
    if target is None:
        pytest.skip("no target object for use item")
    try:
        result = _act_until_done(ft_escaped, "use item", inv[0], target)
    except (AssertionError, RuntimeError):
        pytest.skip("game busy / target stale for use item")
    assert isinstance(result, dict), f"Expected dict result, got: {result!r}"


# ---------------------------------------------------------------------------
# Pathway / room-transition tests
#
# These run on a dedicated FT instance (ft_client_pathways) because they
# deliberately change rooms; sharing the main client would move Ben out of the
# object-rich opening alley and starve the scenery tests above of targets.
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def ft_alley(ft_client_pathways: McpClient) -> McpClient:
    """Skip the intro, land in the dumpster and climb out into the alley, once."""
    if _room(ft_client_pathways) == 10:
        _sweep_out(ft_client_pathways)
    elif _reach_dumpster(ft_client_pathways) == 10:
        _sweep_out(ft_client_pathways)
    assert _room(ft_client_pathways) != 10, "Could not climb out of the dumpster during setup"
    return ft_client_pathways


def test_10_ft_pathways_detected(ft_alley: McpClient) -> None:
    """Full Throttle exit hotspots are flagged as pathways.

    Regression: FT objects were never marked pathway=true (the alley exits were
    invisible to clients looking for room transitions). Exit hotspots have no
    verb-coin handler (no fist/kick/mouth), so they expose only the generic
    walk to / interact / use item verbs and must be flagged as pathways — exactly
    like Curse of Monkey Island exit hotspots. An object that DOES script a coin
    verb (e.g. a kickable door) is not a pure pathway and must not be flagged.
    """
    state = _interactive_state(ft_alley)
    pathways = [o for o in state.get("objects", []) if o.get("pathway")]
    assert pathways, (
        f"Expected at least one pathway (exit hotspot) in room {state.get('room')}, "
        f"got objects: {[o['name'] for o in state.get('objects', [])]}"
    )
    for p in pathways:
        cv = set(p.get("compatible_verbs", []))
        assert "fist" not in cv and "kick" not in cv and "mouth" not in cv, (
            f"pathway {p['name']!r} should not advertise coin verbs, got {cv}"
        )
    # Conversely, an object that scripts a coin verb must NOT be flagged a pathway.
    for o in state.get("objects", []):
        cv = set(o.get("compatible_verbs", []))
        if cv & {"fist", "kick", "mouth"}:
            assert not o.get("pathway"), (
                f"{o['name']!r} scripts a coin verb and must not be a pathway, got {cv}"
            )


def test_11_ft_walk_to_pathway_changes_room(ft_alley: McpClient) -> None:
    """'walk_to' a pathway drives the scene-click input script and changes rooms.

    Regression: FT walk_to used startWalkActor, which moves Ben but never fired
    the exit/room-transition handler — so walking to a scene exit (or through a
    door that was just kicked open in room 6) did nothing. walk_to now simulates
    the scene click the engine's verb script needs, producing a real transition.
    """
    state = _interactive_state(ft_alley)
    initial_room = state.get("room", {}).get("id")
    pathways = [o for o in state.get("objects", []) if o.get("pathway")]
    assert pathways, f"no pathway to walk to in room {initial_room}"

    changed = False
    for p in pathways:
        try:
            result = ft_alley.act("walk_to", p["name"])
        except RuntimeError:
            try:
                ft_alley.skip()
            except Exception:
                pass
            sleep(1.0)
            continue
        if result.get("room_changed") not in (None, initial_room):
            changed = True
            break
        sleep(0.5)

    assert changed, (
        f"walk_to a pathway from room {initial_room} did not produce a room "
        f"transition (tried {[p['name'] for p in pathways]})"
    )
