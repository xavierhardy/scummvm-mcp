"""Harness registry, attach mode, prompts, and pi pre-flight checks."""

import threading

import pytest

from scummvm_bench.harness import NoneHarness, PiHarness, make_harness
from scummvm_bench.harness.base import RunContext
from scummvm_bench.harness.pi import PiHarness as PiHarnessDirect
from scummvm_bench.harness.prompts import build_prompt
from scummvm_bench.models import RunSpec


def test_make_harness_known() -> None:
    assert isinstance(make_harness("none"), NoneHarness)
    assert isinstance(make_harness("pi"), PiHarness)


def test_make_harness_unknown() -> None:
    with pytest.raises(KeyError):
        make_harness("does-not-exist")


def test_none_harness_returns_when_stopped(tmp_path) -> None:
    stop = threading.Event()
    stop.set()
    spec = RunSpec("none", None, None, "monkey-ega-demo", 1)
    ctx = RunContext(spec, 0, str(tmp_path), stop, proxy=None)  # type: ignore[arg-type]
    harness = NoneHarness(max_wait_s=0.2)
    assert harness.run(ctx) is None


def test_pi_harness_requires_provider_and_model(tmp_path) -> None:
    spec = RunSpec("pi", None, None, "monkey-ega-demo", 1)
    ctx = RunContext(spec, 12345, str(tmp_path), threading.Event(), proxy=None)  # type: ignore[arg-type]
    error = PiHarnessDirect().run(ctx)
    assert error is not None
    assert "requires" in error


def test_pi_harness_writes_mcp_json(tmp_path) -> None:
    import json

    harness = PiHarnessDirect()
    harness._write_mcp_json(23456, str(tmp_path))
    data = json.loads((tmp_path / ".mcp.json").read_text())
    server = data["mcpServers"]["scummvm"]
    assert server["url"] == "http://127.0.0.1:23456/mcp"
    assert "state" in server["directTools"]


def test_build_prompt_for_monkey() -> None:
    prompt = build_prompt(RunSpec("pi", "openai", "gpt-4o", "monkey-ega-demo", 1))
    assert "Monkey Island" in prompt
    assert "MCP tools" in prompt


def test_build_prompt_fallback() -> None:
    prompt = build_prompt(RunSpec("pi", "openai", "gpt-4o", "unknown-game", None))
    assert "unknown-game" in prompt
