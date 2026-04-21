/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef BACKENDS_NETWORKING_MCP_MCP_SERVER_H
#define BACKENDS_NETWORKING_MCP_MCP_SERVER_H

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Networking {

// Engine-agnostic MCP (Model Context Protocol) server.
//
// Implements the transport layer for the MCP Streamable HTTP protocol
// (2025-03-26): TCP listener, HTTP request framing, JSON-RPC 2.0 dispatch,
// SSE streaming for long-running tool calls, and session management.
//
// Game/engine-specific tool handling is delegated to an IToolHandler
// implementation registered by the owning engine.
class McpServer {
public:
	class IToolHandler {
	public:
		virtual ~IToolHandler() {}

		// Invoked for each tool call. Behavior depends on the tool kind
		// declared in registerTool():
		//   - Sync tools MUST return a non-null result JSON value (ownership
		//     transferred to the server, which wraps it into the MCP content
		//     envelope). On validation error, return nullptr and set errorOut.
		//   - Streaming tools MUST call McpServer::startStreaming() from within
		//     this method and return nullptr. The server will keep the client
		//     connection open and invoke pumpStream() on each frame until the
		//     handler calls McpServer::endStream(). On validation error before
		//     startStreaming(), return nullptr and set errorOut.
		virtual Common::JSONValue *callTool(const Common::String &name,
		                                     const Common::JSONValue &args,
		                                     Common::String &errorOut) = 0;

		// Invoked each frame while a streaming tool call is in flight.
		// The handler should poll engine state and eventually call
		// McpServer::endStream() to conclude the call.
		virtual void pumpStream() = 0;
	};

	struct ToolSpec {
		Common::String name;
		Common::String description;
		Common::JSONValue *inputSchema;   // ownership transferred to server
		Common::JSONValue *outputSchema;  // ownership transferred to server (nullable)
		bool streaming;
	};

	McpServer(int port, const Common::String &serverName,
	          const Common::String &serverVersion);
	~McpServer();

	// Returns true if the listening socket was successfully bound.
	bool isListening() const;

	// Set the handler that receives tool calls. Non-owning.
	void setToolHandler(IToolHandler *handler);

	// Register a tool visible via tools/list. Takes ownership of schema values.
	void registerTool(const ToolSpec &spec);

	// Must be called once per game-loop frame.
	void pump();

	// --- Called from inside a streaming tool handler ---

	// Begin streaming for the currently-dispatched tool call. Opens SSE headers
	// on the originating client. Must only be called from within callTool().
	void startStreaming();

	// True while a streaming tool call is in flight.
	bool isStreaming() const;

	// Emit a notifications/message event on the active stream. `params` is
	// wrapped into a JSON-RPC notification envelope.
	void emitNotification(const Common::JSONObject &params);

	// Conclude the active stream. On success, `structuredResult` is wrapped
	// with content[] + structuredContent and sent as the JSON-RPC response.
	// On failure, a JSON-RPC error -32000 is sent with `errorMsg`. Ownership
	// of `structuredResult` transfers.
	void endStream(Common::JSONValue *structuredResult, bool success,
	               const Common::String &errorMsg = "");

private:
	struct MessageEntry {
		int actorId;
		Common::String text;
	};

	struct ClientEntry {
		int clientId;
		int fd;
		Common::String inBuffer;
		bool isGetSse;
		uint32 lastHeartbeatFrame;
	};

	struct PendingToolCall {
		int clientId;
		Common::String toolName;
		Common::JSONValue *args;
		Common::JSONValue *id;
	};

	int _port;
	Common::String _serverName;
	Common::String _serverVersion;
	IToolHandler *_handler;

	Common::Array<ToolSpec> _tools;

	int _listenFd;
	Common::String _sessionId;

	Common::Array<ClientEntry> _clients;
	int _nextClientId;
	int _activeFd;
	int _activeClientId;

	uint32 _nextSendMsgId;
	uint32 _nextRecvMsgId;

	uint32 _frameCounter;

	// --- Streaming state ---
	bool _sseActive;
	int _sseClientId;
	Common::JSONValue *_ssePendingId;
	uint32 _sseLastHeartbeatFrame;

	// Set to true while a tool handler is being dispatched (so startStreaming()
	// can attribute the stream to the correct client / request id).
	bool _inToolCall;
	int _toolCallClientId;
	Common::JSONValue *_toolCallId;

	// Deferred tool calls received while another stream was active.
	Common::Array<PendingToolCall> _toolQueue;

	// --- HTTP/SSE helpers ---
	bool sendRaw(const Common::String &data);
	void writeHttpResponse(int status, const Common::String &contentType,
	                       const Common::String &body,
	                       const Common::String &extraHeaders = "");
	void emitSseData(const Common::String &data);

	// --- Routing ---
	void handleHttpRequest(const Common::String &method,
	                       const Common::String &sessionHdr,
	                       const Common::String &body);
	void handleJsonRpc(const Common::String &body);
	Common::JSONValue *handleRequest(const Common::JSONValue &req, bool &startedStream);
	Common::JSONValue *handleInitialize();
	Common::JSONValue *handleToolsList();
	Common::JSONValue *handleToolCall(const Common::JSONValue &req, bool &startedStream);

	// --- Response helpers ---
	void writeJsonRpcResult(const Common::JSONValue *id,
	                        Common::JSONValue *result,
	                        const Common::String &extraHeaders = "");
	void writeJsonRpcError(const Common::JSONValue *id, int code,
	                       const Common::String &msg);

	// Drain queued tool calls (called after a stream ends).
	void drainToolQueue();
};

// -------- JSON schema helpers exposed for tool registration --------

Common::JSONValue *mcpJsonString(const Common::String &s);
Common::JSONValue *mcpJsonInt(int v);
Common::JSONValue *mcpJsonBool(bool v);

// Build a property descriptor: {"type": type, "description": desc?}
Common::JSONValue *mcpProp(const char *type, const char *desc = nullptr);

// Build a oneOf property: {"oneOf":[{type:t1},{type:t2}], "description": desc?}
Common::JSONValue *mcpPropOneOf(const char *type1, const char *type2,
                                 const char *desc = nullptr);

// Build an object schema from properties. If required names are supplied,
// adds a "required" array.
Common::JSONValue *mcpObjectSchema(Common::JSONObject &props,
                                    const char *const *required = nullptr,
                                    int reqCount = 0);

// Sanitize a string for safe JSON output (replaces invalid UTF-8).
Common::String mcpSanitizeString(const Common::String &s);

// Lowercased + trimmed copy.
Common::String mcpLowerTrimmed(const Common::String &s);

} // End of namespace Networking

#endif
