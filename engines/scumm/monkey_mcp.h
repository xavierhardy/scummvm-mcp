/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SCUMM_MONKEY_MCP_H
#define SCUMM_MONKEY_MCP_H

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Scumm {

class ScummEngine;
class Actor;

class MonkeyMcpBridge {
public:
	explicit MonkeyMcpBridge(ScummEngine *vm);
	~MonkeyMcpBridge();

	void pump();

	void onActorLine(int actorId, const Common::String &text);
	void onSystemLine(const Common::String &text);
	void onDialogPrompt(const Common::String &text);

	static Common::String normalizeActionName(const Common::String &action);

	// Accessor for protected getObjOrActorName used by safeObjName()
	const byte *callGetObjOrActorName(int obj) const {
		return _vm ? _vm->getObjOrActorName(obj) : nullptr;
	}

private:
	struct MessageEntry {
		uint64 seq;
		uint32 frame;
		byte room;
		int actorId;
		Common::String type;
		Common::String text;
	};

	// Flat record produced by buildEntityMap()
	struct NamedEntity {
		enum Kind { kInventory, kObject, kActor };
		Kind kind;
		int numId;                  // obj_nr for objects/inventory, actor._number for actors
		Common::String displayName; // deduplicated name (e.g. "sword" or "sword-42")
	};

	ScummEngine *_vm;
	bool _enabled;
	bool _initialized;
	int _listenFd;
	int _clientFd;       // short-lived POST connection
	int _getSseFd;       // persistent GET SSE connection for server notifications
	uint32 _getSseLastHeartbeatFrame;
	Common::String _inBuffer;
	Common::String _sessionId;

	Common::Array<MessageEntry> _messages;
	uint64 _nextMessageSeq;
	uint32 _frameCounter;

	// SSE streaming state (active while act/answer is executing)
	bool _sseActive;
	Common::JSONValue *_ssePendingId;    // owned deep copy of JSON-RPC id
	uint32 _sseStartFrame;
	uint32 _sseLastHeartbeatFrame;
	Common::Array<uint16> _ssePreInventory;
	int _ssePreRoom;
	int _ssePrePosX, _ssePrePosY;

	// ---- Lifecycle ----
	bool isMonkey1() const;
	void pushMessage(const char *type, int actorId, const Common::String &text);

	// ---- Transport ----
	void handleHttpRequest(const Common::String &method,
	                       const Common::String &sessionHdr,
	                       const Common::String &body);
	void writeHttpResponse(int status, const Common::String &contentType,
	                       const Common::String &body,
	                       const Common::String &extraHeaders = "");
	bool sendRaw(const Common::String &data);

	// SSE helpers (keep _clientFd open, no Content-Length)
	void writeSseHeaders();
	void emitSseData(const Common::String &data);
	void emitSseComment(const Common::String &comment);

	// ---- JSON-RPC dispatch ----
	void handleJsonRpc(const Common::String &body);
	Common::JSONValue *handleRequest(const Common::JSONValue &req);
	Common::JSONValue *handleInitialize(const Common::JSONValue &req);
	Common::JSONValue *handleToolsList();
	Common::JSONValue *handleToolCall(const Common::JSONValue &req);

	// ---- Tools ----
	Common::JSONValue *toolState(const Common::JSONValue &args);
	void toolAct(const Common::JSONValue &args, const Common::JSONValue *id);
	void toolAnswer(const Common::JSONValue &args, const Common::JSONValue *id);

	// ---- SSE lifecycle ----
	void pumpSse();
	void startSse(const Common::JSONValue *id);
	void closeSse(bool success, const Common::String &errorMsg = "");
	void snapshotPreAction();
	Common::JSONObject buildStateChanges() const;

	// ---- Game state helpers ----
	Actor *getEgoActor() const;
	bool hasPendingQuestion() const;
	bool isActionDone() const;

	void buildEntityMap(Common::Array<NamedEntity> &entities) const;
	bool resolveEntityByName(const Common::String &name, NamedEntity &out) const;
	bool resolveVerb(const Common::String &action, int &verbId) const;
	void buildChoices(Common::JSONArray &choices) const;

	// ---- Response helpers ----
	void writeJsonRpcResult(const Common::JSONValue *id, Common::JSONValue *result,
	                        const Common::String &extraHeaders = "");
	void writeJsonRpcError(const Common::JSONValue *id, int code, const Common::String &msg);
};

} // End of namespace Scumm

#endif
