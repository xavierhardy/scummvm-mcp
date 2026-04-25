"""
Pytest fixtures for MCP integration tests.
"""

import os
import subprocess
import time

import pytest

from utils import (
    McpClient,
    launch_scummvm,
    wait_for_mcp,
    require_game_path,
    GAME_PATHS,
)


@pytest.fixture(scope="session")
def monkey_client() -> McpClient:
    """Launch Monkey Island 1 EGA demo and return MCP client."""
    require_game_path("monkey-ega-demo")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "monkey-ega-demo",
        GAME_PATHS["monkey-ega-demo"],
        port=23456,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp("127.0.0.1", 23456, timeout=30.0)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=5)


@pytest.fixture(scope="session")
def maniac_client() -> McpClient:
    """Launch Maniac Mansion C64 demo and return MCP client."""
    require_game_path("maniac-c64")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "maniac-c64",
        GAME_PATHS["maniac-c64"],
        port=23457,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp("127.0.0.1", 23457, timeout=30.0)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=5)


@pytest.fixture(scope="session")
def atlantis_client() -> McpClient:
    """Launch Indiana Jones Fate of Atlantis demo and return MCP client."""
    require_game_path("atlantis")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "atlantis", GAME_PATHS["atlantis"], port=23458, scummvm_binary=scummvm_binary
    )
    client = wait_for_mcp("127.0.0.1", 23458, timeout=30.0)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=5)
