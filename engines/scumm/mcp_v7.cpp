/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "common/util.h"

#include "scumm/mcp_subclasses.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#ifdef ENABLE_SCUMM_7_8
#include "scumm/scumm_v7.h"
#include "scumm/insane/insane.h"
#endif

namespace Scumm {

using Networking::mcpJsonString;
using Networking::mcpObjectSchema;

// Game/version-specific MCP bridge logic for the V7 SCUMM family (The Dig,
// Full Throttle). Migrated out of mcp.cpp via the ScummMcpBridge virtual hooks.

// ---------------------------------------------------------------------------
// McpBridgeV7 — shared V7 streaming hooks (dialog verb-script + deferred clicks)
// ---------------------------------------------------------------------------

void McpBridgeV7::pumpStreamGame() {
	// If the game switched to a dialog input script (VAR_VERB_SCRIPT changed),
	// the action is still in progress — reset the settle window so we wait for
	// the dialog choices to appear rather than closing the stream prematurely.
	if (_sseVerbScript != 0 && _vm->VAR_VERB_SCRIPT != 0xFF) {
		int curVerbScript = (int)_vm->VAR(_vm->VAR_VERB_SCRIPT);
		if (curVerbScript != _sseVerbScript) {
			debug(1, "mcp: VAR_VERB_SCRIPT changed %d->%d at frame %d, resetting settle",
			      _sseVerbScript, curVerbScript, _frameCounter);
			_sseVerbScript = curVerbScript;
			_sseVerbScriptChanged = true;
			_sseDoneAtFrame = 0;
		}
	}
}

void McpBridgeV7::pumpStreamGameLate() {
	// Fire the deferred use-item scene click once the engine has had a frame to
	// commit the held-cursor state queued by the inventory click.
	if (_ssePendingV7UseClick && vmUserPut() > 0 &&
	    _frameCounter - _sseStartFrame >= 2) {
		vmMouse().x        = _ssePendingV7UseMouseX;
		vmMouse().y        = _ssePendingV7UseMouseY;
		vmVirtualMouse().x = _ssePendingV7UseObjX;
		vmVirtualMouse().y = _ssePendingV7UseObjY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = _ssePendingV7UseObjX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = _ssePendingV7UseObjY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = _ssePendingV7UseMouseX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = _ssePendingV7UseMouseY;
		vmLeftBtnPressed() |= 0x03; // msClicked | msDown
		_sseButtonClearFrame = _frameCounter + 2;
		_ssePendingV7UseClick = false;
		_sseDoneAtFrame = 0;
	}

	// Feed a deferred dialog-choice click once the game is ready. toolAnswer()
	// stores the choice here. The two V7 games render choices differently, so
	// they are dispatched differently:
	//   * The Dig — horizontal picture icons captured as blast OBJECTS, stored
	//     in ROOM coordinates (objNumber != 0). The dialog input script hit-tests
	//     VAR_VIRT_MOUSE (room space), so we point the virtual mouse at the icon,
	//     the screen mouse at the matching on-screen spot, and press the left
	//     button — exactly like a player clicking the icon.
	//   * Full Throttle — text lines captured as blast TEXT, stored in SCREEN
	//     coordinates (objNumber == 0). Its script reads VAR_MOUSE, so we keep
	//     the original screen-mouse + scene-click dispatch untouched.
	if (_ssePendingV7Choice != 0 && vmUserPut() > 0 &&
	    _frameCounter - _sseStartFrame >= 3) {
		bool haveChoice = false;
		V7Choice chosen;
		if (!_v7DialogChoices.empty()) {
			Common::Array<V7Choice> sorted = _v7DialogChoices;
			for (uint i = 0; i + 1 < sorted.size(); ++i) {
				for (uint j = 0; j + 1 < sorted.size() - i; ++j) {
					bool swap = (sorted[j].y > sorted[j + 1].y) ||
					            (sorted[j].y == sorted[j + 1].y && sorted[j].x > sorted[j + 1].x);
					if (swap) {
						V7Choice tmp = sorted[j];
						sorted[j] = sorted[j + 1];
						sorted[j + 1] = tmp;
					}
				}
			}
			int idx = CLIP<int>(_ssePendingV7Choice - 1, 0, (int)sorted.size() - 1);
			chosen = sorted[idx];
			haveChoice = true;
		}

		if (haveChoice && chosen.objNumber != 0) {
			// The Dig: room-space icon. Drive the virtual mouse + a real click.
			int roomX = chosen.x;
			int roomY = chosen.y;
			VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
			int screenX = CLIP<int>(roomX - vs->xstart, 0, _vm->_screenWidth - 1);
			int screenY = CLIP<int>(roomY - _vm->_screenTop, 0, _vm->_screenHeight - 1);
			debug(1, "mcp: feeding Dig dialog choice %d as left click at room (%d,%d) screen (%d,%d) frame %d",
			      _ssePendingV7Choice, roomX, roomY, screenX, screenY, _frameCounter);
			vmMouse().x = screenX;
			vmMouse().y = screenY;
			vmVirtualMouse().x = roomX;
			vmVirtualMouse().y = roomY;
			if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = screenX;
			if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = screenY;
			if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = roomX;
			if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = roomY;
			vmLeftBtnPressed() |= 0x03; // msClicked | msDown — a real left click
			_sseButtonClearFrame = _frameCounter + 2;
			vmRunInputScript(kSceneClickArea, 0, 1);
		} else {
			// Full Throttle (screen-space text lines) and the no-capture fallback:
			// place the screen mouse on the choice and run the scene-click input
			// script — the original, proven dispatch.
			int screenX = haveChoice ? chosen.x : 160;
			int screenY = haveChoice ? chosen.y : (163 + (_ssePendingV7Choice - 1) * 4);
			debug(1, "mcp: feeding V7 dialog choice %d as scene click at (%d,%d) frame %d",
			      _ssePendingV7Choice, screenX, screenY, _frameCounter);
			vmMouse().x = screenX;
			vmMouse().y = screenY;
			if (_vm->VAR_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_MOUSE_X) = screenX;
			if (_vm->VAR_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_MOUSE_Y) = screenY;
			vmRunInputScript(kSceneClickArea, 0, 1);
		}
		_ssePendingV7Choice = 0;
		_ssePendingV7UseClick = false;
		// Reset settle window so we capture messages produced by this choice.
		_sseDoneAtFrame = 0;
	}
}

// ---------------------------------------------------------------------------
// McpBridgeDig — The Dig (single-cursor / pie-menu model)
// ---------------------------------------------------------------------------

void McpBridgeDig::applyGameVerbs(Common::JSONArray &verbsArr,
                                  Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
	// The Dig (V7) uses a single-cursor / pie-menu interface with no persistent
	// verb bar. Expose 'interact' (universal context action) and 'use_item'
	// (inventory item on room object) — both map to verb ID 7 internally.
	if (questionPending || !activeVerbs.empty())
		return;
	static const struct { int id; const char *name; const char *label; } kV7Fallback[] = {
		{7, "interact", "interact"},
		{7, "use_item", "use item"},
		{0, nullptr,    nullptr}
	};
	for (int i = 0; kV7Fallback[i].name; ++i) {
		verbsArr.push_back(mcpJsonString(kV7Fallback[i].label));
		VerbInfo vi;
		vi.verbId = kV7Fallback[i].id;
		vi.name   = kV7Fallback[i].name;
		vi.label  = kV7Fallback[i].label;
		activeVerbs.push_back(vi);
	}
}

bool McpBridgeDig::resolveGameVerb(const Common::String &normalized, int &verbId) const {
	// The Dig (V7) uses a single-cursor click / pie-menu model. Map interact /
	// use_item (both normalize to "use") to a click sentinel and let toolAct
	// dispatch via a simulated scene click — the doSentence path used for V6
	// does not reliably trigger the per-object scripts for V7 games.
	if (normalized == "use") {
		verbId = -1;
		debug(1, "mcp: resolveVerb V7 interact/use_item -> click dispatch sentinel");
		return true;
	}
	return false;
}

void McpBridgeDig::pumpStreamGameLate() {
	// Picking up a scene object grabs it onto the mouse cursor, turning every
	// subsequent click into "use <item> on X". Each MCP action is discrete, so
	// once a pickup has added a new inventory item we deposit it by simulating
	// the player's right-click — the game's input script puts the held item back
	// into the inventory and restores the default cursor. Fire once per stream,
	// after the item appears and the game accepts input.
	if (!_sseDigDeselectDone && vmUserPut() > 0 && _frameCounter - _sseStartFrame >= 3) {
		int egoForPick = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
		bool pickedUp = false;
		uint16 *inv = vmInventory();
		for (int i = 0; inv && i < vmNumInventory() && !pickedUp; ++i) {
			uint16 obj = inv[i];
			if (!obj || vmGetOwner(obj) != egoForPick) continue;
			bool wasHeldBefore = false;
			for (uint k = 0; k < _ssePreInventory.size(); ++k)
				if (_ssePreInventory[k] == obj) { wasHeldBefore = true; break; }
			if (!wasHeldBefore) pickedUp = true;
		}
		if (pickedUp) {
			debug(1, "mcp: Dig — depositing picked-up item via right-click at frame %d", _frameCounter);
			vmRightBtnPressed() |= 0x03; // msClicked | msDown
			_sseButtonClearFrame = _frameCounter + 2;
			_sseDigDeselectDone = true;
			_sseDoneAtFrame = 0; // re-settle so the deselect completes before closing
		}
	}

	// Then run the shared V7 deferred use-item / dialog-choice clicks.
	McpBridgeV7::pumpStreamGameLate();
}

// ---------------------------------------------------------------------------
// McpBridgeFullThrottle — Full Throttle (verb-coin + INSANE bike fight)
// ---------------------------------------------------------------------------

void McpBridgeFullThrottle::registerGameTools() {
	// --- ride_bike (Full Throttle highway bike fight) ---
	Networking::McpServer::ToolSpec spec;
	spec.name = "ride_bike";
	spec.description =
	    "Ride the motorcycle. Only available once the player character has the "
	    "keys and is standing at the bike. The ride turns into a real-time fight "
	    "with a rival biker, steered by the mouse with left-click punches — too "
	    "fast to play a call at a time, so this tool plays it out: it keeps the "
	    "character alongside the enemy and punches in their direction until the "
	    "fight resolves. Blocks until the whole sequence ends, then returns what "
	    "changed. Takes no arguments.";
	spec.inputSchema  = nullptr;  // No input required
	spec.outputSchema = buildChangesSchema();
	spec.streaming    = true;
	_server->registerTool(spec);
}

Common::JSONValue *McpBridgeFullThrottle::dispatchGameTool(const Common::String &name,
                                                           const Common::JSONValue &args,
                                                           Common::String &errorOut, bool &handled) {
	if (name == "ride_bike") {
		handled = true;
		toolRideBike(args, errorOut);
		return nullptr; // streaming
	}
	return McpBridgeV7::dispatchGameTool(name, args, errorOut, handled);
}

void McpBridgeFullThrottle::applyGameVerbs(Common::JSONArray &verbsArr,
                                           Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
	// Full Throttle uses a verb-coin: holding over a hotspot pops up three icons
	// around Ben's head — fist (grab/punch/use), kick (boot), mouth (talk/look) —
	// plus a generic single-click action. Expose the three coin verbs, a generic
	// 'interact', and 'use item' (inventory item on a target). Each maps to a real
	// per-object verb script (fist=9, mouth=8, kick=5); 'interact' picks the
	// object's best available action at dispatch time.
	if (questionPending || !activeVerbs.empty())
		return;
	static const struct { int id; const char *name; const char *label; } kFtFallback[] = {
		{9,  "fist",     "fist"},
		{5,  "kick",     "kick"},
		{8,  "mouth",    "mouth"},
		{-3, "walk_to",  "walk to"},
		{-1, "interact", "interact"},
		{-1, "use_item", "use item"},
		{0,  nullptr,    nullptr}
	};
	for (int i = 0; kFtFallback[i].name; ++i) {
		verbsArr.push_back(mcpJsonString(kFtFallback[i].label));
		VerbInfo vi;
		vi.verbId = kFtFallback[i].id;
		vi.name   = kFtFallback[i].name;
		vi.label  = kFtFallback[i].label;
		activeVerbs.push_back(vi);
	}
}

bool McpBridgeFullThrottle::resolveGameVerb(const Common::String &normalized, int &verbId) const {
	// Map interact / use_item (both normalize to "use") to the click sentinel.
	if (normalized == "use") {
		verbId = -1;
		debug(1, "mcp: resolveVerb V7 interact/use_item -> click dispatch sentinel");
		return true;
	}
	// Full Throttle verb-coin verbs: fist (grab/punch/use), kick (boot), mouth
	// (talk/look) map to the per-object verb scripts 9/5/8; the generic 'interact'
	// uses the click-dispatch sentinel (-1), resolved to the object's best
	// available coin verb in toolAct().
	if (normalized == "fist")     { verbId = 9;  return true; }
	if (normalized == "kick")     { verbId = 5;  return true; }
	if (normalized == "mouth")    { verbId = 8;  return true; }
	if (normalized == "walk_to")  { verbId = -3; return true; }
	if (normalized == "interact") { verbId = -1; return true; }
	// Debug helper: "v_N"/"verb_N" dispatches an arbitrary verb id for empirical
	// mapping.
	if (_debugToolsEnabled && (normalized.hasPrefix("v_") || normalized.hasPrefix("verb_"))) {
		const char *p = normalized.c_str() + (normalized.hasPrefix("v_") ? 2 : 5);
		int id = atoi(p);
		if (id > 0) {
			verbId = id;
			return true;
		}
	}
	return false;
}

bool McpBridgeFullThrottle::dispatchGameAct(int verbId, int targetA, int targetB) {
	(void)targetB;
	if (verbId == -3 && targetA != 0) {
		// Full Throttle walk_to: walking is a scene-click in-game, and exit
		// hotspots (scene pathways, and doors once opened — e.g. the room 6 door
		// after it has been kicked open) only transition when the scene-click
		// input script runs, not via startWalkActor. Simulate a left click at the
		// target's location so the verb script walks Ben there and fires any
		// exit/room-transition handler — the same proven path toolWalk() uses for
		// the dumpster climb-out.
		int clickX = vmGetObjX(targetA);
		int clickY = vmGetObjY(targetA);
		vmMouse().x        = clickX;
		vmMouse().y        = clickY;
		vmVirtualMouse().x = clickX;
		vmVirtualMouse().y = clickY;
		if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = clickX;
		if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = clickY;
		if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = clickX;
		if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = clickY;
		vmLeftBtnPressed() |= 0x03;       // msClicked | msDown
		_debugButtonReleaseFrame = _frameCounter + 2;
		return true;
	}
	return false;
}

bool McpBridgeFullThrottle::pumpStreamGameEarly() {
	// Full Throttle bike fight (ride_bike): the ride + fight run inside INSANE's
	// own loop, during which pumpStream is not ticked at all. So we only reach
	// here before the ride starts (still room 6) or after the whole section has
	// resolved. The demo resolves the highway either to its closing narration
	// (room 48, after Ben wins the fight) or to the mechanic's shack (room 17,
	// after a wipe-out), so keep the stream open — suppressing the usual
	// room-change/settle close — until one of those is reached, then disable the
	// auto-pilot and finish. A frame cap (real, non-INSANE frames) is a safety net.
	if (!_sseFtRide)
		return false;
	bool resolved = (_vm->_currentRoom == 48 || _vm->_currentRoom == 17);
	bool capped = (_frameCounter >= _sseFtRideGiveUpFrame);
	if (resolved || capped) {
#ifdef ENABLE_SCUMM_7_8
		ScummEngine_v7 *v7 = (ScummEngine_v7 *)_vm;
		if (v7->getInsane())
			v7->getInsane()->_mcpAutoPilot = false;
#endif
		_sseFtRide = false;
		debug(1, "mcp: ride_bike finished (resolved=%d capped=%d room=%d)",
		      resolved, capped, _vm->_currentRoom);
		Common::JSONObject changes = buildStateChanges();
		_streaming = false;
		_server->endStream(new Common::JSONValue(changes), true);
		return true;
	}
	// Still waiting for INSANE to run/resolve; do not let the generic close
	// logic fire on the pre-ride frames.
	_sseLastEventFrame = _frameCounter;
	return true;
}

bool McpBridgeFullThrottle::toolRideBike(const Common::JSONValue &args, Common::String &errorOut) {
	(void)args;
	if (_streaming) {
		errorOut = "ride_bike: another action is already in progress";
		return false;
	}
	if (vmUserPut() <= 0) {
		errorOut = "ride_bike: game is not accepting input right now";
		return false;
	}
	// The bike is object 55 at the bar front (room 6); riding it needs the keys
	// (object 54, picked up from the bartender). Without them the bike just says
	// "Some joker took my keys." rather than starting the ride.
	if (_vm->_currentRoom != 6) {
		errorOut = "ride_bike: go to Ben's bike at the bar front (walk back out of the bar) first";
		return false;
	}
	int ego = (_vm->VAR_EGO != 0xFF) ? (int)_vm->VAR(_vm->VAR_EGO) : 1;
	if (vmGetOwner(54) != ego) {
		errorOut = "ride_bike: Ben needs his bike keys first — punch the bartender to get them";
		return false;
	}
#ifdef ENABLE_SCUMM_7_8
	ScummEngine_v7 *v7 = static_cast<ScummEngine_v7 *>(_vm);
	Insane *insane = v7->getInsane();
	if (!insane) {
		errorOut = "ride_bike: the action engine is not available";
		return false;
	}

	// Enable the INSANE auto-pilot so the bike fight (which runs inside INSANE's
	// own loop, out of reach of the MCP server's per-frame pump) plays itself.
	insane->_mcpAutoPilot = true;
	insane->_mcpAutoPilotFrame = 0;
#else
	errorOut = "ride_bike: Full Throttle support is not built in";
	return false;
#endif

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
	_ssePendingV7Choice = 0;
	_ssePendingV7UseClick = false;
	_sseFtRide = true;
	// Frame cap is a safety net only; _frameCounter does not advance while INSANE
	// owns the loop, so this counts real (non-INSANE) pump frames before/after.
	_sseFtRideGiveUpFrame = _frameCounter + 1200;

	// Mount the bike: dispatch the fist coin-verb on object 55, exactly as a
	// player clicking it does. resolveVerb keeps the verb id correct across builds.
	int fistVerb = 9;
	resolveVerb("fist", fistVerb);
	vmDoSentence(fistVerb, 55, 0);
	_server->startStreaming();
	return true;
}

} // End of namespace Scumm
