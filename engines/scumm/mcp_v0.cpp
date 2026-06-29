/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "scumm/mcp_subclasses.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v0.h"
#include "scumm/verbs.h"

namespace Scumm {

using Networking::mcpJsonString;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// Game/version-specific MCP bridge logic for the V0 SCUMM family (Maniac
// Mansion). Migrated out of mcp.cpp via the ScummMcpBridge virtual hooks.

// ---------------------------------------------------------------------------
// McpBridgeManiac — Maniac Mansion (kid switching + phone dial pad)
// ---------------------------------------------------------------------------

void McpBridgeManiac::registerGameTools() {
	// --- switch_character ---
	// V0 (C64/Apple II) maps F1-F3 to switchActor(slot)/VAR(97+slot); the V1/V2
	// ports use the in-game "New Kid" verb but share the same ego/kid vars, so
	// the tool drives the switch directly for them.
	{
		Common::JSONObject props;
		props.setVal("name", mcpProp("string",
		    "Name of the kid to control, as listed in state.available_characters (e.g. 'dave')."));
		const char *req[] = {"name"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "switch_character";
		spec.description =
		    "Switch the player-controlled kid (the F1-F3 keys in Maniac Mansion). "
		    "state lists the available names in 'available_characters' and the "
		    "current one in 'controlling'. Only allowed during normal gameplay (not "
		    "in a cutscene and not while kid switching is disabled). Blocks until "
		    "the switch settles, then returns state changes — room_changed/position "
		    "reflect the newly controlled kid.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- dial (phone keypad) ---
	{
		Common::JSONObject props;
		props.setVal("number", mcpProp("string",
		    "The number to dial, as a string of keypad keys: digits 0-9 plus "
		    "'*' and '#' (e.g. '1234')."));
		const char *req[] = {"number"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "dial";
		spec.description =
		    "Dial a number on the phone dial pad in Maniac Mansion. Only valid "
		    "while the dial pad is on screen (use the phone first via "
		    "act(verb='use', target1='phone')). Presses the keypad buttons one "
		    "at a time, blocks until the sequence (and any resulting call) "
		    "settles, then returns state changes.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}
}

Common::JSONValue *McpBridgeManiac::dispatchGameTool(const Common::String &name,
                                                     const Common::JSONValue &args,
                                                     Common::String &errorOut, bool &handled) {
	if (name == "switch_character") {
		handled = true;
		toolSwitchCharacter(args, errorOut);
		return nullptr; // streaming
	}
	if (name == "dial") {
		handled = true;
		toolDial(args, errorOut);
		return nullptr; // streaming
	}
	return McpBridgeV0::dispatchGameTool(name, args, errorOut, handled);
}

void McpBridgeManiac::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("controlling", mcpProp("string", "Name of the currently controlled kid"));
	Common::JSONObject arr;
	arr.setVal("type",  mcpJsonString("array"));
	arr.setVal("items", mcpProp("string"));
	outputProps.setVal("available_characters", new Common::JSONValue(arr));
}

void McpBridgeManiac::augmentState(Common::JSONObject &out) {
	// Expose the switchable kids and the current one so clients can drive the
	// switch_character tool by name.
	Common::Array<ManiacKid> kids;
	collectManiacKids(kids);
	if (kids.empty())
		return;
	int egoNum = (_vm->VAR_EGO != 0xFF) ? (int)_vm->VAR(_vm->VAR_EGO) : -1;
	Common::JSONArray charArr;
	for (uint i = 0; i < kids.size(); ++i) {
		charArr.push_back(mcpJsonString(kids[i].name));
		if (kids[i].actorId == egoNum)
			out.setVal("controlling", mcpJsonString(kids[i].name));
	}
	out.setVal("available_characters", new Common::JSONValue(charArr));
}

void McpBridgeManiac::pumpStreamGame() {
	// Phone dial pad: press the queued keypad buttons one at a time. Wait for the
	// previous press's sentence to dispatch (the keypad scripts run without
	// walking) and leave a few frames between presses so each button script
	// finishes before the next begins.
	const uint32 kDialSpacingFrames = 12;
	if (!_ssePendingDialObjs.empty() && _vm->_sentenceNum == 0 &&
	    (_sseLastDialFedFrame == 0
	     || _frameCounter - _sseLastDialFedFrame >= kDialSpacingFrames)) {
		int obj = _ssePendingDialObjs[0];
		_ssePendingDialObjs.remove_at(0);
		_sseLastDialFedFrame = _frameCounter;
		_sseLastEventFrame = _frameCounter;
		vmDoSentence(_sseDialVerbId, obj, 0);
	}
}

void McpBridgeManiac::resetGameStream() {
	_ssePendingDialObjs.clear();
	_sseLastDialFedFrame = 0;
}

bool McpBridgeManiac::gameStreamBusy() const {
	return !_ssePendingDialObjs.empty();
}

void McpBridgeManiac::collectManiacKids(Common::Array<ManiacKid> &out) const {
	out.clear();
	// V0's F1-F3 handler maps slot N to the actor stored in VAR(97+N) (see
	// ScummEngine_v0::switchActor); the V1/V2 ports keep the same kid vars.
	// Slots holding no valid actor are skipped, so on a variant where these
	// vars are unused the list simply comes out empty.
	for (int slot = 0; slot < 3; ++slot) {
		int actorId = (int)_vm->VAR(97 + slot);
		if (actorId <= 0 || !_vm->isValidActor(actorId)) continue;
		ManiacKid kid;
		kid.slot = slot;
		kid.actorId = actorId;
		Common::String name = objName(vmActorToObj(actorId));
		kid.name = name.empty() ? Common::String::format("actor-%d", actorId)
		                        : normalizeActionName(safeUtf8(name));
		out.push_back(kid);
	}
}

bool McpBridgeManiac::toolSwitchCharacter(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "switch_character: another action is already in progress";
		return false;
	}
	if (vmUserPut() <= 0) {
		errorOut = "switch_character: game is not accepting input right now";
		return false;
	}
	// V0: mirror switchActor()'s own gate so the client gets an error instead
	// of a silent no-op when switching is disallowed (cutscene, keypad, lab
	// door). V1/V2 have no equivalent mode byte; _userPut covers them above.
	if (_vm->_game.version == 0 && !v0InNormalMode()) {
		errorOut = "switch_character: switching is not allowed right now (cutscene or kid switching disabled)";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "switch_character: arguments must be an object with a 'name' field";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("name") || !a["name"]->isString()) {
		errorOut = "switch_character: 'name' (string) is required";
		return false;
	}

	Common::Array<ManiacKid> kids;
	collectManiacKids(kids);
	Common::String wanted = normalizeActionName(a["name"]->asString());
	const ManiacKid *match = nullptr;
	Common::String available;
	for (uint i = 0; i < kids.size(); ++i) {
		if (!available.empty()) available += ", ";
		available += kids[i].name;
		if (kids[i].name == wanted) match = &kids[i];
	}
	if (!match) {
		errorOut = "switch_character: unknown character '" + a["name"]->asString() +
		           "'. Available: " + (available.empty() ? "(none)" : available);
		return false;
	}

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;
	_sseButtonClearFrame = 0;
	if (_vm->_game.version == 0) {
		v0SwitchActor(match->slot);
	} else {
		// V1/V2: no engine-side helper exists (the original ports switch via
		// the "New Kid" verb script), so replicate V0's switchActor() body.
		vmResetSentence();
		_vm->VAR(_vm->VAR_EGO) = match->actorId;
		vmActorFollowCamera(match->actorId);
	}
	_server->startStreaming();
	return true;
}

bool McpBridgeManiac::toolDial(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "dial: another action is already in progress";
		return false;
	}
	if (vmUserPut() <= 0) {
		errorOut = "dial: game is not accepting input right now";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("number") ||
	    !args.asObject()["number"]->isString()) {
		errorOut = "dial: 'number' (string of keypad keys, e.g. '1234') is required";
		return false;
	}
	Common::String number = args.asObject()["number"]->asString();
	number.trim();
	if (number.empty() || number.size() > 16) {
		errorOut = "dial: 'number' must contain 1-16 keypad keys";
		return false;
	}
	for (uint i = 0; i < number.size(); ++i) {
		char c = number[i];
		if (!(c >= '0' && c <= '9') && c != '*' && c != '#') {
			errorOut = Common::String::format("dial: invalid keypad key '%c' (use 0-9, * or #)", c);
			return false;
		}
	}
	// V0 tracks the dial pad (and other selection screens) via _currentMode.
	// V1/V2 have no mode byte; for them the button-map scan below is the gate.
	if (_vm->_game.version == 0 && !v0InKeypadMode()) {
		errorOut = "dial: no dial pad on screen — use the phone first (act verb='use' target1='phone')";
		return false;
	}

	// Build the key -> button-object map for the current room.
	// Strategy 1: buttons named after their key ("1".."9", "0", "*", "#").
	// Strategy 2: the C64 demo's buttons are unnamed — but the pad is exactly
	// 12 equal-sized button objects in the standard 3x4 phone grid, so sort
	// them row-major and assign the layout by position. (Object 427 carries
	// the name "6" in the demo and lands on '6' this way, confirming the
	// mapping.)
	static const char kDialPadLayout[] = "123456789*0#";
	struct DialButton { int obj; int x; int y; };
	Common::Array<DialButton> grid;
	int gridObjForKey[12] = {};
	int nameObjForKey[12] = {};
	auto layoutIndex = [](char c) -> int {
		for (int k = 0; k < 12; ++k)
			if (kDialPadLayout[k] == c) return k;
		return -1;
	};
	{
		// Dominant button size among the room objects.
		int bestW = 0, bestH = 0, bestCount = 0;
		for (int i = 1; _vm->_objs && i < vmNumLocalObjects(); ++i) {
			const ObjectData &od = _vm->_objs[i];
			if (!od.obj_nr || od.width <= 0 || od.height <= 0) continue;
			int cnt = 0;
			for (int j = 1; j < vmNumLocalObjects(); ++j) {
				const ObjectData &o2 = _vm->_objs[j];
				if (o2.obj_nr && o2.width == od.width && o2.height == od.height) ++cnt;
			}
			if (cnt > bestCount) { bestCount = cnt; bestW = od.width; bestH = od.height; }
		}
		for (int i = 1; _vm->_objs && i < vmNumLocalObjects(); ++i) {
			const ObjectData &od = _vm->_objs[i];
			if (!od.obj_nr) continue;
			// Named buttons map directly regardless of geometry.
			Common::String nm = objName(od.obj_nr);
			nm.trim();
			if (nm.size() == 1) {
				int k = layoutIndex(nm[0]);
				if (k >= 0 && !nameObjForKey[k]) nameObjForKey[k] = od.obj_nr;
			}
			if (od.width == bestW && od.height == bestH) {
				DialButton b;
				b.obj = od.obj_nr;
				b.x = od.x_pos;
				b.y = od.y_pos;
				grid.push_back(b);
			}
		}
		if (grid.size() == 12) {
			// Row-major sort (top-to-bottom, left-to-right).
			for (uint i = 0; i + 1 < grid.size(); ++i)
				for (uint j = 0; j + 1 < grid.size() - i; ++j)
					if (grid[j].y > grid[j + 1].y ||
					    (grid[j].y == grid[j + 1].y && grid[j].x > grid[j + 1].x)) {
						DialButton t = grid[j]; grid[j] = grid[j + 1]; grid[j + 1] = t;
					}
			for (int k = 0; k < 12; ++k)
				gridObjForKey[k] = grid[(uint)k].obj;
		}
	}

	Common::Array<int> presses;
	for (uint i = 0; i < number.size(); ++i) {
		int k = layoutIndex(number[i]);
		int obj = nameObjForKey[k] ? nameObjForKey[k] : gridObjForKey[k];
		if (!obj) {
			errorOut = Common::String::format(
			    "dial: could not locate the '%c' button — is the dial pad on screen?", number[i]);
			return false;
		}
		presses.push_back(obj);
	}

	// The keypad buttons respond to the push verb: V0's input handler forces
	// _activeVerb = kVerbPush while in keypad mode; for V1/V2 resolve the verb
	// from the verb bar like act() does.
	int pushVerb = kVerbPush;
	if (_vm->_game.version != 0 && !resolveVerb("push", pushVerb)) {
		errorOut = "dial: could not resolve the 'push' verb";
		return false;
	}

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;
	_sseButtonClearFrame = 0;
	// Queue after snapshotPreAction (which clears the dial queue); pumpStream
	// feeds one press per spacing window starting next frame.
	_ssePendingDialObjs = presses;
	_sseDialVerbId = pushVerb;
	_sseLastDialFedFrame = 0;
	_server->startStreaming();
	return true;
}

} // End of namespace Scumm
