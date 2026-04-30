/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/util.h"

#include "scumm/actor.h"
#include "scumm/scumm_v0.h"
#include "scumm/detection.h"
#include "scumm/mcp.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "scumm/boxes.h"

namespace Scumm {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpPropOneOf;
using Networking::mcpObjectSchema;
using Networking::mcpSanitizeString;
using Networking::mcpLowerTrimmed;

namespace {

// V6+ standard action icon verb IDs.
// Sam & Max uses V6 image verbs, but verb IDs differ from other SCUMM variants,
// so keep both common V6 and Sam & Max candidate IDs.
static const int kV6ActionIds[]    = {4, 5, 6, 7, 11, 13, 14, 15};
static const int kNumV6ActionIds   = 8;

static bool isV6ActionVerb(int verbid) {
	for (int i = 0; i < kNumV6ActionIds; ++i)
		if (kV6ActionIds[i] == verbid) return true;
	return false;
}

// Canonical V6 verb label table (verbid → name/label for image-verb games).
struct V6VerbEntry { int id; const char *name; const char *label; };
static const V6VerbEntry kV6CanonicalVerbs[] = {
	// Common V6 mapping used by several games
	{4,  "pick_up", "pick up"},
	{5,  "look_at", "look at"},
	{6,  "talk_to", "talk to"},
	{7,  "use",     "use"},
	{13, "walk_to", "walk to"},
	// Sam & Max mapping (icon verbs)
	{11, "use",     "use"},
	{14, "pick_up", "pick up"},
	{15, "look_at", "look at"},
	{0,  nullptr,   nullptr}
};

static const V6VerbEntry *findV6Verb(int verbid) {
	for (int i = 0; kV6CanonicalVerbs[i].name; ++i)
		if (kV6CanonicalVerbs[i].id == verbid) return &kV6CanonicalVerbs[i];
	return nullptr;
}

// Return the display name of an object/actor. Empty string if unnamed.
Common::String getObjName(const ScummMcpBridge *bridge, int obj) {
	if (!bridge) return "";
	const byte *name = bridge->callGetObjOrActorName(obj);
	if (!name || !*name) return "";
	return Common::String((const char *)name);
}

// Clean up game text: remove control characters, trim whitespace, remove trailing @, remove unicode chars
Common::String cleanGameText(const Common::String &text) {
	Common::String out;
	for (size_t i = 0; i < text.size(); ++i) {
		unsigned char c = (unsigned char)text[i];
		// Replace control characters (except newline) and DEL with space
		if ((c < 0x20 && c != 0x0A) || c == 0x7F) {
			out += ' ';
			continue;
		}
		// Skip UTF-8 sequences for replacement character (� = 0xEF 0xBF 0xBD)
		if (c == 0xEF && i + 2 < text.size()) {
			unsigned char c1 = (unsigned char)text[i+1];
			unsigned char c2 = (unsigned char)text[i+2];
			if (c1 == 0xBF && c2 == 0xBD) {
				// Skip replacement character unless it's part of orichalum beads
				if (i + 4 < text.size()) {
					unsigned char c3 = (unsigned char)text[i+3];
					// Orichalum beads:�� = 0xEF 0xBF 0xBD 0x04 0xEF 0xBF 0xBD
					if (c3 == 0x04) {
						out += text[i];
						out += text[++i];
						out += text[++i];
						out += text[++i];
						continue;
					}
				}
				// Replace with space rather than skipping: V6 games emit non-ASCII
				// charset bytes that mcpSanitizeString converts to U+FFFD; silently
				// dropping them would erase whole dialog lines.
				out += ' ';
				i += 2;
				continue;
			}
		}
		out += text[i];
	}
	// Collapse multiple consecutive spaces
	Common::String collapsed;
	bool prevWasSpace = false;
	for (size_t i = 0; i < out.size(); ++i) {
		char c = out[i];
		if (c == ' ') {
			if (!prevWasSpace) {
				collapsed += c;
				prevWasSpace = true;
			}
		} else {
			collapsed += c;
			prevWasSpace = false;
		}
	}
	out = collapsed;
	// Trim leading/trailing whitespace (including newlines)
	size_t start = 0;
	while (start < out.size() && (unsigned char)out[start] <= 0x20) start++;
	size_t end = out.size();
	while (end > start && (unsigned char)out[end-1] <= 0x20) end--;
	out = out.substr(start, end - start);
	// Remove trailing @
	while (!out.empty() && out[out.size()-1] == '@') {
		out = out.substr(0, out.size()-1);
	}
	return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ScummMcpBridge::ScummMcpBridge(ScummEngine *vm)
	: _vm(vm),
	  _enabled(false),
	  _skipToolEnabled(false),
	  _server(nullptr),
	  _nextMessageSeq(1),
	  _frameCounter(0),
	  _streaming(false),
	  _sseStartFrame(0),
	  _sseDoneAtFrame(0),
	  _sseStuckAtFrame(0),
	  _sseLastEventFrame(0),
	  _sseEgoMoved(false),
	  _sseTargetObject(0),
	  _ssePreRoom(0),
	  _ssePrePosX(0),
	  _ssePrePosY(0) {
	if (!_vm) return;

	_enabled = ConfMan.getBool("mcp");
	if (!_enabled) return;

	_skipToolEnabled = ConfMan.hasKey("mcp_skip_tool") && ConfMan.getBool("mcp_skip_tool");

	int port = ConfMan.hasKey("mcp_port") ? ConfMan.getInt("mcp_port") : 23456;
	Common::String host = ConfMan.hasKey("mcp_host") ? ConfMan.get("mcp_host") : "127.0.0.1";
	_server = new Networking::McpServer(port, "scummvm", "1.0", host);
	if (!_server->isListening()) {
		delete _server;
		_server = nullptr;
		_enabled = false;
		return;
	}
	_server->setToolHandler(this);
	registerTools();
}

ScummMcpBridge::~ScummMcpBridge() {
	delete _server;
}

// ---------------------------------------------------------------------------
// Game-loop hook
// ---------------------------------------------------------------------------

void ScummMcpBridge::pump() {
	if (!_enabled || !_server) return;
	++_frameCounter;
	_server->pump();
}

// ---------------------------------------------------------------------------
// Message capture from engine
// ---------------------------------------------------------------------------

void ScummMcpBridge::pushMessage(const char *type, int actorId, const Common::String &text) {
	if (!_enabled || text.empty()) return;
	MessageEntry m;
	m.seq = _nextMessageSeq++;
	m.frame = _frameCounter;
	m.room = _vm ? _vm->_currentRoom : 0;
	m.actorId = actorId;
	m.type = type;
	m.text = text;
	_messages.push_back(m);
	const uint kMaxMessages = 512;
	if (_messages.size() > kMaxMessages)
		_messages.remove_at(0);
}

void ScummMcpBridge::onActorLine(int actorId, const Common::String &text) {
	pushMessage("actor", actorId, text);
}
void ScummMcpBridge::onSystemLine(const Common::String &text) {
	pushMessage("system", -1, text);
}
void ScummMcpBridge::onDialogPrompt(const Common::String &text) {
	pushMessage("dialog", -1, text);
}

const byte *ScummMcpBridge::callGetObjOrActorName(int obj) const {
	return _vm ? _vm->getObjOrActorName(obj) : nullptr;
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void ScummMcpBridge::registerTools() {
	// --- state ---
	{
		Common::JSONObject inputProps;
		Common::JSONObject outputProps;
		Common::JSONObject roomProps;
		roomProps.setVal("id", mcpProp("integer", "Current room ID"));
		roomProps.setVal("name", mcpProp("string", "Human-readable room name (optional)"));
		Common::JSONObject roomSchema;
		roomSchema.setVal("type", mcpJsonString("object"));
		roomSchema.setVal("properties", new Common::JSONValue(roomProps));
		outputProps.setVal("room", new Common::JSONValue(roomSchema));

		Common::JSONObject positionProps;
		positionProps.setVal("x", mcpProp("integer", "X coordinate"));
		positionProps.setVal("y", mcpProp("integer", "Y coordinate"));
		Common::JSONObject positionSchema;
		positionSchema.setVal("type", mcpJsonString("object"));
		positionSchema.setVal("properties", new Common::JSONValue(positionProps));
		outputProps.setVal("position", new Common::JSONValue(positionSchema));

		auto makeStringArray = []() -> Common::JSONValue * {
			Common::JSONObject arr;
			arr.setVal("type",  mcpJsonString("array"));
			arr.setVal("items", mcpProp("string"));
			return new Common::JSONValue(arr);
		};
		outputProps.setVal("verbs",     makeStringArray());
		outputProps.setVal("inventory", makeStringArray());

		Common::JSONObject objectItemProps;
		objectItemProps.setVal("id",              mcpProp("integer", "Object ID"));
		objectItemProps.setVal("name",            mcpProp("string",  "Object name"));
		objectItemProps.setVal("state",           mcpProp("integer", "Object state"));
		objectItemProps.setVal("x",               mcpProp("integer", "X coordinate"));
		objectItemProps.setVal("y",               mcpProp("integer", "Y coordinate"));
		objectItemProps.setVal("pathway",         mcpProp("boolean", "Is pathway/exit"));
		objectItemProps.setVal("compatible_verbs",mcpProp("array",   "Verbs that have script handlers for this object"));
		Common::JSONObject objectItem;
		objectItem.setVal("type",       mcpJsonString("object"));
		objectItem.setVal("properties", new Common::JSONValue(objectItemProps));
		Common::JSONObject objectsArray;
		objectsArray.setVal("type",  mcpJsonString("array"));
		objectsArray.setVal("items", new Common::JSONValue(objectItem));
		outputProps.setVal("objects", new Common::JSONValue(objectsArray));

		// actors[] removed — NPCs now appear in objects[] with compatible_verbs

		Common::JSONObject msgItemProps;
		msgItemProps.setVal("text",  mcpProp("string", "Message text"));
		msgItemProps.setVal("actor", mcpProp("string", "Actor name (optional)"));
		Common::JSONObject msgItem;
		msgItem.setVal("type",       mcpJsonString("object"));
		msgItem.setVal("properties", new Common::JSONValue(msgItemProps));
		Common::JSONObject messagesArray;
		messagesArray.setVal("type",  mcpJsonString("array"));
		messagesArray.setVal("items", new Common::JSONValue(msgItem));
		outputProps.setVal("messages", new Common::JSONValue(messagesArray));

		Common::JSONObject choiceItemProps;
		choiceItemProps.setVal("id",    mcpProp("integer", "1-based choice index"));
		choiceItemProps.setVal("label", mcpProp("string",  "Choice text"));
		Common::JSONObject choiceItem;
		choiceItem.setVal("type",       mcpJsonString("object"));
		choiceItem.setVal("properties", new Common::JSONValue(choiceItemProps));
		Common::JSONObject choicesArray;
		choicesArray.setVal("type",  mcpJsonString("array"));
		choicesArray.setVal("items", new Common::JSONValue(choiceItem));
		Common::JSONObject questionProps;
		questionProps.setVal("choices", new Common::JSONValue(choicesArray));
		Common::JSONObject questionSchema;
		questionSchema.setVal("type",       mcpJsonString("object"));
		questionSchema.setVal("properties", new Common::JSONValue(questionProps));
		outputProps.setVal("question", new Common::JSONValue(questionSchema));

		Networking::McpServer::ToolSpec spec;
		spec.name = "state";
		spec.description =
		    "Returns the current game state: room, position, inventory, scene objects "
		    "(including NPCs with their compatible_verbs — always includes talk_to), "
		    "active verbs, latest messages (cleared after reading), "
		    "and pending dialog question if any. The player character is never listed. "
		    "Use act(verb='talk_to', target1=<npc_name>) to speak to an NPC.";
		spec.inputSchema  = mcpObjectSchema(inputProps);
		spec.outputSchema = mcpObjectSchema(outputProps);
		spec.streaming    = false;
		_server->registerTool(spec);
	}

	// Shared output schema factory for streaming tools.
	auto makeChangesSchema = []() -> Common::JSONValue * {
		Common::JSONObject props;
		props.setVal("inventory_added", mcpProp("array",   "Names of items added to inventory"));
		props.setVal("room_changed",    mcpProp("integer", "New room number (only present if room changed)"));
		Common::JSONObject posProps;
		posProps.setVal("x", mcpProp("integer", "X coordinate"));
		posProps.setVal("y", mcpProp("integer", "Y coordinate"));
		Common::JSONObject posSchema;
		posSchema.setVal("type",       mcpJsonString("object"));
		posSchema.setVal("properties", new Common::JSONValue(posProps));
		props.setVal("position",        new Common::JSONValue(posSchema));
		props.setVal("objects_changed", mcpProp("array",  "Objects whose state changed: [{name, old_state, new_state}]"));
		props.setVal("messages",        mcpProp("array",  "Dialog/narration lines spoken during the action: [{text, actor?}]"));
		props.setVal("question",        mcpProp("object", "Pending dialog question if action ended with one: {choices:[{id,label}]}"));
		return mcpObjectSchema(props);
	};

	// --- act ---
	{
		Common::JSONObject props;
		props.setVal("verb",    mcpProp("string", "Verb name (e.g. 'open', 'use'). Required."));
		props.setVal("target1", mcpPropOneOf("string", "integer",
		    "Primary target: name or numeric id of an object/inventory item "
		    "currently present in state (objects[] or inventory[]). "
		    "NPCs appear in objects[] and can be targeted by name. "
		    "For 'use X on Y', this is X."));
		props.setVal("target2", mcpPropOneOf("string", "integer",
		    "Secondary target for 'use X on Y' (Y): name or numeric id, "
		    "must currently exist in state."));
		const char *req[] = {"verb"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "act";
		spec.description =
		    "Perform a verb action on one or two named targets. Blocks until the "
		    "action/cutscene sequence completes, streaming dialog and events via SSE, "
		    "then returns state changes. For walking to specific coordinates, use 'walk'. "
		    "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		    "Wait for the previous act/answer/walk call to complete before sending the next one. "
		    "Fails if a question is pending (use 'answer' first) or another action is running.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- answer ---
	{
		Common::JSONObject props;
		props.setVal("id", mcpProp("integer", "1-indexed dialog choice (1 = first option shown in state.question.choices)."));
		const char *req[] = {"id"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "answer";
		spec.description =
		    "Select a dialog choice by 1-based index. Blocks until the conversation "
		    "sequence completes, streaming events via SSE, then returns state changes. "
		    "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		    "Wait for the previous act/answer/walk call to complete before sending the next one. "
		    "Fails if no question is currently pending or another action is running.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- walk ---
	{
		Common::JSONObject props;
		props.setVal("x", mcpProp("integer", "Target X pixel coordinate (auto-clamped to room bounds)"));
		props.setVal("y", mcpProp("integer", "Target Y pixel coordinate (auto-clamped to room bounds)"));
		const char *req[] = {"x", "y"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "walk";
		spec.description =
		    "Walk ego to explicit (x, y) pixel coordinates in the current room. "
		    "Out-of-bounds values are automatically clamped to the room bounds. "
		    "Use 'act' with verb='walk_to' and target1=<name> to walk to a named object. "
		    "Blocks until the walk completes and returns state changes.";
		spec.inputSchema  = mcpObjectSchema(props, req, 2);
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- skip ---
	if (_skipToolEnabled) {
		Networking::McpServer::ToolSpec spec;
		spec.name = "skip";
		spec.description =
		    "Skip/cancel current action or cutscene by simulating an Escape key press. "
		    "Useful for skipping long intros or animations. Returns state changes.";
		spec.inputSchema  = nullptr;  // No input required
		spec.outputSchema = makeChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}
}

// ---------------------------------------------------------------------------
// Tool dispatch
// ---------------------------------------------------------------------------

Common::JSONValue *ScummMcpBridge::callTool(const Common::String &name,
                                             const Common::JSONValue &args,
                                             Common::String &errorOut) {
	if (!_vm) {
		errorOut = "No game loaded";
		return nullptr;
	}
	if (name == "state")
		return toolState(args, errorOut);
	if (name == "act") {
		if (!toolAct(args, errorOut)) return nullptr;
		return nullptr; // streaming started
	}
	if (name == "answer") {
		if (!toolAnswer(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "walk") {
		if (!toolWalk(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "skip") {
		if (!toolSkip(args, errorOut)) return nullptr;
		return nullptr;
	}
	errorOut = "Unknown tool: " + name;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::JSONValue *ScummMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt(_vm->_currentRoom));
	if (_vm->_objs && _vm->_numLocalObjects > 0 && _vm->_objs[0].obj_nr) {
		Common::String rn = getObjName(this, _vm->_objs[0].obj_nr);
		if (!rn.empty()) {
			rn = normalizeActionName(rn);
			bool hasCtrl = false;
			for (uint ci = 0; ci < rn.size(); ++ci)
				if ((unsigned char)rn[ci] < 0x20) { hasCtrl = true; break; }
			if (!hasCtrl)
				roomObj.setVal("name", mcpJsonString(mcpSanitizeString(rn)));
		}
	}
	out.setVal("room", new Common::JSONValue(roomObj));

	Actor *ego = getEgoActor();
	if (ego) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(ego->getRealPos().x));
		pos.setVal("y", mcpJsonInt(ego->getRealPos().y));
		out.setVal("position", new Common::JSONValue(pos));
	}

	// Check for pending dialog question before building the verb bar.
	// When a question is pending, the verb bar is replaced by dialog choices
	// (in V4/MI1, the same verb slots are reused with new text; in V5/Indy4,
	// new slots are added). Either way, we emit an empty verbs list and put
	// the choices into 'question' instead.
	bool questionPending = hasPendingQuestion();

	struct VerbInfo { int verbId; Common::String name; Common::String label; };
	Common::Array<VerbInfo> activeVerbs;
	Common::JSONArray verbsArr;
	if (!questionPending) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
			if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
			if (vs.curmode != 0 && vs.curmode != 1) continue;
			const byte *ptr2 = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr2) continue;
			byte textBuf2[256];
			_vm->convertMessageToString(ptr2, textBuf2, sizeof(textBuf2));
			if (!textBuf2[0]) continue;
			Common::String label = mcpLowerTrimmed((const char *)textBuf2);
			if (label.empty()) continue;
			if (label == "obim") continue;
			bool labelHasCtrl = false;
			for (uint ci = 0; ci < label.size(); ++ci)
				if ((unsigned char)label[ci] < 0x20) { labelHasCtrl = true; break; }
			if (labelHasCtrl) continue;
			Common::String safe2 = mcpSanitizeString(normalizeActionName(label));
			Common::String safeLabel = mcpSanitizeString(label);
			verbsArr.push_back(mcpJsonString(safeLabel));
			VerbInfo vi;
			vi.verbId = vs.verbid;
			vi.name   = safe2;
			vi.label  = safeLabel;
			activeVerbs.push_back(vi);
		}
	}

	// V6+ games use image verbs (kImageVerbType) with no text labels. Build the
	// verb list from the canonical verbid -> name table for any image-type slots
	// not already captured by the text path above.
	if (_vm->_game.version >= 6 && !questionPending) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0) continue;
			if (_vm->_game.version > 0 && vs.verbid == 1) continue;
			if (vs.curmode == 0) continue;
			if (vs.type != kImageVerbType) continue;
			const V6VerbEntry *entry = findV6Verb(vs.verbid);
			if (!entry) continue;
			bool alreadyAdded = false;
			for (uint k = 0; k < activeVerbs.size(); ++k)
				if (activeVerbs[k].verbId == vs.verbid) { alreadyAdded = true; break; }
			if (alreadyAdded) continue;
			verbsArr.push_back(mcpJsonString(entry->label));
			VerbInfo vi2;
			vi2.verbId = vs.verbid;
			vi2.name   = entry->name;
			vi2.label  = entry->label;
			activeVerbs.push_back(vi2);
		}
	}

	// Sam & Max (V6) does not populate the classic text verb slots; expose a
	// stable MCP verb set even when _verbs[] is empty.
	if (_vm->_game.id == GID_SAMNMAX && !questionPending && activeVerbs.empty()) {
		struct FallbackVerb { int id; const char *name; const char *label; };
		static const FallbackVerb kSamnMaxFallback[] = {
			{13, "walk_to", "walk to"},
			{15, "look_at", "look at"},
			{11, "use", "use"},
			{6,  "talk_to", "talk to"},
			{14, "pick_up", "pick up"},
			{0, nullptr, nullptr}
		};
		for (int i = 0; kSamnMaxFallback[i].name; ++i) {
			verbsArr.push_back(mcpJsonString(kSamnMaxFallback[i].label));
			VerbInfo vi;
			vi.verbId = kSamnMaxFallback[i].id;
			vi.name = kSamnMaxFallback[i].name;
			vi.label = kSamnMaxFallback[i].label;
			activeVerbs.push_back(vi);
		}
	}

	out.setVal("verbs", new Common::JSONValue(verbsArr));

	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);

	Common::JSONArray inventory, objects;
	for (uint i = 0; i < entities.size(); ++i) {
		const NamedEntity &ne = entities[i];
		Common::String safe = mcpSanitizeString(ne.displayName);
		switch (ne.kind) {
		case NamedEntity::kInventory: {
			Common::String cleanItem = cleanGameText(safe);
			if (!cleanItem.empty()) {
				inventory.push_back(mcpJsonString(cleanItem));
			}
			break;
		}
		case NamedEntity::kObject: {
			// Skip objects that are out of bounds for the object space
			if (_vm->_numGlobalObjects > 0 && ne.numId >= _vm->_numGlobalObjects) break;

			// Find the actual verb bar labels and check if verbs exist
			Common::String lookAtLabel, walkToLabel;
			bool lookAtExists = false, walkToExists = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (activeVerbs[k].name == "look_at") { lookAtLabel = activeVerbs[k].label; lookAtExists = true; }
				if (activeVerbs[k].name == "walk_to") { walkToLabel = activeVerbs[k].label; walkToExists = true; }
			}

			Common::JSONArray compatVerbs;
			bool hasLookAt = false, hasWalkTo = false;
			int handlerCount = 0;
			bool walkToHasHandler = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (_vm->getVerbEntrypoint(ne.numId, activeVerbs[k].verbId) != 0) {
					compatVerbs.push_back(mcpJsonString(activeVerbs[k].label));
					handlerCount++;
					if (activeVerbs[k].name == "look_at") hasLookAt = true;
					if (activeVerbs[k].name == "walk_to") { hasWalkTo = true; walkToHasHandler = true; }
				}
			}
			if (!hasLookAt && lookAtExists) compatVerbs.push_back(mcpJsonString(lookAtLabel));
			if (!hasWalkTo && walkToExists) compatVerbs.push_back(mcpJsonString(walkToLabel));

			bool isPathway = walkToHasHandler && (handlerCount == 1);

			Common::JSONObject obj;
			obj.setVal("id",               mcpJsonInt(ne.numId));
			obj.setVal("name",             mcpJsonString(safe));
			obj.setVal("state",            mcpJsonInt(_vm->getState(ne.numId)));
			obj.setVal("x",                mcpJsonInt(_vm->getObjX(ne.numId)));
			obj.setVal("y",                mcpJsonInt(_vm->getObjY(ne.numId)));
			obj.setVal("pathway",          mcpJsonBool(isPathway));
			obj.setVal("compatible_verbs", new Common::JSONValue(compatVerbs));
			objects.push_back(new Common::JSONValue(obj));
			break;
		}
		case NamedEntity::kActor: {
			int actorObjId = _vm->actorToObj(ne.numId);
			// Skip actors whose converted object ID is out of bounds
			if (_vm->_numGlobalObjects > 0 && actorObjId >= _vm->_numGlobalObjects) break;

			// Find the actual verb bar label for talk_to and check if it exists
			Common::String talkToLabel;
			bool talkToExists = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (activeVerbs[k].name == "talk_to") { talkToLabel = activeVerbs[k].label; talkToExists = true; }
			}

			Common::JSONArray compatVerbs;
			bool hasTalkTo = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (_vm->getVerbEntrypoint(actorObjId, activeVerbs[k].verbId) != 0) {
					compatVerbs.push_back(mcpJsonString(activeVerbs[k].label));
					if (activeVerbs[k].name == "talk_to") hasTalkTo = true;
				}
			}
			if (!hasTalkTo && talkToExists) compatVerbs.push_back(mcpJsonString(talkToLabel));
			Common::JSONObject actorObj;
			actorObj.setVal("id",               mcpJsonInt(actorObjId));
			actorObj.setVal("name",             mcpJsonString(safe));
			actorObj.setVal("state",            mcpJsonInt(_vm->getState(actorObjId)));
			actorObj.setVal("x",                mcpJsonInt(_vm->getObjX(actorObjId)));
			actorObj.setVal("y",                mcpJsonInt(_vm->getObjY(actorObjId)));
			actorObj.setVal("pathway",          mcpJsonBool(false));
			actorObj.setVal("compatible_verbs", new Common::JSONValue(compatVerbs));
			objects.push_back(new Common::JSONValue(actorObj));
			break;
		}
		}
	}
	out.setVal("inventory", new Common::JSONValue(inventory));
	out.setVal("objects",   new Common::JSONValue(objects));

	Common::JSONArray msgsArr;
	for (uint i = 0; i < _messages.size(); ++i) {
		const MessageEntry &m = _messages[i];
		Common::String cleanText = cleanGameText(mcpSanitizeString(m.text));
		if (cleanText.empty()) continue;
		Common::JSONObject entry;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			// Only include actor name if the object ID is within bounds
			if (_vm->_numGlobalObjects <= 0 || objId < _vm->_numGlobalObjects) {
				Common::String actorName = getObjName(this, objId);
				if (!actorName.empty()) {
					Common::String safe = mcpSanitizeString(mcpLowerTrimmed(actorName));
					entry.setVal("actor", mcpJsonString(safe));
				}
			}
		}
		entry.setVal("text", mcpJsonString(cleanText));
		msgsArr.push_back(new Common::JSONValue(entry));
	}
	out.setVal("messages", new Common::JSONValue(msgsArr));
	_messages.clear();

	if (questionPending) {
		int choiceCount = 0;
		Common::JSONArray choiceList;
		if (_vm->_game.version >= 6) {
			// V6+ dialog choices are icon verbs. Standard action verbs are saved away;
			// the remaining active non-action verb slots are the dialog topic choices.
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0) continue;
				if (_vm->_game.version > 0 && vs.verbid == 1) continue;
				if (vs.curmode == 0) continue;
				if (isV6ActionVerb(vs.verbid)) continue;
				// Try to read associated text first (some V6 dialog slots have it)
				Common::String label;
				const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
				if (ptr) {
					byte textBuf[256] = {};
					_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
					label = cleanGameText(mcpSanitizeString(Common::String((const char *)textBuf)));
				}
				if (label.empty())
					label = Common::String::format("Topic %d", choiceCount + 1);
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(label));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		} else {
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
				if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
				if (vs.curmode != 0 && vs.curmode != 1) continue;
				const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
				if (!ptr) continue;
				byte textBuf[256];
				_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
				if (!textBuf[0]) continue;
				Common::String cleanLabel = cleanGameText(mcpSanitizeString(Common::String((const char *)textBuf)));
				if (cleanLabel.empty()) continue;
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(cleanLabel));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		}
		if (choiceCount > 0) {
			Common::JSONObject question;
			question.setVal("choices", new Common::JSONValue(choiceList));
			out.setVal("question", new Common::JSONValue(question));
		}
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "act: game is not accepting input right now";
		return false;
	}
	if (hasPendingQuestion()) {
		errorOut = "act: a dialog question is pending — use 'answer' first";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "act: arguments must be an object";
		return false;
	}

	const Common::JSONObject &a = args.asObject();
	if (!a.contains("verb") || !a["verb"]->isString()) {
		errorOut = "act: missing 'verb'";
		return false;
	}
	Common::String verbStr = a["verb"]->asString();

	int verbId = -1;
	if (!resolveVerb(verbStr, verbId)) {
		errorOut = "act: unknown verb '" + verbStr + "'";
		return false;
	}

	auto resolveTarget = [&](const char *param, int &out) -> bool {
		if (!a.contains(param)) return true;
		const Common::JSONValue *v = a[param];
		if (v->isIntegerNumber()) {
			out = (int)v->asIntegerNumber();
			if (out < 0) {
				errorOut = Common::String::format("act: %s id %d is negative", param, out);
				return false;
			}
			if (_vm->_numGlobalObjects > 0 && out >= _vm->_numGlobalObjects) {
				errorOut = Common::String::format(
					"act: %s id %d out of bounds (0-%d)",
					param, out, _vm->_numGlobalObjects - 1);
				return false;
			}
			return true;
		}
		if (v->isString()) {
			NamedEntity ent;
			if (!resolveEntityByName(v->asString(), ent)) {
				const char *str = v->asString().c_str();
				char *endptr = nullptr;
				long val = strtol(str, &endptr, 10);
				if (endptr != str && *endptr == '\0') {
					out = (int)val;
					if (out < 0) {
						errorOut = Common::String::format("act: %s id %d is negative", param, out);
						return false;
					}
				} else {
					errorOut = Common::String("act: unknown ") + param + " '" + v->asString() + "'";
					return false;
				}
			} else {
				out = (ent.kind == NamedEntity::kActor) ? _vm->actorToObj(ent.numId) : ent.numId;
			}
			if (_vm->_numGlobalObjects > 0 && out >= _vm->_numGlobalObjects) {
				errorOut = Common::String::format(
					"act: %s id %d out of bounds (0-%d)",
					param, out, _vm->_numGlobalObjects - 1);
				return false;
			}
			return true;
		}
		errorOut = Common::String("act: ") + param + " must be a string name or integer id";
		return false;
	};

	int targetA = 0, targetB = 0;
	if (!resolveTarget("target1", targetA)) return false;
	if (!resolveTarget("target2", targetB)) return false;

	debug(1, "mcp: act verb='%s' verbId=%d targetA=%d targetB=%d",
	      verbStr.c_str(), verbId, targetA, targetB);

	// For Indy4, actors are handled by the sentence script, not verb entrypoints.
	// Skip the entrypoint check for actors and proceed to doSentence.
	bool isIndy4Actor = _vm->_game.id == GID_INDY4 && targetA != 0 && _vm->objIsActor(targetA);

	if (targetA != 0 && !isIndy4Actor) {
		int ep = _vm->getVerbEntrypoint(targetA, verbId);
		debug(1, "mcp: act entrypoint for obj %d verb %d = %d", targetA, verbId, ep);

		// For Indiana Jones: if this object has no handler, search for one that does
		if (ep == 0 && _vm->_game.id == GID_INDY4 && _vm->_objs && verbId > 0) {
			for (int i = 1; i < _vm->_numLocalObjects; ++i) {
				const ObjectData &od = _vm->_objs[i];
				if (!od.obj_nr) continue;
				if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
				int tryEp = _vm->getVerbEntrypoint(od.obj_nr, verbId);
				if (tryEp != 0) {
					debug(1, "mcp: no handler on %d, trying object %d instead", targetA, od.obj_nr);
					targetA = od.obj_nr;
					ep = tryEp;
					break;
				}
			}
		}
	}

	// In Maniac Mansion, executing verbs without entrypoints can cause out-of-bounds errors
	// when the default sentence handling code accesses object 386. Skip execution if no handler.
	if (_vm->_game.id == GID_MANIAC && targetA != 0) {
		int ep = _vm->getVerbEntrypoint(targetA, verbId);
		if (ep == 0) {
			debug(1, "mcp: skipping verb with no entrypoint on object %d", targetA);
			errorOut = "verb has no handler for this object";
			return false;
		}
		// In V0, certain transitive verbs require a second object (direct object).
		// Executing them without one causes a crash in the sentence handler.
		if (_vm->_game.version == 0 && targetB == 0 &&
		    (verbId == kVerbUse || verbId == kVerbGive || verbId == kVerbUnlock || verbId == kVerbFix)) {
			debug(1, "mcp: skipping verb %d on object %d (requires second object)", verbId, targetA);
			errorOut = "transitive verb requires second object";
			return false;
		}
	}

	// For V0: ScummEngine_v0 asserts that the primary object (st.objectA) exists.
	// We must check if targetA is valid and present before starting the action.
	if (_vm->_game.version == 0) {
		if (targetA == 0 || _vm->whereIsObject(targetA) == WIO_NOT_FOUND) {
			errorOut = "target object does not exist or is not available";
			return false;
		}
	}

	snapshotPreAction();
	_streaming = true;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	// For V0: track the primary target so isActionDone() can wait for its script to finish.
	// V0 scripts do not lock _userPut during execution, unlike V5, so script-slot polling
	// is the only reliable signal that the verb script has completed.
	_sseTargetObject = (_vm->_game.version == 0) ? targetA : 0;
	_vm->doSentence(verbId, targetA, targetB);
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "answer: another action is already in progress";
		return false;
	}
	if (!hasPendingQuestion()) {
		errorOut = "answer: no dialog question is currently pending";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "answer: arguments must be an object";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("id") || !a["id"]->isIntegerNumber()) {
		errorOut = "answer: missing 'id'";
		return false;
	}
	int choiceIdx = (int)a["id"]->asIntegerNumber();
	if (choiceIdx < 1) {
		errorOut = "answer: id must be >= 1";
		return false;
	}

	int current = 0;
	int chosenSlot = -1;
	if (_vm->_game.version >= 6) {
		// V6+ dialog choices: enumerate active non-action verb slots (image-based, no text).
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0) continue;
			if (_vm->_game.version > 0 && vs.verbid == 1) continue;
			if (vs.curmode == 0) continue;
			if (isV6ActionVerb(vs.verbid)) continue;
			++current;
			if (current == choiceIdx) { chosenSlot = slot; break; }
		}
	} else {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
			// Only accept curmode=0 if slot has numeric key (dialog); otherwise require curmode=1
			if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
			if (vs.curmode != 0 && vs.curmode != 1) continue;
			const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr) continue;
			byte textBuf[256];
			_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
			if (!textBuf[0]) continue;
			++current;
			if (current == choiceIdx) { chosenSlot = slot; break; }
		}
	}
	if (chosenSlot < 0) {
		errorOut = Common::String::format("answer: choice %d not found (only %d available)", choiceIdx, current);
		return false;
	}

	const VerbSlot &vs = _vm->_verbs[chosenSlot];
	snapshotPreAction();
	_streaming = true;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_sseTargetObject = 0;  // dialog answer has no target object
	_vm->runInputScript(kVerbClickArea, vs.verbid, 1);
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (_vm->_userPut <= 0) {
		errorOut = "walk: game is not accepting input right now";
		return false;
	}
	if (hasPendingQuestion()) {
		errorOut = "walk: a dialog question is pending — use 'answer' first";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "walk: arguments must be an object";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "walk: 'x' and 'y' integer coordinates are required";
		return false;
	}

	Actor *ego = getEgoActor();
	if (!ego) {
		errorOut = "walk: no ego actor available";
		return false;
	}

	int wx = (int)a["x"]->asIntegerNumber();
	int wy = (int)a["y"]->asIntegerNumber();
	int maxX = (_vm->_roomWidth  > 0 ? _vm->_roomWidth  : _vm->_screenWidth)  - 1;
	int maxY = (_vm->_roomHeight > 0 ? _vm->_roomHeight : _vm->_screenHeight) - 1;
	if (maxX < 0) maxX = 0;
	if (maxY < 0) maxY = 0;
	wx = CLIP<int>(wx, 0, maxX);
	wy = CLIP<int>(wy, 0, maxY);

	snapshotPreAction();
	_streaming = true;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_sseTargetObject = 0;  // walk has no target object
	ego->startWalkActor(wx, wy, -1);
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool ScummMcpBridge::toolSkip(const Common::JSONValue &args, Common::String &errorOut) {
	// Allow skip even if streaming to interrupt current action
	if (!_streaming) {
		snapshotPreAction();
		_streaming = true;
		_sseStartFrame = _frameCounter;
		_sseDoneAtFrame = 0;
		_sseStuckAtFrame = 0;
		_sseLastEventFrame = 0;
		_sseEgoMoved = false;
		_sseMessages.clear();
		_sseTargetObject = 0;
		_server->startStreaming();
	}

	// Simulate Escape key press to skip/cancel
	_vm->_keyPressed = Common::KeyCode(27); // ESC key
	return true;
}

// ---------------------------------------------------------------------------
// Streaming pump
// ---------------------------------------------------------------------------

void ScummMcpBridge::emitPendingMessages() {
	while (!_messages.empty()) {
		const MessageEntry &m = _messages[0];
		_sseMessages.push_back(m);
		_sseLastEventFrame = _frameCounter;
		Common::JSONObject params;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			// Only include actor name if the object ID is within bounds
			if (_vm->_numGlobalObjects <= 0 || objId < _vm->_numGlobalObjects) {
				Common::String actorName = getObjName(this, objId);
				if (!actorName.empty()) {
					Common::String safe = mcpSanitizeString(mcpLowerTrimmed(actorName));
					params.setVal("actor", mcpJsonString(safe));
				}
			}
		}
		Common::String cleanText = cleanGameText(mcpSanitizeString(m.text));
		if (!cleanText.empty()) {
			params.setVal("text", mcpJsonString(cleanText));
			params.setVal("type", mcpJsonString(mcpSanitizeString(m.type)));
			_server->emitNotification(params);
		}
		_messages.remove_at(0);
	}
}

void ScummMcpBridge::pumpStream() {
	if (!_streaming) return;

	emitPendingMessages();

	// Track whether ego moved at any point during this stream.
	{
		Actor *ego = getEgoActor();
		if (ego && ego->_moving)
			_sseEgoMoved = true;
	}

	// Early-close: if the room has already changed, there is nothing left to settle —
	// no dialogue will appear in the old room and accessing old-room state is unsafe.
	if ((int)_vm->_currentRoom != _ssePreRoom) {
		debug(1, "mcp: room changed to %d during stream, closing immediately", _vm->_currentRoom);
		Common::JSONObject changes = buildStateChanges();
		_streaming = false;
		_server->endStream(new Common::JSONValue(changes), true);
		return;
	}

	// Early-exit: stuck (no speech, user-put locked).
	// This includes both idle and animated states (e.g., cutscenes with ego moving).
	// Use a short timeout when no events have occurred yet (action had no visible
	// effect and completed quickly), and a longer one when we've seen activity.
	{
		bool stuck = _vm->_talkDelay == 0 && _vm->_userPut <= 0;
		if (stuck) {
			if (_sseStuckAtFrame == 0) _sseStuckAtFrame = _frameCounter;
			bool hadActivity = _sseLastEventFrame > 0 || _sseEgoMoved;
			uint32 stuckLimit = hadActivity ? 90 : 15;
			if (_frameCounter - _sseStuckAtFrame > stuckLimit) {
				debug(1, "mcp: action stuck for %d frames — closing stream", stuckLimit);
				Common::JSONObject changes = buildStateChanges();
				_streaming = false;
				_server->endStream(new Common::JSONValue(changes), true);
				return;
			}
		} else {
			_sseStuckAtFrame = 0;
		}
	}

	// Hard timeout: 600 frames (~20 s).
	if (_frameCounter - _sseStartFrame > 600) {
		debug(1, "mcp: stream timeout after 600 frames");
		_streaming = false;
		_server->endStream(nullptr, false, "action timed out");
		return;
	}

	bool done = isActionDone();
	if (done) {
		if (_sseDoneAtFrame == 0) {
			_sseDoneAtFrame = _frameCounter;
			debug(1, "mcp: action looks done at frame %d, settling (egoMoved=%d, lastEvent=%d)",
			      _frameCounter, _sseEgoMoved, _sseLastEventFrame);
		}

		// If a new message arrived after we first thought we were done, the action
		// script was still running — reset the window to wait for it to finish.
		if (_sseLastEventFrame > _sseDoneAtFrame) {
			debug(1, "mcp: new event at frame %d after done at %d, extending window",
			      _sseLastEventFrame, _sseDoneAtFrame);
			_sseDoneAtFrame = _sseLastEventFrame;
		}

		bool questionReady = hasPendingQuestion();
		uint32 settleFrames = (_vm->_game.version == 0) ? 3 : 10;
		if (_vm->_game.version != 0 && _sseEgoMoved && !questionReady)
			settleFrames = 20;

		// For V0 (Maniac Mansion): after ego reaches the target object, runObjectScript
		// is called for the verb. Wait until that object script finishes (isScriptInUse
		// with the target object's ID). Cap at 30 frames after sseDoneAtFrame.
		bool v0ScriptRunning = (_vm->_game.version == 0) &&
		                       (_sseTargetObject != 0) &&
		                       _vm->isScriptInUse(_sseTargetObject) &&
		                       (_frameCounter - _sseDoneAtFrame < 30);

		bool settled = !v0ScriptRunning && (_frameCounter - _sseDoneAtFrame >= settleFrames);
		if (questionReady || settled) {
			debug(1, "mcp: closing stream at frame %d (question=%d, settled=%d, settleFrames=%d)",
				_frameCounter, questionReady, settled, settleFrames);
			Common::JSONObject changes = buildStateChanges();
			_streaming = false;
			_server->endStream(new Common::JSONValue(changes), true);
		}
	} else {
		_sseDoneAtFrame = 0;
	}
}

// ---------------------------------------------------------------------------
// Pre-action snapshot + state-change diff
// ---------------------------------------------------------------------------

void ScummMcpBridge::snapshotPreAction() {
	_ssePreRoom = _vm->_currentRoom;
	_ssePreInventory.clear();
	{
		int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
		for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i) {
			uint16 obj = _vm->_inventory[i];
			if (obj && _vm->getOwner(obj) == ego)
				_ssePreInventory.push_back(obj);
		}
	}
	Actor *ego = getEgoActor();
	if (ego) {
		_ssePrePosX = ego->getRealPos().x;
		_ssePrePosY = ego->getRealPos().y;
	} else {
		_ssePrePosX = _ssePrePosY = 0;
	}
	_ssePreObjectStates.clear();
	if (!_vm->_objs) return;
	for (int i = 1; i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		// Skip objects that are out of bounds for the object space
		if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
		ObjStateSnap snap;
		snap.objNr = od.obj_nr;
		snap.state = _vm->getState(od.obj_nr);
		debug(1, "mcp: preSnapshot obj=%d state=%d", snap.objNr, snap.state);
		_ssePreObjectStates.push_back(snap);
	}
}

Common::JSONObject ScummMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	Common::JSONArray added;
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i) {
		uint16 obj = _vm->_inventory[i];
		if (!obj) continue;
		// Skip inventory items that are out of bounds for the object space
		if (_vm->_numGlobalObjects > 0 && obj >= _vm->_numGlobalObjects) continue;
		if (_vm->getOwner(obj) != ego) continue;
		bool wasPresent = false;
		for (uint j = 0; j < _ssePreInventory.size(); ++j)
			if (_ssePreInventory[j] == obj) { wasPresent = true; break; }
		if (!wasPresent) {
			Common::String name = getObjName(this, obj);
			if (name.empty()) name = Common::String::format("obj-%d", obj);
			Common::String cleanName = cleanGameText(mcpSanitizeString(mcpLowerTrimmed(name)));
			if (!cleanName.empty()) {
				added.push_back(mcpJsonString(cleanName));
			}
		}
	}
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	Common::JSONArray removed;
	for (uint j = 0; j < _ssePreInventory.size(); ++j) {
		uint16 obj = _ssePreInventory[j];
		if (!obj) continue;
		if (_vm->_numGlobalObjects > 0 && obj >= _vm->_numGlobalObjects) continue;
		if (_vm->getOwner(obj) == ego) continue;
		Common::String name = getObjName(this, obj);
		if (name.empty()) name = Common::String::format("obj-%d", obj);
		Common::String cleanName = cleanGameText(mcpSanitizeString(mcpLowerTrimmed(name)));
		if (!cleanName.empty()) {
			removed.push_back(mcpJsonString(cleanName));
		}
	}
	if (!removed.empty())
		changes.setVal("inventory_removed", new Common::JSONValue(removed));

	if ((int)_vm->_currentRoom != _ssePreRoom)
		changes.setVal("room_changed", mcpJsonInt(_vm->_currentRoom));

	Actor *ego2 = getEgoActor();
	if (ego2) {
		int cx = ego2->getRealPos().x;
		int cy = ego2->getRealPos().y;
		if (cx != _ssePrePosX || cy != _ssePrePosY) {
			Common::JSONObject pos;
			pos.setVal("x", mcpJsonInt(cx));
			pos.setVal("y", mcpJsonInt(cy));
			changes.setVal("position", new Common::JSONValue(pos));
		}
	}

	Common::JSONArray objChanges;
	for (int i = 1; _vm->_objs && i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		// Skip objects that are out of bounds for the object space
		if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
		int newState = _vm->getState(od.obj_nr);
		debug(1, "mcp: buildStateChanges obj=%d newState=%d", od.obj_nr, newState);
		int preState = newState;
		for (uint j = 0; j < _ssePreObjectStates.size(); ++j) {
			if (_ssePreObjectStates[j].objNr == od.obj_nr) {
				preState = _ssePreObjectStates[j].state;
				break;
			}
		}
		if (newState == preState) continue;
		Common::String name = getObjName(this, od.obj_nr);
		if (name.empty()) name = Common::String::format("obj-%d", od.obj_nr);
		Common::JSONObject entry;
		entry.setVal("name",      mcpJsonString(mcpSanitizeString(mcpLowerTrimmed(name))));
		entry.setVal("old_state", mcpJsonInt(preState));
		entry.setVal("new_state", mcpJsonInt(newState));
		objChanges.push_back(new Common::JSONValue(entry));
	}
	if (!objChanges.empty())
		changes.setVal("objects_changed", new Common::JSONValue(objChanges));

	{
		Common::JSONArray msgs;
		for (uint i = 0; i < _sseMessages.size(); ++i) {
			const MessageEntry &me = _sseMessages[i];
			Common::String cleanText = cleanGameText(mcpSanitizeString(me.text));
			if (cleanText.empty()) continue;
			Common::JSONObject m;
			m.setVal("text", mcpJsonString(cleanText));
			if (me.actorId > 0) {
				const byte *actorNamePtr = callGetObjOrActorName(me.actorId);
				if (actorNamePtr) {
					Common::String actorName = mcpSanitizeString(mcpLowerTrimmed(Common::String((const char *)actorNamePtr)));
					if (!actorName.empty())
						m.setVal("actor", mcpJsonString(actorName));
				}
			}
			msgs.push_back(new Common::JSONValue(m));
		}
		if (!msgs.empty())
			changes.setVal("messages", new Common::JSONValue(msgs));
	}

	if (hasPendingQuestion()) {
		int choiceCount = 0;
		Common::JSONArray choiceList;
		if (_vm->_game.version >= 6) {
			// V6+ dialog choices are icon verbs without text. Enumerate active non-action slots.
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0) continue;
				if (_vm->_game.version > 0 && vs.verbid == 1) continue;
				if (vs.curmode == 0) continue;
				if (isV6ActionVerb(vs.verbid)) continue;
				Common::String label;
				const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
				if (ptr) {
					byte textBuf[256] = {};
					_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
					label = cleanGameText(mcpSanitizeString(Common::String((const char *)textBuf)));
				}
				if (label.empty())
					label = Common::String::format("Topic %d", choiceCount + 1);
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(label));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		} else {
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (!vs.verbid || vs.saveid != 0 || (_vm->_game.version > 0 && vs.verbid == 1)) continue;
				// Only accept curmode=0 if slot has numeric key (dialog); otherwise require curmode=1
				if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
				if (vs.curmode != 0 && vs.curmode != 1) continue;
				const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
				if (!ptr) continue;
				byte textBuf[256];
				_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
				if (!textBuf[0]) continue;
				Common::String cleanLabel = cleanGameText(mcpSanitizeString(Common::String((const char *)textBuf)));
				if (cleanLabel.empty()) continue;
				Common::JSONObject choice;
				choice.setVal("id",    mcpJsonInt(++choiceCount));
				choice.setVal("label", mcpJsonString(cleanLabel));
				choiceList.push_back(new Common::JSONValue(choice));
			}
		}
		if (choiceCount > 0) {
			Common::JSONObject question;
			question.setVal("choices", new Common::JSONValue(choiceList));
			changes.setVal("question", new Common::JSONValue(question));
		}
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Game-state helpers
// ---------------------------------------------------------------------------

Actor *ScummMcpBridge::getEgoActor() const {
	if (!_vm || _vm->VAR_EGO == 0xFF) return nullptr;
	int egoNum = _vm->VAR(_vm->VAR_EGO);
	if (!_vm->isValidActor(egoNum)) return nullptr;
	return _vm->derefActor(egoNum, "getEgoActor");
}

bool ScummMcpBridge::isActionDone() const {
	if (_frameCounter - _sseStartFrame < 3) return false;
	Actor *ego = getEgoActor();
	// Ego movement check with timeout only for V0 (Maniac Mansion):
	// V0 doesn't lock _userPut, so we need a timeout to prevent indefinite waits.
	// V5+ games handle movement more predictably and don't need this timeout.
	if (_vm->_talkDelay > 0) return false;
	if (_vm->_userPut <= 0) return false;
	if (_vm->_game.version == 0) {
		// V0 (Maniac Mansion): actors use _moving==2 for "arrived", not 0. The
		// reliable completion signal is: sentence dispatched (_sentenceNum==0)
		// AND walk-then-turn-then-act cycle finished (isWalkToObjectDone()).
		// By the time isActionDone() first runs (>= 3 frames in), checkAndRunSentenceScript
		// has already set _walkToObjectState to non-zero, so the initial state
		// (_sentenceNum==0, _walkToObjectState==kWalkToObjectStateDone) is safe.
		ScummEngine_v0 *v0 = static_cast<ScummEngine_v0 *>(_vm);
		if (_vm->_sentenceNum > 0 || !v0->isWalkToObjectDone())
			return false;
	} else {
		if (ego && ego->_moving) return false;
	}
	return true;
}

bool ScummMcpBridge::hasPendingQuestion() const {
	if (!_vm || _vm->_userPut <= 0) return false;

	// V6+ (Sam & Max and later): dialog uses icon verb slots. The game saves the
	// five standard action icon verbs (saveid != 0) and inserts new topic icon slots.
	// Dialog is pending when at least one standard action verb is saved AND at least
	// one non-standard active verb exists (the topic choices).
	if (_vm->_game.version >= 6) {
		bool hasActiveSavedAction = false;
		bool hasActiveDialog     = false;
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid) continue;
			if (_vm->_game.version > 0 && vs.verbid == 1) continue;
			if (isV6ActionVerb(vs.verbid)) {
				if (vs.saveid != 0) hasActiveSavedAction = true;
			} else if (vs.saveid == 0 && vs.curmode != 0) {
				hasActiveDialog = true;
			}
		}
		return hasActiveSavedAction && hasActiveDialog;
	}

	bool hasKeyed = false, hasUnkeyed = false, hasNumericKeyed = false;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0) continue;
		// Skip OBIM verb slots (verbid=1) — they are graphical UI elements, not text
		// choices, but their key=0 would incorrectly set hasUnkeyed=true and prevent
		// Indy4-style numeric-key dialog detection. In V0, verbid=1 is Open, not OBIM.
		if (_vm->_game.version > 0 && vs.verbid == 1) continue;
		// Only accept curmode=0 if slot has numeric key (dialog); otherwise require curmode=1
		if (vs.curmode == 0 && (vs.key < '1' || vs.key > '9')) continue;
		if (vs.curmode != 0 && vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		if (vs.key) {
			hasKeyed = true;
			// Indy4/FOA assigns numeric keys '1'-'9' to dialog choices
			if (vs.key >= '1' && vs.key <= '9') hasNumericKeyed = true;
		} else {
			hasUnkeyed = true;
		}
	}
	// MI1-style: dialog choices are unkeyed, normal verb bar is keyed (or absent).
	// Skip this for Maniac Mansion, which has unkeyed verbs in normal gameplay.
	if (hasUnkeyed && !hasKeyed && _vm->_game.id != GID_MANIAC) return true;
	// Indy4-style: dialog choices have numeric keys; normal verb bar is saved (saveid!=0)
	if (hasNumericKeyed && !hasUnkeyed) return true;
	return false;
}

// ---------------------------------------------------------------------------
// Name resolution
// ---------------------------------------------------------------------------

Common::String ScummMcpBridge::normalizeActionName(const Common::String &action) {
	Common::String s(action);
	s.trim();
	s.toLowercase();
	s.replace('-', '_');
	s.replace(' ', '_');
	if (s == "walk")    return "walk_to";
	if (s == "goto")    return "walk_to";
	if (s == "look")    return "look_at";
	if (s == "what_is") return "look_at";
	if (s == "examine") return "look_at";
	if (s == "pick")    return "pick_up";
	if (s == "pickup")  return "pick_up";
	if (s == "take")    return "pick_up";
	if (s == "get")     return "pick_up";
	if (s == "talk")    return "talk_to";
	return s;
}

// Map of verb canonical names for looking up by label
static const struct {
	const char *canonical;
	const char *label;
} kVerbMap[] = {
	{"talk_to", "Talk to"},
	{"talk_to", "talk_to"},
	{"look_at", "Look at"},
	{"look_at", "look_at"},
	{"look_at", "What is"},		// Maniac Mansion C64
	{"pick_up", "Pick up"},
	{"pick_up", "pick_up"},
	{"walk_to", "Walk to"},
	{"walk_to", "walk_to"},
	{"open", "Open"},
	{"open", "open"},
	{"close", "Close"},
	{"close", "close"},
	{"use", "Use"},
	{"use", "use"},
	{"unlock", "Unlock"},
	{"unlock", "unlock"},
	{"give", "Give"},
	{"give", "give"},
	{"push", "Push"},
	{"push", "push"},
	{"pull", "Pull"},
	{"pull", "pull"},
	{nullptr, nullptr}
};

// Check if a verb bar label matches the canonical action
static bool verbLabelMatches(const Common::String &rawLabel, const Common::String &canonicalAction) {
	for (int i = 0; kVerbMap[i].canonical; ++i) {
		if (rawLabel == kVerbMap[i].label && canonicalAction == kVerbMap[i].canonical)
			return true;
	}
	// Fallback: normalize the raw label and compare directly
	Common::String normLabel = ScummMcpBridge::normalizeActionName(mcpLowerTrimmed(rawLabel));
	return normLabel == canonicalAction;
}

// ---------------------------------------------------------------------------
// Selectability helpers
// ---------------------------------------------------------------------------

// Mirrors findObject(): returns false for objects the player cannot click on.
bool ScummMcpBridge::isObjectSelectable(const ObjectData &od) const {
	// The untouchable class flag is the primary gate — it applies to all games.
	if (_vm->getClass(od.obj_nr, kObjectClassUntouchable))
		return false;

	// Per-game additional checks that mirror the version-specific branches in findObject().
	switch (_vm->_game.id) {
	case GID_MANIAC:
		// V0: only foreground objects carry the untouchable state flag.
		// Background (BG) and actor-type objects use only the class check above.
		if (OBJECT_V0_TYPE(od.obj_nr) == kObjectV0TypeFG && (od.state & kObjectStateUntouchable))
			return false;
		break;
	case GID_MONKEY_EGA:
	case GID_INDY4:
		// V5: the class-level check above is sufficient; no state-flag veto.
		break;
	default:
		break;
	}
	return true;
}

// Mirrors getActorFromPos(): returns false for actors the player cannot target.
bool ScummMcpBridge::isActorSelectable(int actorId) const {
	// The untouchable class flag is the only selectability gate for actors across
	// all three supported games; getActorFromPos() applies the same test.
	switch (_vm->_game.id) {
	case GID_MANIAC:
	case GID_MONKEY_EGA:
	case GID_INDY4:
		return !_vm->getClass(actorId, kObjectClassUntouchable);
	default:
		return !_vm->getClass(actorId, kObjectClassUntouchable);
	}
}

void ScummMcpBridge::buildEntityMap(Common::Array<NamedEntity> &entities) const {
	entities.clear();

	struct RawEntry {
		NamedEntity::Kind kind;
		int numId;
		Common::String baseName;
		bool visible = false;
		bool isPathway = false;
	};
	Common::Array<RawEntry> raw;

	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i) {
		int obj = _vm->_inventory[i];
		if (!obj || _vm->getOwner(obj) != ego) continue;
		Common::String name = getObjName(this, obj);
		if (name.empty()) continue;
		RawEntry e;
		e.kind = NamedEntity::kInventory;
		e.numId = obj;
		e.baseName = normalizeActionName(name);
		raw.push_back(e);
	}

	for (int i = 1; _vm->_objs && i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		// Exclude objects the player cannot interact with (mirrors findObject()).
		if (!isObjectSelectable(od)) continue;
		Common::String name = getObjName(this, od.obj_nr);
		RawEntry e;
		e.kind = NamedEntity::kObject;
		e.numId = od.obj_nr;
		e.baseName = name.empty() ? Common::String::format("obj-%d", od.obj_nr)
		                          : normalizeActionName(name);
		// Visibility mask: v0-v2 use only the intrinsic (on/off) bit; v3+ use the full
		// lower nibble which encodes pickupable, untouchable, locked, and intrinsic.
		const int mask = (_vm->_game.version <= 2)
		    ? kObjectStateIntrinsic
		    : (kObjectStatePickupable | kObjectStateUntouchable | kObjectStateLocked | kObjectStateIntrinsic);
		e.visible = (od.state & mask) != 0;
		if (e.visible && od.parent != 0 && od.parent < _vm->_numLocalObjects)
			e.visible = ((_vm->_objs[od.parent].state & mask) == od.parentstate);
		// Pathway objects are invisible exits with only a walk_to handler.
		// They are kept in the list so the agent can navigate, but flagged separately.
		if (!e.visible) {
			bool hasWalkTo = false, hasOther = false;
			if (_vm->_game.version >= 6) {
				// V6+ verbs are image-based: identify walk_to by verbid (13) directly.
				for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
					const VerbSlot &vs = _vm->_verbs[slot];
					if (!vs.verbid || vs.saveid != 0) continue;
					if (_vm->getVerbEntrypoint(e.numId, vs.verbid) == 0) continue;
					if (vs.verbid == 13) hasWalkTo = true;
					else hasOther = true;
				}
			} else {
				for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
					const VerbSlot &vs = _vm->_verbs[slot];
					if (!vs.verbid || vs.saveid != 0) continue;
					if (_vm->getVerbEntrypoint(e.numId, vs.verbid) == 0) continue;
					const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
					if (!ptr) continue;
					byte textBuf[256];
					_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
					Common::String label = normalizeActionName((const char *)textBuf);
					if (label == "walk_to") hasWalkTo = true;
					else hasOther = true;
				}
			}
			if (hasWalkTo && !hasOther) e.isPathway = true;
		}
		if (!name.empty()) {
			bool hasCtrl = false;
			for (uint ci = 0; ci < e.baseName.size(); ++ci)
				if ((unsigned char)e.baseName[ci] < 0x20) { hasCtrl = true; break; }
			if (hasCtrl) continue;
		}
		raw.push_back(e);
	}

	int egoNum = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : -1;
	for (int i = 1; _vm->_actors && i < _vm->_numActors; ++i) {
		Actor *a = _vm->_actors[i];
		if (!a) continue;
		// Only include actors that are properly placed in the current room.
		// In V4+ (Monkey Island, Atlantis), actors are visual representations only;
		// room objects carry the verb scripts. Requiring _room == currentRoom prevents
		// including actors placed by scripts at off-screen positions (e.g. (0,0)).
		if (!a->isInCurrentRoom()) continue;
		// Ego is the player character; it is never presented as an interaction target.
		if (a->_number == egoNum) continue;
		// Exclude actors the player cannot click on (mirrors getActorFromPos()).
		if (!isActorSelectable(a->_number)) continue;
		int objId = _vm->actorToObj(a->_number);
		// Even if objId is out of bounds, include the actor so it can be targeted
		// (the verb handler will handle whether the verb is available)
		Common::String name;
		if (_vm->_numGlobalObjects <= 0 || objId < _vm->_numGlobalObjects) {
			name = getObjName(this, objId);
		}
		RawEntry e;
		e.kind = NamedEntity::kActor;
		e.numId = a->_number;
		e.visible = a->_visible;
		e.baseName = name.empty() ? Common::String::format("actor-%d", a->_number)
		                          : normalizeActionName(name);
		if (!name.empty()) {
			bool hasCtrl = false;
			for (uint ci = 0; ci < e.baseName.size(); ++ci)
				if ((unsigned char)e.baseName[ci] < 0x20) { hasCtrl = true; break; }
			if (hasCtrl) continue;
		}
		raw.push_back(e);
	}

	for (uint i = 0; i < raw.size(); ++i) {
		NamedEntity ne;
		ne.kind        = raw[i].kind;
		ne.numId       = raw[i].numId;
		ne.displayName = raw[i].baseName;
		ne.visible     = raw[i].visible;
		ne.isPathway   = raw[i].isPathway;
		entities.push_back(ne);
	}
}

bool ScummMcpBridge::resolveEntityByName(const Common::String &name, NamedEntity &out) const {
	Common::String normalized = normalizeActionName(name);
	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);
	debug(1, "mcp: resolveEntityByName '%s' (normalized='%s'), %u entities in map",
	      name.c_str(), normalized.c_str(), (uint)entities.size());
	for (uint i = 0; i < entities.size(); ++i) {
		debug(1, "mcp:   entity[%u] kind=%d id=%d name='%s' visible=%d",
		      i, (int)entities[i].kind, entities[i].numId,
		      entities[i].displayName.c_str(), entities[i].visible);
	}
	// When an actor and a room object share a name:
	// V0 (Maniac Mansion): actors carry the verb scripts → prefer actor over object.
	// V4+ (Monkey Island, Atlantis): room objects carry verb scripts; actors are
	//   visual only → prefer room object over actor so verbs execute correctly.
	int actorMatch = -1;
	int objectMatch = -1;
	int firstMatch = -1;
	for (uint i = 0; i < entities.size(); ++i) {
		if (entities[i].displayName != normalized) continue;
		if (firstMatch < 0) firstMatch = (int)i;
		if (entities[i].kind == NamedEntity::kActor) {
			if (actorMatch < 0) actorMatch = (int)i;
		} else {
			if (objectMatch < 0) objectMatch = (int)i;
		}
	}
	bool v0Game = (_vm->_game.version == 0);
	int best = v0Game
	    ? ((actorMatch  >= 0) ? actorMatch  : (objectMatch >= 0) ? objectMatch : firstMatch)
	    : ((objectMatch >= 0) ? objectMatch : (actorMatch  >= 0) ? actorMatch  : firstMatch);
	if (best >= 0) {
		// For V4+: if the best match is an actor and there is an untouchable room object
		// with the same name, prefer the room object — it carries the verb entrypoints
		// while the actor is a visual-only representation.
		if (!v0Game && entities[best].kind == NamedEntity::kActor && _vm->_objs) {
			for (int i = 1; i < _vm->_numLocalObjects; ++i) {
				const ObjectData &od = _vm->_objs[i];
				if (!od.obj_nr) continue;
				if (_vm->_numGlobalObjects > 0 && od.obj_nr >= _vm->_numGlobalObjects) continue;
				Common::String objName = getObjName(this, od.obj_nr);
				if (objName.empty()) continue;
				if (normalizeActionName(objName) == normalized) {
					out.kind        = NamedEntity::kObject;
					out.numId       = od.obj_nr;
					out.displayName = normalized;
					out.visible     = false;
					debug(1, "mcp: resolveEntityByName '%s' redirected from actor to room obj %d",
					      normalized.c_str(), od.obj_nr);
					return true;
				}
			}
		}
		out = entities[best]; return true;
	}
	debug(1, "mcp: resolveEntityByName '%s' not found", name.c_str());
	return false;
}

bool ScummMcpBridge::resolveVerb(const Common::String &action, int &verbId) const {
	Common::String normalized = normalizeActionName(action);
	debug(1, "mcp: resolveVerb '%s' (normalized='%s')", action.c_str(), normalized.c_str());
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		byte textBuf[256] = {};
		if (ptr) _vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String rawLabel((const char *)textBuf);
		Common::String normLabel = normalizeActionName(rawLabel);
		debug(1, "mcp:   slot=%d verbid=%d saveid=%d curmode=%d key=%d label='%s'",
		      slot, vs.verbid, vs.saveid, vs.curmode, vs.key, rawLabel.c_str());
		if (vs.saveid != 0) continue;
		if (!ptr) continue;
		if (rawLabel.empty()) continue;
		// Match label against verb variants
		if (!verbLabelMatches(rawLabel, normalized)) continue;

		// For talk_to, accept the verb bar match even without entrypoints; dialog
		// may not use the verb entrypoint system.
		if (normalized == "talk_to") {
			verbId = vs.verbid;
			debug(1, "mcp: resolveVerb found verbid=%d via label match (talk_to)", verbId);
			return true;
		}
		// For other verbs, verify the verb has an actual entrypoint; skip if not
		// (the verb bar text might be reused or mislabeled).
		bool hasEntrypoint = false;
		for (int oi = 1; _vm->_objs && oi < _vm->_numLocalObjects; ++oi) {
			if (!_vm->_objs[oi].obj_nr) continue;
			if (_vm->getVerbEntrypoint(_vm->_objs[oi].obj_nr, vs.verbid) != 0) {
				hasEntrypoint = true;
				break;
			}
		}
		if (!hasEntrypoint) {
			for (int ii = 0; _vm->_inventory && ii < _vm->_numInventory; ++ii) {
				int obj = _vm->_inventory[ii];
				if (!obj) continue;
				if (_vm->getVerbEntrypoint(obj, vs.verbid) != 0) {
					hasEntrypoint = true;
					break;
				}
			}
		}
		if (hasEntrypoint) {
			verbId = vs.verbid;
			debug(1, "mcp: resolveVerb found verbid=%d via label match", verbId);
			return true;
		}
	}
	// V6+: standard action verbs are image-based. Resolve by verbid directly.
	if (_vm->_game.version >= 6) {
		const V6VerbEntry *entry = nullptr;
		for (int i = 0; kV6CanonicalVerbs[i].name; ++i) {
			if (normalized == kV6CanonicalVerbs[i].name) { entry = &kV6CanonicalVerbs[i]; break; }
		}
		if (entry) {
			// Verify the verb slot is currently active (action icon bar is visible).
			for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
				const VerbSlot &vs = _vm->_verbs[slot];
				if (vs.verbid == entry->id && vs.saveid == 0 && vs.curmode != 0) {
					verbId = entry->id;
					debug(1, "mcp: resolveVerb V6 direct verbid=%d for '%s'", verbId, normalized.c_str());
					return true;
				}
			}
			// Slot not active (e.g. dialog in progress), but verb is still a known V6 verb.
			// Accept it unconditionally so the caller can still dispatch the action.
			verbId = entry->id;
			debug(1, "mcp: resolveVerb V6 verbid=%d (slot not active) for '%s'", verbId, normalized.c_str());
			return true;
		}
	}

	// V5 and below: if no text label matched for walk_to, take the first active verb slot.
	// Skipped for V6+ where walk_to is always verbid 13 and handled by kFallback below.
	if (normalized == "walk_to" && _vm->_game.version < 6) {
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (vs.verbid && vs.saveid == 0 && vs.curmode == 1) {
				verbId = vs.verbid;
				return true;
			}
		}
	}

	// Fallback for v6/v7/v8 games that use right-click context menus: the verb
	// bar is ephemeral so _verbs has no text labels during normal gameplay.
	// Try a canonical name→ID table, accepting an ID only if at least one room
	// object or inventory item actually has a script handler for it.
	static const struct { const char *name; int id; } kFallback[] = {
		{"open",    1}, {"close",   2}, {"give",    3},
		{"pick_up", 4}, {"look_at", 5}, {"talk_to", 6},
		{"use",     7}, {"push",    8}, {"pull",    9},
		{"walk_to", 13}, {nullptr,  0}
	};
	for (int fi = 0; kFallback[fi].name; ++fi) {
		if (normalized != kFallback[fi].name) continue;
		int cid = kFallback[fi].id;
		for (int oi = 1; _vm->_objs && oi < _vm->_numLocalObjects; ++oi) {
			if (!_vm->_objs[oi].obj_nr) continue;
			if (_vm->getVerbEntrypoint(_vm->_objs[oi].obj_nr, cid) != 0) {
				verbId = cid;
				return true;
			}
		}
		int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
		for (int ii = 0; _vm->_inventory && ii < _vm->_numInventory; ++ii) {
			int obj = _vm->_inventory[ii];
			if (!obj || _vm->getOwner(obj) != ego) continue;
			if (_vm->getVerbEntrypoint(obj, cid) != 0) {
				verbId = cid;
				return true;
			}
		}
	}

	return false;
}

} // End of namespace Scumm
