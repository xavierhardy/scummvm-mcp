/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef SCUMM_MCP_H
#define SCUMM_MCP_H

#include "backends/networking/mcp/mcp_server.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Scumm {

class ScummEngine;
class Actor;
struct ObjectData;

class ScummMcpBridge : public Networking::McpServer::IToolHandler {
public:
	explicit ScummMcpBridge(ScummEngine *vm);
	~ScummMcpBridge() override;

	void pump();

	void onActorLine(int actorId, const Common::String &text);
	void onSystemLine(const Common::String &text);
	void onDialogPrompt(const Common::String &text);

	static Common::String normalizeActionName(const Common::String &action);

	// Accessor for protected getObjOrActorName used by helpers.
	const byte *callGetObjOrActorName(int obj) const;

	// IToolHandler
	Common::JSONValue *callTool(const Common::String &name,
	                             const Common::JSONValue &args,
	                             Common::String &errorOut) override;
	void pumpStream() override;

private:
	struct MessageEntry {
		uint64 seq;
		uint32 frame;
		byte room;
		int actorId;
		Common::String type;
		Common::String text;
	};

	struct NamedEntity {
		enum Kind { kInventory, kObject, kActor };
		Kind kind;
		int numId;
		Common::String displayName;
		bool visible;
		bool isPathway = false;
	};

	struct ObjStateSnap {
		int objNr;
		int state;
	};

	ScummEngine *_vm;
	bool _enabled;
	bool _skipToolEnabled;
	bool _debugToolsEnabled;
	Networking::McpServer *_server;

	Common::Array<MessageEntry> _messages;
	uint64 _nextMessageSeq;
	uint32 _frameCounter;

	// Streaming (action/answer/walk) state
	bool _streaming;
	uint32 _sseStartFrame;
	uint32 _sseDoneAtFrame;
	uint32 _sseStuckAtFrame;
	uint32 _sseLastEventFrame;  // Frame of most recent message received during stream
	bool _sseEgoMoved;          // ego moved at any point during this stream
	int _sseTargetObject;       // V0: primary object acted on; 0 if none or non-V0
	Common::Array<uint16> _ssePreInventory;
	Common::Array<ObjStateSnap> _ssePreObjectStates;
	int _ssePreRoom;
	int _ssePrePosX, _ssePrePosY;
	Common::Array<MessageEntry> _sseMessages;
	bool _ssePendingSecondClick;
	int _sseClickMouseX, _sseClickMouseY;
	Common::Array<Common::KeyCode> _ssePendingNotes;
	// Last value seen in the Loom note variable (var 259). Used in pumpStream
	// to detect 0 -> note transitions and emit them as MCP notifications.
	int32 _ssePrevNoteValue;
	// Frame at which the most recent pending-note keypress was fed.
	uint32 _sseLastNoteFedFrame;

	void pushMessage(const char *type, int actorId, const Common::String &text);

	// Tool implementations
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut);
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut);
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut);
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut);
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut);
	bool toolPlayNote(const Common::JSONValue &args, Common::String &errorOut);

	// Debug tools (gated by mcp_debug ini option). Engine-version-agnostic.
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut);
	bool toolKeystroke(const Common::JSONValue &args, Common::String &errorOut);
	bool toolMouseMove(const Common::JSONValue &args, Common::String &errorOut);
	bool toolMouseClick(const Common::JSONValue &args, Common::String &errorOut);

	// Loom segment detection (full Loom or the Loom mini-game in Passport to Adventure)
	bool isInLoomSection() const;

	// Register all tools with the server.
	void registerTools();

	// Streaming helpers
	void snapshotPreAction();
	Common::JSONObject buildStateChanges() const;
	void emitPendingMessages();

	// Game-state helpers
	Actor *getEgoActor() const;
	bool hasPendingQuestion() const;
	bool isActionDone() const;

	void buildEntityMap(Common::Array<NamedEntity> &entities) const;
	bool resolveEntityByName(const Common::String &name, NamedEntity &out) const;
	bool resolveVerb(const Common::String &action, int &verbId) const;

	// Selectability helpers: mirror the engine's findObject() / getActorFromPos() rules
	// so that non-interactive entities are excluded from the MCP entity list.
	bool isObjectSelectable(const ObjectData &od) const;
	bool isActorSelectable(int actorId) const;
};

} // End of namespace Scumm

#endif
