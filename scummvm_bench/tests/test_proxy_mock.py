"""The proxy dispatch path: forwarding, failure classification, state caching."""

import asyncio

from fastmcp import Client
from mock_harness import monkey_backend

from scummvm_bench.backend import MockBackend, ScriptStep
from scummvm_bench.goals import get_goal_set
from scummvm_bench.proxy import BenchProxy
from scummvm_bench.recorder import LimitConfig, Recorder


def _proxy(backend, limits: LimitConfig | None = None) -> tuple[BenchProxy, Recorder]:
    goal_set = get_goal_set("monkey-ega-demo", 1)
    recorder = Recorder(goal_set, limits or LimitConfig())
    proxy = BenchProxy(backend, recorder, goal_set)
    proxy.prime()
    return proxy, recorder


def test_dispatch_forwards_and_records_success() -> None:
    proxy, recorder = _proxy(monkey_backend())
    result = proxy.dispatch("walk", {"x": 120, "y": 132})
    assert result["messages"][0]["text"] == "None shall pass!"
    assert recorder.call_count == 1
    assert recorder._calls[-1].ok is True


def test_dispatch_classifies_invalid_request() -> None:
    proxy, recorder = _proxy(monkey_backend())
    result = proxy.dispatch("act", {"verb": "frobnicate"})
    assert result == {"error": "invalid_request"}
    assert recorder._calls[-1].failure == "invalid_request"
    assert recorder._calls[-1].ok is False


def test_dispatch_updates_room_cache_from_room_changed() -> None:
    proxy, _ = _proxy(monkey_backend())
    proxy.dispatch("act", {"verb": "walk_to", "target1": "door"})  # -> room 52
    assert proxy._state_cache["room"] == {"id": 52}


def test_dispatch_updates_cache_from_state_call() -> None:
    proxy, _ = _proxy(monkey_backend())
    proxy.dispatch("act", {"verb": "walk_to", "target1": "door"})  # -> 52
    snapshot = proxy.dispatch("state", {})
    assert snapshot["room"] == {"id": 52}
    assert proxy._state_cache["room"] == {"id": 52}


def test_record_invalid_request_helper() -> None:
    proxy, recorder = _proxy(monkey_backend())
    proxy.record_invalid_request("act", {"verb": "?"})
    assert recorder.call_count == 1
    assert recorder._calls[-1].failure == "invalid_request"


def test_invalid_response_classification() -> None:
    from scummvm_bench.backend import BackendInvalidResponse

    class BadBackend:
        def start(self) -> None: ...

        def stop(self) -> None: ...

        def state(self) -> dict:
            return {"room": {"id": 1}}

        def call(self, tool: str, args: dict) -> dict:
            raise BackendInvalidResponse("garbage")

    proxy, recorder = _proxy(BadBackend())
    result = proxy.dispatch("act", {"verb": "look"})
    assert result == {"error": "invalid_response"}
    assert recorder._calls[-1].failure == "invalid_response"


def test_in_memory_client_lists_all_tools() -> None:
    proxy, _ = _proxy(monkey_backend())

    async def go() -> list[str]:
        async with Client(proxy.app) as client:
            return sorted(t.name for t in await client.list_tools())

    names = asyncio.run(go())
    assert {"state", "act", "answer", "walk", "skip"} <= set(names)


def test_stop_callback_fires_on_stopping_goal() -> None:
    stopped: list[object] = []
    backend = MockBackend(
        [
            ScriptStep(
                "act",
                {"verb": "talk_to", "target1": "Troll"},
                {"messages": [{"text": "I don't know how you did it, but you did it"}]},
            )
        ],
        initial_room=55,
    )
    goal_set = get_goal_set("monkey-ega-demo", 1)
    recorder = Recorder(goal_set, LimitConfig())
    proxy = BenchProxy(backend, recorder, goal_set, on_stop=stopped.append)
    proxy.prime()
    proxy.dispatch("act", {"verb": "talk_to", "target1": "Troll"})
    assert len(stopped) == 1
