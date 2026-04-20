Using the built-in TCP MCP server

Overview

The Monkey MCP bridge in the Scumm engine can run as a non-blocking TCP server inside the game process. This allows external tools to connect over the network and issue JSON-RPC commands to control or inspect the running Monkey Island 1 game.

Enable the TCP server (runtime config)

Add the following to the Monkey game domain in your scummvm.ini, or to the global [scummvm] section:

[monkey]
monkey_mcp=true
monkey_mcp_port=12345
monkey_mcp_host=127.0.0.1

Notes:
- monkey_mcp must be true to enable the MCP bridge at all.
- monkey_mcp_port must be a positive integer. If unset or 0, the TCP server remains disabled and MCP uses stdin/stdout as before.
- monkey_mcp_host defaults to 0.0.0.0 if omitted (binds all interfaces). Use 127.0.0.1 to restrict to localhost.

Start scummvm and confirm the server is listening

Run scummvm (from a terminal) with debug enabled so you can see the MCP debug messages:

./scummvm --debugflags monkey_mcp,scumm --debuglevel 11 -c /path/to/scummvm.ini

Look for debug messages like:

monkey_mcp: listening on 127.0.0.1:12345 (fd=NN)

Connecting with netcat (for testing)

From another terminal on the host, you can connect with netcat and send a JSON-RPC "initialize" request:

printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' | nc 127.0.0.1 12345

You should receive a single line JSON response back with the serverInfo and protocolVersion.

Notes about behavior and safety

- Single-client model: The in-engine server accepts a single client connection at a time. When a client disconnects, a new client can connect.
- Non-blocking: All network operations are non-blocking and performed on the main engine thread inside the regular pump() call to avoid stalling the game loop.
- Security: Binding to all interfaces (0.0.0.0) exposes the MCP server to external networks. For local testing, bind to 127.0.0.1.

If you prefer a separate TCP proxy wrapper or multiple-client handling, consider using the provided docs/protocols/start_scummvm_tcp.py or an external proxy like socat.