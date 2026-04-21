## ScummVM MCP Protocol

This directory contains machine-readable and human-readable descriptions of the MCP (Model Context Protocol) server built into the ScummVM SCUMM engine.

### Files

- `mcp.json` — JSON description of the MCP protocol: transport, server identity, tool schemas, and request/response examples.
- `USAGE_NETWORK.md` — How to enable the server, configure the port, and connect a client.
- `client_tcp.py` — Minimal Python MCP client for manual testing.
- `start_scummvm_tcp.py` — Legacy stdio-over-TCP proxy (deprecated; the engine now has a built-in TCP server).
- `start_scummvm_fifo.sh` — Legacy FIFO/pipe helper (deprecated).

### Quick facts

- **Transport**: Streamable HTTP — JSON-RPC 2.0 over plain HTTP with SSE for streaming responses (MCP spec 2025-03-26).
- **Bind address**: `127.0.0.1` (localhost only).
- **Default port**: `23456`.
- **Protocol version**: `2025-03-26`.
- **Server identity**: `{ "name": "scummvm", "version": "1.0" }`.
- **Supported games**: All SCUMM engine games (MI1, MI2, Indiana Jones, Day of the Tentacle, Sam & Max, etc.). MI1 is the primary tested game.

### Activation

Add `mcp=true` to the game's section in `scummvm.ini`:

```ini
[monkey1]
mcp=true
```

Override the port with `mcp_port`:

```ini
[monkey1]
mcp=true
mcp_port=23456
```

### Tools

| Tool    | Type      | Description |
|---------|-----------|-------------|
| `state`  | sync      | Read room, position, verbs, inventory, objects, actors, messages, and pending dialog question |
| `act`    | streaming | Execute a verb on a named object or actor; blocks until complete |
| `answer` | streaming | Select a dialog choice by 1-based ID; blocks until conversation completes |
| `walk`   | streaming | Walk to explicit pixel coordinates (auto-clamped to room bounds); blocks until complete |

Streaming tools open an SSE channel and emit `notifications/message` events as dialog plays out, then close the stream with a structured result.

### Architecture

The server is split across two modules:

- `backends/networking/mcp/mcp_server.cpp` — engine-agnostic transport layer (TCP listener, HTTP parser, JSON-RPC dispatch, SSE streaming, session management).
- `engines/scumm/mcp.cpp` — SCUMM-specific bridge (tool implementations, entity resolution, verb lookup, action-completion detection, dialog message capture).
