Monkey MCP protocol

This directory contains the machine-readable description of the Monkey Island 1 MCP (Model Context Protocol) bridge implemented in the Scumm engine.

File:
- monkey_mcp.json — JSON description of the MCP protocol, tools, schemas and examples.

Notes:
- The bridge uses JSON-RPC 2.0 over stdio (stdin/stdout).
- To enable the bridge, set 'monkey_mcp=true' in scummvm configuration for the Monkey Island 1 game entry.
- The bridge is intentionally opt-in and limited to Monkey Island 1. It does not open network sockets.

If you prefer an OpenAPI/Swagger representation or separate JSON Schema files for each tool, we can convert and add them here.