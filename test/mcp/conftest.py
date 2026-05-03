"""
Pytest fixtures for MCP integration tests.
"""

import os
import pytest

from utils import (
    McpClient,
    launch_scummvm,
    wait_for_mcp,
    require_game_path,
    GAME_PATHS,
    MCP_HOST,
    MCP_CONNECT_TIMEOUT_SECS,
)

PROC_KILL_TIMEOUT_SECS = 5


@pytest.fixture(scope="session")
def monkey_client() -> McpClient:
    """Launch Monkey Island 1 EGA demo and return MCP client."""
    mcp_port = 23456

    require_game_path("monkey-ega-demo")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "monkey-ega-demo",
        GAME_PATHS["monkey-ega-demo"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    # Close log file handles
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def maniac_client() -> McpClient:
    """Launch Maniac Mansion C64 demo and return MCP client."""
    mcp_port = 23457

    require_game_path("maniac-c64")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "maniac-c64",
        GAME_PATHS["maniac-c64"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    # Close log file handles
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def atlantis_client() -> McpClient:
    """Launch Indiana Jones Fate of Atlantis demo and return MCP client."""
    mcp_port = 23458

    require_game_path("atlantis")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "atlantis", GAME_PATHS["atlantis"], port=mcp_port, scummvm_binary=scummvm_binary
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    # Close log file handles
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def samnmax_client() -> McpClient:
    """Launch Sam & Max Hit the Road demo and return MCP client."""
    mcp_port = 23459

    require_game_path("samnmax")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "samnmax",
        GAME_PATHS["samnmax"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def ft_client() -> McpClient:
    """Launch Full Throttle DOS demo and return MCP client."""
    mcp_port = 23461

    require_game_path("ft-demo")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "ft-demo",
        GAME_PATHS["ft-demo"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def indy3_client() -> McpClient:
    """Launch Passport to Adventure (Indy3 segment, save slot 3) and return MCP client.

    Save slot 3 (pass.s03) drops Indy at the boxing gym, ready to walk to the
    locker room and start a fight.
    """
    mcp_port = 23463

    require_game_path("pass")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "pass",
        GAME_PATHS["pass"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
        save_slot=3,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def indy3_travel_client() -> McpClient:
    """Launch Passport to Adventure (Indy3 segment, save slot 4) for travel tests.

    Save slot 4 (pass.s04) drops Indy in the Pan Am clipper (room 24) where the
    'Travel' verb on the verb bar opens the destination dialog
    ('To Henry's House' / 'Cancel').
    """
    mcp_port = 23464

    require_game_path("pass")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "pass",
        GAME_PATHS["pass"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
        save_slot=4,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def loom_client() -> McpClient:
    """Launch Passport to Adventure (Loom segment) demo and return MCP client."""
    mcp_port = 23462

    require_game_path("pass")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "pass",
        GAME_PATHS["pass"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def dig_client() -> McpClient:
    """Launch The Dig demo and return MCP client."""
    mcp_port = 23460

    require_game_path("dig-demo")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "dig-demo",
        GAME_PATHS["dig-demo"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()


@pytest.fixture(scope="session")
def comi_client() -> McpClient:
    """Launch Curse of Monkey Island demo and return MCP client."""
    mcp_port = 23469

    require_game_path("comi-demo")
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "comi-demo",
        GAME_PATHS["comi-demo"],
        port=mcp_port,
        scummvm_binary=scummvm_binary,
    )
    client = wait_for_mcp(MCP_HOST, mcp_port, timeout=MCP_CONNECT_TIMEOUT_SECS)
    yield client
    client.close()
    proc.kill()
    proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
    if hasattr(proc, "_stdout_file"):
        proc._stdout_file.close()
    if hasattr(proc, "_stderr_file"):
        proc._stderr_file.close()
