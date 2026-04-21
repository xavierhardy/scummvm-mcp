## Using the built-in TCP MCP server

### Overview

The ScummVM SCUMM engine includes a built-in non-blocking MCP server. It runs inside the game process on the main thread, accepting connections and processing JSON-RPC requests once per game frame. External tools (AI agents, scripts, debuggers) can connect over localhost and issue MCP tool calls to observe and control the running game.

The server implements the MCP Streamable HTTP protocol (2025-03-26): sync tools return JSON immediately; streaming tools (act, answer, walk) upgrade the connection to SSE and push `notifications/message` events as dialog plays out before sending the final result.

### Enable the server

Add the following to the game's section in `scummvm.ini`:

```ini
[monkey1]
mcp=true
```

To use a custom port (default is **23456**):

```ini
[monkey1]
mcp=true
mcp_port=23456
```

Notes:
- `mcp=true` is required to start the server. It is disabled by default.
- `mcp_port` must be a positive integer. Defaults to `23456` if omitted.
- The server always binds to `127.0.0.1` (localhost only).
- The server is available for all SCUMM engine games, not just Monkey Island 1.

### Start ScummVM

Run ScummVM from a terminal. Add `--debuglevel=1` to see MCP debug messages:

```bash
./scummvm --debuglevel=1 monkey1
```

You should see a line like:

```
mcp: listening on 127.0.0.1:23456 (fd=NN)
```

### Connect with netcat (quick test)

Send an `initialize` request to confirm the server is up:

```bash
printf 'POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 87\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}}' | nc 127.0.0.1 23456
```

Expected response contains:

```json
{ "jsonrpc": "2.0", "id": 1, "result": { "protocolVersion": "2025-03-26", "serverInfo": { "name": "scummvm", "version": "1.0" } } }
```

### Use with an MCP client

Configure your MCP client to use the HTTP transport:

```json
{
  "mcpServers": {
    "scummvm": {
      "transport": "http",
      "url": "http://127.0.0.1:23456/mcp"
    }
  }
}
```

### Behavior and safety notes

- **Single-client**: The server handles one active client at a time. When a client disconnects, the next connection is accepted on the following `pump()` call.
- **Non-blocking**: All I/O is non-blocking and happens on the main game thread inside the regular `pump()` call — no threads, no stalls.
- **Localhost only**: The server binds to `127.0.0.1`. There is no authentication; only expose it to trusted local processes.
- **Streaming tools**: `act`, `answer`, and `walk` hold the HTTP connection open via SSE. Only one streaming action can run at a time; concurrent tool calls are queued and dispatched after the current stream ends.
