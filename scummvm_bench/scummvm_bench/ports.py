"""Free-port allocation for parallel sessions.

Each session needs two distinct local ports: one for the FastMCP proxy the
harness connects to, and one for the ScummVM MCP server behind it. Ports are
discovered by binding ``:0``; there is a small TOCTOU window, so callers should
retry on ``EADDRINUSE`` when they actually bind.
"""

import socket


def find_free_port() -> int:
    """Return a currently-free TCP port on the loopback interface."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def reserve_pair() -> tuple[int, int]:
    """Return two distinct free ports (bench_port, scummvm_port)."""
    bench_port = find_free_port()
    scummvm_port = find_free_port()
    while scummvm_port == bench_port:
        scummvm_port = find_free_port()
    return bench_port, scummvm_port
