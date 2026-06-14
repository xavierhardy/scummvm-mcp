"""Shared pytest fixtures."""

import pytest
from mock_harness import monkey_backend

from scummvm_bench.goals import get_goal_set
from scummvm_bench.goals.engine import GoalSet


@pytest.fixture
def monkey_goalset() -> GoalSet:
    return get_goal_set("monkey-ega-demo", 1)


@pytest.fixture
def backend():
    return monkey_backend()
