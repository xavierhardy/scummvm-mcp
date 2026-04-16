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

class MonkeyMcpBridge {
public:
	enum EntityKind {
		kEntityInvalid,
		kEntityObject,
		kEntityActor,
		kEntityInventory,
		kEntityPlaceBox,
		kEntityChoiceVerbSlot
	};

	struct ParsedEntityId {
		EntityKind kind;
		int value;
		ParsedEntityId() : kind(kEntityInvalid), value(-1) {}
	};

	explicit MonkeyMcpBridge(ScummEngine *vm);
	~MonkeyMcpBridge();

	void pump();

	void onActorLine(int actorId, const Common::String &text);
	void onSystemLine(const Common::String &text);
	void onDialogPrompt(const Common::String &text);

	// Unit-test friendly helpers
	static bool parseEntityId(const Common::String &id, ParsedEntityId &parsed);
	static Common::String normalizeActionName(const Common::String &action);

public:
	// Helper to access protected getObjOrActorName from a friend
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

	ScummEngine *_vm;
	bool _enabled;
	bool _initialized;
	int _stdinFd;
	int _stdoutFd;
	int _listenFd;
	int _clientFd;
	Common::String _inBuffer;

	Common::Array<MessageEntry> _messages;
	uint64 _nextMessageSeq;
	uint32 _frameCounter;

	bool isMonkey1() const;
	void pushMessage(const char *type, int actorId, const Common::String &text);

	void handleJsonLine(const Common::String &line);
	Common::JSONValue *handleRequest(const Common::JSONValue &req);

	Common::JSONValue *handleInitialize(const Common::JSONValue &req);
	Common::JSONValue *handleToolsList();
	Common::JSONValue *handleToolCall(const Common::JSONValue &req);

	Common::JSONValue *toolListInventory(const Common::JSONValue &args);
	Common::JSONValue *toolListSceneInteractables(const Common::JSONValue &args);
	Common::JSONValue *toolExecuteAction(const Common::JSONValue &args);
	Common::JSONValue *toolRespondToChoice(const Common::JSONValue &args);
	Common::JSONValue *toolMoveTo(const Common::JSONValue &args);
	Common::JSONValue *toolReadLatestMessages(const Common::JSONValue &args);

	void writeJsonRpcResult(const Common::JSONValue *id, Common::JSONValue *result);
	void writeJsonRpcError(const Common::JSONValue *id, int code, const Common::String &msg);
	void writeJson(Common::JSONValue *value);

	int resolveTargetToObjectId(const Common::String &id, bool allowPlace, bool *isPlace = nullptr, int *placeBox = nullptr) const;
	bool resolveVerb(const Common::String &action, int &verbId) const;
	void buildChoices(Common::JSONArray &choices) const;
};

} // End of namespace Scumm

#endif
