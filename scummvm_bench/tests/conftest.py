"""Shared pytest fixtures and the opt-in gate for real-ScummVM tests."""

import pytest
from mock_harness import monkey_backend

from scummvm_bench.goals import get_goal_set
from scummvm_bench.goals.engine import GoalSet


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--run-real",
        action="store_true",
        default=False,
        help="run the slow tests that launch a real ScummVM instance",
    )


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line(
        "markers",
        "real: launches a real ScummVM (slow; opt-in via --run-real)",
    )


def pytest_collection_modifyitems(
    config: pytest.Config, items: list[pytest.Item]
) -> None:
    if config.getoption("--run-real"):
        return
    skip_real = pytest.mark.skip(reason="needs --run-real")
    for item in items:
        if "real" in item.keywords:
            item.add_marker(skip_real)


@pytest.fixture
def monkey_goalset() -> GoalSet:
    return get_goal_set("monkey-ega-demo", 1)


@pytest.fixture
def backend():
    return monkey_backend()
