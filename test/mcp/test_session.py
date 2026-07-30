"""Multi-session integration tests.

Verifies that two clients can each hold a session simultaneously against one
ScummVM instance — the fix for the single-session limitation that caused the
observer to be evicted whenever a real agent connected.
"""

import os

import pytest

from utils import (
    GAME_PATHS,
    MCP_HOST,
    McpClient,
    get_mcp_port,
    launch_scummvm,
    require_game_path,
    wait_for_mcp,
)

PROC_KILL_TIMEOUT_SECS = 5


def _server_handle():
    """Launch one ScummVM instance and yield the port + proc handle.

    Reused by every test in this file so we only need one running instance.
    """
    require_game_path("monkey-ega-demo")
    port = get_mcp_port(30)  # stable fixture index outside the main pool
    scummvm_binary = os.path.join(os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "monkey-ega-demo",
        GAME_PATHS["monkey-ega-demo"],
        port=port,
        scummvm_binary=scummvm_binary,
        save_slot=1,
    )
    client = wait_for_mcp(MCP_HOST, port)
    client.close()
    return port, proc


@pytest.fixture(scope="module")
def server(request) -> tuple[int, object]:
    """Module-scoped ScummVM instance for all tests in this file."""
    import os as _os

    require_game_path("monkey-ega-demo")
    port = get_mcp_port(30)
    scummvm_binary = _os.path.join(_os.path.dirname(__file__), "..", "..", "scummvm")
    proc = launch_scummvm(
        "monkey-ega-demo",
        GAME_PATHS["monkey-ega-demo"],
        port=port,
        scummvm_binary=scummvm_binary,
        save_slot=1,
    )
    # Wait once for the server to be ready.
    c = wait_for_mcp(MCP_HOST, port)
    c.close()

    def fin():
        proc.kill()
        proc.wait(timeout=PROC_KILL_TIMEOUT_SECS)
        handles = getattr(proc, "_log_handles", None)
        if handles is not None:
            for h in handles:
                h.close()

    request.addfinalizer(fin)
    return port, proc


def test_two_clients_can_hold_sessions_simultaneously(server) -> None:
    """Two clients initialise against one instance; both call state successfully."""
    import os as _os

    port = server[0]
    client1 = McpClient(MCP_HOST, port)
    client2 = McpClient(MCP_HOST, port)

    client1.initialize()
    client2.initialize()

    # Both should have distinct session IDs.
    assert client1._session_id is not None
    assert client2._session_id is not None
    assert client1._session_id != client2._session_id, \
        "each client must hold a different session id"

    # Both should be able to call state.
    state1 = client1.state()
    assert "room" in state1, f"client1 state missing 'room': {state1}"

    state2 = client2.state()
    assert "room" in state2, f"client2 state missing 'room': {state2}"

    client1.close()
    client2.close()


def test_three_clients_all_active(server) -> None:
    """Three clients all hold sessions and get the same game state back."""
    port = server[0]
    clients = [McpClient(MCP_HOST, port) for _ in range(3)]
    for c in clients:
        c.initialize()

    # Verify each has a unique session.
    ids = [c._session_id for c in clients]
    assert len(set(ids)) == len(ids), "all three sessions must be distinct"

    # Each sees the same starting room.
    states = [c.state() for c in clients]
    rooms = [s.get("room", {}).get("id") for s in states]
    assert all(r == rooms[0] for r in rooms), \
        f"all clients should see the same starting room, got {rooms}"

    for c in clients:
        c.close()


def test_evict_oldest_session_when_at_capacity(server) -> None:
    """The server evicts the oldest session when a 5th client connects."""
    port = server[0]
    clients = [McpClient(MCP_HOST, port) for _ in range(5)]
    for c in clients:
        c.initialize()

    # The first session should have been evicted (only 4 slots).
    # We can verify by checking that client[0]'s session is no longer valid.
    first_session = clients[0]._session_id

    # The fifth client should have a valid session.
    assert clients[4]._session_id is not None
    assert clients[4]._session_id != first_session

    # The first client's session should now be rejected.
    import httpx
    try:
        clients[0].state()
        # If it doesn't raise, the session was NOT evicted — fail.
        # But only fail if we actually got a response (not a connection error).
        pytest.fail("expected session eviction but first client still works")
    except (httpx.HTTPError, RuntimeError, Exception):
        pass  # expected — session was evicted

    for c in clients:
        c.close()
