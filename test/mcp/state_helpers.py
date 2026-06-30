#!/usr/bin/env python3
"""Shared state / verb / dialog inspection helpers for the integration tests.

Pure helpers over the state and tool-result dicts (object/verb/choice lookups),
plus the interactive-readiness polls and the ``skip``/``require`` guards that
several test files share. Imported via ``utils`` for back-compat.
"""

from __future__ import annotations

import time
from typing import TYPE_CHECKING, Any, Protocol

import httpx

if TYPE_CHECKING:
    from mcp_client import McpClient


class VerbActor(Protocol):
    """Anything with an ``act(verb, target1, target2)`` method (e.g. McpClient).

    Lets :func:`bind_verb` / :func:`make_verbs` be type-checked against test
    doubles that record calls, not just the live :class:`McpClient`.
    """

    def act(
        self,
        verb: str,
        target1: str | int | None = ...,
        target2: str | int | None = ...,
    ) -> dict[str, Any]: ...


def bind_verb(client: VerbActor, verb: str):
    """Return a callable invoking ``client.act(verb, *targets)``."""
    return lambda *targets: client.act(verb, *targets)


def make_verbs(client: VerbActor, *verb_names: str) -> tuple:
    """Bind verb names to *client* so tests read ``use(a, b)`` for ``act("use", a, b)``.

    ``make_verbs(client, "pick_up", "use")`` returns two callables where
    ``use(*targets)`` is exactly ``client.act("use", *targets)``. It always
    returns a tuple so call sites stay verb-first regardless of how many verbs
    are bound (``give = bind_verb(client, "give")``).
    """
    return tuple(bind_verb(client, name) for name in verb_names)


def find_id(state: dict, name: str) -> int | None:
    """Return the id of the first object whose name exactly matches *name*."""
    obj = _find_object(state, name)
    return obj["id"] if obj else None


def object_names(state: dict) -> set:
    """Return the set of object names visible in *state*."""
    return {obj["name"] for obj in state.get("objects", [])}


def object_by_id(state: dict, obj_id: int) -> dict | None:
    """Return the first object whose id matches *obj_id*, or None."""
    for obj in state.get("objects", []):
        if obj.get("id") == obj_id:
            return obj
    return None


def pathways(state: dict) -> list:
    """Return the objects flagged as pathways (scene exits) in *state*."""
    return [obj for obj in state.get("objects", []) if obj.get("pathway")]


def message_texts(result: dict) -> list:
    """Return the text of every message in *result* (missing text becomes '')."""
    return [message.get("text", "") for message in result.get("messages", [])]


def joined_message_text(result: dict) -> str:
    """Return every message text in *result* joined by single spaces."""
    return " ".join(message_texts(result))


def choice_labels(question: dict | None) -> list:
    """Return the label of every choice in *question*."""
    return [choice.get("label") for choice in (question or {}).get("choices", [])]


def find_choice_id(question: dict, label: str) -> int | None:
    """Return the id of the dialog choice whose label exactly matches *label*."""
    for choice in (question or {}).get("choices", []):
        if choice["label"] == label:
            return choice["id"]
    return None


def find_choice_id_containing(question: dict, needle: str) -> int | None:
    """Return the id of the first dialog choice whose label contains *needle*.

    Matching is case-insensitive; returns None when no label matches.
    """
    lowered = needle.lower()
    for choice in (question or {}).get("choices", []):
        if lowered in choice["label"].lower():
            return choice["id"]
    return None


def find_object_by_name(state: dict, substring: str) -> str | None:
    """Return the first object name containing *substring* (case-insensitive)."""
    for obj in state.get("objects", []):
        if substring.lower() in obj["name"].lower():
            return obj["name"]
    return None


def find_object_with_verb(state: dict, verb: str) -> str | None:
    """Return the first non-pathway object that lists *verb* as compatible."""
    for obj in state.get("objects", []):
        if obj.get("pathway"):
            continue
        if verb in obj.get("compatible_verbs", []):
            return obj["name"]
    return None


def skip_intros(client: McpClient, max_skips: int = 20, poll_secs: float = 1.0) -> None:
    """Send repeated skip commands to advance past SMUSH/intro videos.

    May block until a video finishes, so timeouts are silently ignored.
    """
    for _ in range(max_skips):
        time.sleep(poll_secs)
        try:
            client.skip()
        except Exception:
            pass


def wait_for_interactive(client: McpClient, timeout: float = 120.0) -> bool:
    """Poll with skips until walk() succeeds (game accepts input)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(1.0)
        try:
            client.skip()
        except Exception:
            pass
        try:
            state = client.state()
            pos = state.get("position", {})
            x, y = pos.get("x", 160), pos.get("y", 100)
            client.walk(x, y)
            return True
        except RuntimeError as e:
            if "not accepting input" in str(e):
                continue
            return True
        except Exception:
            continue
    return False


def require_interactive(
    client: McpClient,
    message: str = "game did not reach an interactive state",
    timeout: float = 120.0,
) -> None:
    """Skip the current test unless the game reaches an interactive state."""
    import pytest

    if not wait_for_interactive(client, timeout=timeout):
        pytest.skip(message)


def skip_unless(condition: bool, message: str) -> None:
    """Skip the current test unless *condition* holds."""
    import pytest

    if not condition:
        pytest.skip(message)


def wait_until_or_skip(predicate, message: str, timeout: float = 10.0) -> None:
    """Skip the current test unless *predicate* becomes true within *timeout*."""
    import pytest

    if not _wait_until(predicate, timeout=timeout):
        pytest.skip(message)


def get_state_with_retry(client: McpClient, max_attempts: int = 5) -> dict:
    """Call state() with retries for ReadTimeout (cutscene in progress)."""
    for attempt in range(max_attempts):
        try:
            return client.state()
        except (httpx.ReadTimeout, httpx.ConnectTimeout):
            if attempt == max_attempts - 1:
                raise
            time.sleep(2.0)
    raise RuntimeError("state() failed after retries")


def _wait_until(predicate, timeout: float = 10.0, poll: float = 0.5) -> bool:
    """Wait until *predicate()* returns True or timeout expires."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if predicate():
                return True
        except (httpx.ReadTimeout, httpx.ConnectTimeout):
            pass
        time.sleep(poll)
    return False


def _state_or_skip(client: McpClient, retries: int = 5) -> dict:
    """Return state() or skip the test if it can't be read."""
    import pytest

    for _ in range(retries):
        try:
            return client.state()
        except (httpx.ReadTimeout, httpx.ConnectTimeout):
            time.sleep(1.0)
    pytest.skip("could not read state")


def _find_object(state: dict, name: str) -> dict | None:
    """Return the first object whose name exactly matches *name*."""
    for obj in state.get("objects", []):
        if obj["name"] == name:
            return obj
    return None
