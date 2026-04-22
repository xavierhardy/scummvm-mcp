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

// Return the display name of an object/actor. Empty string if unnamed.
Common::String getObjName(const ScummMcpBridge *bridge, int obj) {
	if (!bridge) return "";
	const byte *name = bridge->callGetObjOrActorName(obj);
	if (!name || !*name) return "";
	return Common::String((const char *)name);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ScummMcpBridge::ScummMcpBridge(ScummEngine *vm)
	: _vm(vm),
	  _enabled(false),
	  _server(nullptr),
	  _nextMessageSeq(1),
	  _frameCounter(0),
	  _streaming(false),
	  _sseStartFrame(0),
	  _sseDoneAtFrame(0),
	  _sseStuckAtFrame(0),
	  _sseEgoMoved(false),
	  _ssePreRoom(0),
	  _ssePrePosX(0),
	  _ssePrePosY(0) {
	if (!_vm) return;

	_enabled = ConfMan.getBool("mcp");
	if (!_enabled) return;

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
		outputProps.setVal("room", mcpProp("integer", "Current room number"));
		outputProps.setVal("room_name", mcpProp("string", "Human-readable room name (optional)"));

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
		objectItemProps.setVal("visible",         mcpProp("boolean", "Visibility status"));
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
		    "and pending dialog question if any. "
		    "Objects include both visible and invisible ones (check the 'visible' field); "
		    "the player character is never listed. "
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
		props.setVal("verb",    mcpProp("string", "Verb name (e.g. 'open', 'use', 'look_at', 'walk_to'). Required."));
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
	errorOut = "Unknown tool: " + name;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::JSONValue *ScummMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	out.setVal("room", mcpJsonInt(_vm->_currentRoom));

	if (_vm->_objs && _vm->_numLocalObjects > 0 && _vm->_objs[0].obj_nr) {
		Common::String rn = getObjName(this, _vm->_objs[0].obj_nr);
		if (!rn.empty()) {
			rn = normalizeActionName(rn);
			bool hasCtrl = false;
			for (uint ci = 0; ci < rn.size(); ++ci)
				if ((unsigned char)rn[ci] < 0x20) { hasCtrl = true; break; }
			if (!hasCtrl)
				out.setVal("room_name", mcpJsonString(mcpSanitizeString(rn)));
		}
	}

	Actor *ego = getEgoActor();
	if (ego) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(ego->getRealPos().x));
		pos.setVal("y", mcpJsonInt(ego->getRealPos().y));
		out.setVal("position", new Common::JSONValue(pos));
	}

	struct VerbInfo { int verbId; Common::String name; };
	Common::Array<VerbInfo> activeVerbs;
	Common::JSONArray verbsArr;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || !vs.key) continue;
		const byte *ptr2 = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr2) continue;
		byte textBuf2[256];
		_vm->convertMessageToString(ptr2, textBuf2, sizeof(textBuf2));
		Common::String label = mcpLowerTrimmed((const char *)textBuf2);
		if (label.empty()) continue;
		bool labelHasCtrl = false;
		for (uint ci = 0; ci < label.size(); ++ci)
			if ((unsigned char)label[ci] < 0x20) { labelHasCtrl = true; break; }
		if (labelHasCtrl) continue;
		Common::String safe2 = mcpSanitizeString(normalizeActionName(label));
		verbsArr.push_back(mcpJsonString(safe2));
		VerbInfo vi;
		vi.verbId = vs.verbid;
		vi.name   = safe2;
		activeVerbs.push_back(vi);
	}
	out.setVal("verbs", new Common::JSONValue(verbsArr));

	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);

	Common::JSONArray inventory, objects;
	for (uint i = 0; i < entities.size(); ++i) {
		const NamedEntity &ne = entities[i];
		Common::String safe = mcpSanitizeString(ne.displayName);
		switch (ne.kind) {
		case NamedEntity::kInventory:
			inventory.push_back(mcpJsonString(safe));
			break;
		case NamedEntity::kObject: {
			Common::JSONArray compatVerbs;
			bool hasLookAt = false, hasWalkTo = false;
			int handlerCount = 0;
			bool walkToHasHandler = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (_vm->getVerbEntrypoint(ne.numId, activeVerbs[k].verbId) != 0) {
					compatVerbs.push_back(mcpJsonString(activeVerbs[k].name));
					handlerCount++;
					if (activeVerbs[k].name == "look_at") hasLookAt = true;
					if (activeVerbs[k].name == "walk_to") { hasWalkTo = true; walkToHasHandler = true; }
				}
			}
			if (!hasLookAt) compatVerbs.push_back(mcpJsonString("look_at"));
			if (!hasWalkTo) compatVerbs.push_back(mcpJsonString("walk_to"));

			bool isPathway = walkToHasHandler && (handlerCount == 1);

			Common::JSONObject obj;
			obj.setVal("id",               mcpJsonInt(ne.numId));
			obj.setVal("name",             mcpJsonString(safe));
			obj.setVal("state",            mcpJsonInt(_vm->getState(ne.numId)));
			obj.setVal("visible",          mcpJsonBool(ne.visible));
			obj.setVal("pathway",          mcpJsonBool(isPathway));
			obj.setVal("compatible_verbs", new Common::JSONValue(compatVerbs));
			objects.push_back(new Common::JSONValue(obj));
			break;
		}
		case NamedEntity::kActor: {
			int actorObjId = _vm->actorToObj(ne.numId);
			Common::JSONArray compatVerbs;
			bool hasTalkTo = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (_vm->getVerbEntrypoint(actorObjId, activeVerbs[k].verbId) != 0) {
					compatVerbs.push_back(mcpJsonString(activeVerbs[k].name));
					if (activeVerbs[k].name == "talk_to") hasTalkTo = true;
				}
			}
			if (!hasTalkTo) compatVerbs.push_back(mcpJsonString("talk_to"));
			Common::JSONObject actorObj;
			actorObj.setVal("id",               mcpJsonInt(actorObjId));
			actorObj.setVal("name",             mcpJsonString(safe));
			actorObj.setVal("state",            mcpJsonInt(_vm->getState(actorObjId)));
			actorObj.setVal("visible",          mcpJsonBool(ne.visible));
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
		Common::JSONObject entry;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			Common::String actorName = getObjName(this, objId);
			if (!actorName.empty()) {
				Common::String safe = mcpSanitizeString(mcpLowerTrimmed(actorName));
				entry.setVal("actor", mcpJsonString(safe));
			}
		}
		entry.setVal("text", mcpJsonString(mcpSanitizeString(m.text)));
		msgsArr.push_back(new Common::JSONValue(entry));
	}
	out.setVal("messages", new Common::JSONValue(msgsArr));
	_messages.clear();

	int choiceCount = 0;
	Common::JSONArray choiceList;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		if (!hasPendingQuestion()) break;
		Common::JSONObject choice;
		choice.setVal("id",    mcpJsonInt(++choiceCount));
		choice.setVal("label", mcpJsonString(mcpSanitizeString(Common::String((const char *)textBuf))));
		choiceList.push_back(new Common::JSONValue(choice));
	}
	if (choiceCount > 0) {
		Common::JSONObject question;
		question.setVal("choices", new Common::JSONValue(choiceList));
		out.setVal("question", new Common::JSONValue(question));
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
				errorOut = Common::String("act: unknown ") + param + " '" + v->asString() + "'";
				return false;
			}
			out = (ent.kind == NamedEntity::kActor) ? _vm->actorToObj(ent.numId) : ent.numId;
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
	if (targetA != 0) {
		int ep = _vm->getVerbEntrypoint(targetA, verbId);
		debug(1, "mcp: act entrypoint for obj %d verb %d = %d", targetA, verbId, ep);
	}

	snapshotPreAction();
	_streaming = true;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
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
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		++current;
		if (current == choiceIdx) { chosenSlot = slot; break; }
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
	_sseEgoMoved = false;
	_sseMessages.clear();
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
	_sseEgoMoved = false;
	_sseMessages.clear();
	ego->startWalkActor(wx, wy, -1);
	_server->startStreaming();
	return true;
}

// ---------------------------------------------------------------------------
// Streaming pump
// ---------------------------------------------------------------------------

void ScummMcpBridge::emitPendingMessages() {
	while (!_messages.empty()) {
		const MessageEntry &m = _messages[0];
		_sseMessages.push_back(m);
		Common::JSONObject params;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			Common::String actorName = getObjName(this, objId);
			if (!actorName.empty()) {
				Common::String safe = mcpSanitizeString(mcpLowerTrimmed(actorName));
				params.setVal("actor", mcpJsonString(safe));
			}
		}
		params.setVal("text", mcpJsonString(mcpSanitizeString(m.text)));
		params.setVal("type", mcpJsonString(mcpSanitizeString(m.type)));
		_server->emitNotification(params);
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

	// Early-exit: stuck (ego idle, no speech, user-put locked) for 90 frames.
	{
		Actor *ego = getEgoActor();
		bool egoIdle = !ego || !ego->_moving;
		bool stuck = egoIdle && _vm->_talkDelay == 0 && _vm->_userPut <= 0;
		if (stuck) {
			if (_sseStuckAtFrame == 0) _sseStuckAtFrame = _frameCounter;
			if (_frameCounter - _sseStuckAtFrame > 90) {
				debug(1, "mcp: action stuck for 90 frames — closing stream");
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
			debug(1, "mcp: action looks done at frame %d, settling (egoMoved=%d)",
			      _frameCounter, _sseEgoMoved);
		}
		bool questionReady = hasPendingQuestion();
		// After a walk the sentence script needs several extra frames to face the
		// actor and set up dialog choices; use a 45-frame window in that case.
		uint32 settleFrames = _sseEgoMoved ? 45 : 15;
		bool settled = _frameCounter - _sseDoneAtFrame >= settleFrames;
		if (questionReady || settled) {
			debug(1, "mcp: closing stream at frame %d (question=%d, settled=%d)",
				_frameCounter, questionReady, settled);
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
	for (int i = 0; _vm->_inventory && i < _vm->_numInventory; ++i)
		_ssePreInventory.push_back(_vm->_inventory[i]);
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
		ObjStateSnap snap;
		snap.objNr = od.obj_nr;
		snap.state = _vm->getState(od.obj_nr);
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
		if (_vm->getOwner(obj) != ego) continue;
		bool wasPresent = false;
		for (uint j = 0; j < _ssePreInventory.size(); ++j)
			if (_ssePreInventory[j] == obj) { wasPresent = true; break; }
		if (!wasPresent) {
			Common::String name = getObjName(this, obj);
			if (name.empty()) name = Common::String::format("obj-%d", obj);
			added.push_back(mcpJsonString(mcpSanitizeString(mcpLowerTrimmed(name))));
		}
	}
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

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
		int newState = _vm->getState(od.obj_nr);
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
			Common::JSONObject m;
			m.setVal("text", mcpJsonString(me.text));
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
		for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
			const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr) continue;
			byte textBuf[256];
			_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
			if (!textBuf[0]) continue;
			Common::JSONObject choice;
			choice.setVal("id",    mcpJsonInt(++choiceCount));
			choice.setVal("label", mcpJsonString(mcpSanitizeString(Common::String((const char *)textBuf))));
			choiceList.push_back(new Common::JSONValue(choice));
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
	if (ego && ego->_moving) return false;
	if (_vm->_talkDelay > 0) return false;
	if (_vm->_userPut <= 0) return false;
	return true;
}

bool ScummMcpBridge::hasPendingQuestion() const {
	if (!_vm || _vm->_userPut <= 0) return false;
	bool hasKeyed = false, hasUnkeyed = false, hasNumericKeyed = false;
	for (int slot = 1; _vm->_verbs && slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
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
	// MI1-style: dialog choices are unkeyed, normal verb bar is keyed (or absent)
	if (hasUnkeyed && !hasKeyed) return true;
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
	if (s == "examine") return "look_at";
	if (s == "pick")    return "pick_up";
	if (s == "pickup")  return "pick_up";
	if (s == "take")    return "pick_up";
	if (s == "get")     return "pick_up";
	if (s == "talk")    return "talk_to";
	return s;
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
		Common::String name = getObjName(this, od.obj_nr);
		RawEntry e;
		e.kind = NamedEntity::kObject;
		e.numId = od.obj_nr;
		e.baseName = name.empty() ? Common::String::format("obj-%d", od.obj_nr)
		                          : normalizeActionName(name);
		e.visible = (od.state & 0xF) != 0;
		if (e.visible && od.parent != 0 && od.parent < _vm->_numLocalObjects)
			e.visible = ((_vm->_objs[od.parent].state & 0xF) == od.parentstate);
		// Detect pathway objects (walk_to is their sole verb handler).
		if (!e.visible) {
			bool hasWalkTo = false, hasOther = false;
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
		if (!a || !a->isInCurrentRoom()) continue;
		if (a->_number == egoNum) continue;
		int objId = _vm->actorToObj(a->_number);
		Common::String name = getObjName(this, objId);
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
	// Prefer actors: an actor and its room object share a name; kActor is authoritative.
	int firstMatch = -1;
	for (uint i = 0; i < entities.size(); ++i) {
		if (entities[i].displayName != normalized) continue;
		if (entities[i].kind == NamedEntity::kActor) { out = entities[i]; return true; }
		if (firstMatch < 0) firstMatch = (int)i;
	}
	if (firstMatch >= 0) { out = entities[firstMatch]; return true; }
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
		Common::String label = normalizeActionName((const char *)textBuf);
		debug(1, "mcp:   slot=%d verbid=%d saveid=%d curmode=%d key=%d label='%s'",
		      slot, vs.verbid, vs.saveid, vs.curmode, vs.key, label.c_str());
		if (vs.saveid != 0) continue;
		if (!ptr) continue;
		if (label.empty()) continue;
		if (label == normalized || label.contains(normalized)) {
			verbId = vs.verbid;
			debug(1, "mcp: resolveVerb found verbid=%d via label match", verbId);
			return true;
		}
	}
	if (normalized == "walk_to") {
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
