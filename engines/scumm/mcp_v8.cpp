/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "common/util.h"

#include "scumm/actor.h"
#include "scumm/mcp_subclasses.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"

namespace Scumm {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// Game/version-specific MCP bridge logic for the V8 SCUMM family
// (Curse of Monkey Island). Migrated out of mcp.cpp via the ScummMcpBridge
// virtual hooks.

// ---------------------------------------------------------------------------
// McpBridgeComi — Curse of Monkey Island (the cannon minigame + V8 verbs)
// ---------------------------------------------------------------------------

void McpBridgeComi::registerGameTools() {
	// --- shoot_cannon (CMI cannon minigame only) ---
	Common::JSONObject props;
	props.setVal("x", mcpProp("integer",
	    "Screen X coordinate to aim the cannon at (0–639). Pass a boat's x "
	    "from state.objects (entries named boat_1, boat_2, … carry the exact "
	    "aim point)."));
	props.setVal("y", mcpProp("integer",
	    "Screen Y coordinate to aim the cannon at (0–479). Pass a boat's y "
	    "from state.objects."));
	const char *req[] = {"x", "y"};
	Networking::McpServer::ToolSpec spec;
	spec.name = "shoot_cannon";
	spec.description =
	    "Aim the cannon at screen position (x, y) and fire a cannonball. "
	    "Only available in the Curse of Monkey Island cannon minigame. The "
	    "skeleton war-canoes to sink are listed in state as boat_N objects "
	    "with their (x, y); aim at one of those points. Moves the mouse "
	    "cursor to (x, y) and left-clicks to fire. Blocks until the shot "
	    "resolves — cannonball flight, explosion, and any resulting speech "
	    "(e.g. the skeleton crew jeering on a miss) — then returns state "
	    "changes; a sunk boat disappears from state.objects.";
	spec.inputSchema  = mcpObjectSchema(props, req, 2);
	spec.outputSchema = buildChangesSchema();
	spec.streaming    = true;
	_server->registerTool(spec);
}

Common::JSONValue *McpBridgeComi::dispatchGameTool(const Common::String &name,
                                                   const Common::JSONValue &args,
                                                   Common::String &errorOut, bool &handled) {
	if (name == "shoot_cannon") {
		handled = true;
		toolShootCannon(args, errorOut);
		return nullptr; // streaming
	}
	return McpBridgeV8::dispatchGameTool(name, args, errorOut, handled);
}

void McpBridgeComi::applyGameVerbs(Common::JSONArray &verbsArr,
                                   Common::Array<VerbInfo> &activeVerbs, bool questionPending) {
	// Curse of Monkey Island (V8) uses a single-cursor model similar to The Dig
	// and Full Throttle, with no persistent verb bar. Expose the 5 core verbs.
	// Always clear whatever the text-slot scan may have picked up (e.g. lingering
	// dialog-choice slots after a conversation ends) and replace with the fixed set.
	if (questionPending)
		return;
	verbsArr.clear();
	activeVerbs.clear();
	struct FallbackVerb { int id; const char *name; const char *label; };
	static const FallbackVerb kCMIFallback[] = {
		{13, "walk_to", "walk to"},
		{6,  "talk_to", "talk to"},
		{14, "pick_up", "pick up"},
		{5,  "look_at", "look at"},
		{7,  "use", "use"},
		{0, nullptr, nullptr}
	};
	for (int i = 0; kCMIFallback[i].name; ++i) {
		verbsArr.push_back(mcpJsonString(kCMIFallback[i].label));
		VerbInfo vi;
		vi.verbId = kCMIFallback[i].id;
		vi.name = kCMIFallback[i].name;
		vi.label = kCMIFallback[i].label;
		activeVerbs.push_back(vi);
	}
}

void McpBridgeComi::augmentStateObjects(Common::JSONArray &objects) {
	// CMI cannon minigame (the demo's room 4): the skeleton war-canoes you have
	// to sink are drawn as clusters of unnamed actor sprites (a hull plus crew),
	// not as background objects, so the normal object loop never lists them and
	// the MCP client has nothing to aim at. Surface each boat as a synthetic room
	// object carrying the cluster centre (x, y) — exactly the point to pass to
	// shoot_cannon.
	if (_vm->_currentRoom != 4)
		return;
	Common::Array<Common::Point> boatCenters;
	Common::Array<int> boatObjs;
	collectCmiCannonBoats(boatCenters, boatObjs);
	for (uint c = 0; c < boatCenters.size(); ++c) {
		Common::JSONObject boat;
		boat.setVal("id",               mcpJsonInt(boatObjs[c]));
		boat.setVal("name",             mcpJsonString(Common::String::format("boat_%u", c + 1)));
		boat.setVal("state",            mcpJsonInt(0));
		boat.setVal("x",                mcpJsonInt(boatCenters[c].x));
		boat.setVal("y",                mcpJsonInt(boatCenters[c].y));
		boat.setVal("pathway",          mcpJsonBool(false));
		Common::JSONArray bv;
		bv.push_back(mcpJsonString("shoot cannon"));
		boat.setVal("compatible_verbs", new Common::JSONValue(bv));
		objects.push_back(new Common::JSONValue(boat));
	}
}

void McpBridgeComi::augmentDebug(Common::JSONObject &out) {
	// Debug-only: CMI cannon-minigame aim state. The fire script (room-4 local
	// 2008) reads var234 (barrel target column, slewed by the mouse's X offset
	// from centre), var235 (elevation index 0..15, slewed by mouse Y / 30) and
	// var237 (which cannonball costume), and launches the ball straight up from
	// the barrel's current X. Surfacing them makes the aim observable.
	if (_vm->_currentRoom != 4)
		return;
	out.setVal("cannon_barrel_target_x", mcpJsonInt((int)_vm->VAR(234)));
	out.setVal("cannon_elevation",       mcpJsonInt((int)_vm->VAR(235)));
	out.setVal("cannon_ball_costume",    mcpJsonInt((int)_vm->VAR(237)));
	Actor *barrel = vmActorOrNull(3);
	if (barrel)
		out.setVal("cannon_barrel_x", mcpJsonInt(barrel->getRealPos().x));
}

void McpBridgeComi::augmentStateChanges(Common::JSONObject &changes) const {
	// CMI cannon minigame: report how many war-canoes are still afloat after the
	// action resolves. A sunk boat is an actor cluster vanishing, which never
	// shows up in objects_changed, so this is the signal a bench/agent needs to
	// know a shot connected (the count drops by one per boat sunk). Gate on the
	// room the action *started* in: sinking the last boat triggers the win
	// sequence and leaves room 4, and we still want to report the final 0.
	if (_ssePreRoom != 4)
		return;
	Common::Array<Common::Point> boatCenters;
	Common::Array<int> boatObjs;
	collectCmiCannonBoats(boatCenters, boatObjs);
	changes.setVal("boats_remaining", mcpJsonInt((int)boatCenters.size()));
}

bool McpBridgeComi::dispatchGameAct(int verbId, int targetA, int targetB) {
	if (verbId == 7 && targetA != 0 && targetB != 0) {
		// CMI "use A on B": the engine's sentence dispatcher uses verb id 5
		// (the same id the engine raises when the player clicks an armed
		// inventory item on a target). doSentence(7, ...) does nothing —
		// the combination table and scripted use-handlers are wired to verb 5.
		// This works uniformly for inv-on-inv (combine table → new item) and
		// inv-on-room (target's verb-5 entrypoint runs with VAR_USE_OBJECT set).
		const int kCmiUseVerb = 5;
		vmDoSentence(kCmiUseVerb, targetA, targetB);
		return true;
	}
	if (verbId == 13) {
		// CMI walk_to: verb 13 has no entrypoint in the game, so doSentence(13,...)
		// produces a "No." response. For objects with action handlers, startWalkActor
		// to the stand position is correct. For exit/pathway objects (no action handlers),
		// the game internally uses verb=1 (the walk/click verb) via doSentence — this
		// goes through the sentence script which handles room transitions. Mirror that here.
		bool hasActionHandler = (targetA != 0) &&
		    (vmGetVerbEntrypoint(targetA, 6) != 0 ||  // look_at
		     vmGetVerbEntrypoint(targetA, 7) != 0 ||  // pick_up / use
		     vmGetVerbEntrypoint(targetA, 8) != 0);   // talk_to
		if (!hasActionHandler && targetA != 0) {
			// Exit/pathway: CMI exit hotspots are activated by the game's scene-click
			// handler, which detects objects by bounding box. Simulate a left click at
			// the object's bbox center — the scene script then walks ego there and
			// triggers the room transition, exactly as a real player click would.
			int idx = vmGetObjectIndex(targetA);
			if (idx >= 0) {
				const ObjectData &od = _vm->_objs[idx];
				int clickX = od.x_pos + od.width  / 2;
				int clickY = od.y_pos + od.height / 2;
				vmMouse().x        = clickX;
				vmMouse().y        = clickY;
				vmVirtualMouse().x = clickX;
				vmVirtualMouse().y = clickY;
				if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = clickX;
				if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = clickY;
				if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = clickX;
				if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = clickY;
				vmLastInputScriptTime() = _vm->_system->getMillis();
				vmLeftBtnPressed() |= 0x03; // msClicked | msDown
			}
		} else {
			Actor *ego = getEgoActor();
			if (ego) {
				int objX = vmGetObjX(targetA);
				int objY = vmGetObjY(targetA);
				ego->startWalkActor(objX, objY, -1);
			}
		}
		return true;
	}
	return false;
}

bool McpBridgeComi::dispatchGameAnswer(const Common::Rect &slotRect, int verbid) {
	// CMI (V8) dialog choices are rendered as text lines on the screen. To
	// dispatch a choice we replicate a real click on the choice line: place the
	// mouse inside the verb slot's rect and run the verb-click input script
	// (mode 1 = activate / select).
	int mouseX = (slotRect.left + slotRect.right) / 2;
	int mouseY = (slotRect.top + slotRect.bottom) / 2;
	if (mouseX < 0) mouseX = 0;
	if (mouseX > _vm->_screenWidth - 1) mouseX = _vm->_screenWidth - 1;
	if (mouseY < 0) mouseY = 0;
	if (mouseY > _vm->_screenHeight - 1) mouseY = _vm->_screenHeight - 1;
	vmMouse().x = mouseX;
	vmMouse().y = mouseY;
	vmVirtualMouse().x = mouseX;
	vmVirtualMouse().y = mouseY;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = mouseX;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = mouseY;
	if (_vm->VAR_MOUSE_X != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_X) = mouseX;
	if (_vm->VAR_MOUSE_Y != 0xFF)      _vm->VAR(_vm->VAR_MOUSE_Y) = mouseY;
	vmRunInputScript(kVerbClickArea, verbid, 1);
	return true;
}

bool McpBridgeComi::resolveGameVerb(const Common::String &normalized, int &verbId) const {
	// Curse of Monkey Island (V8) verb IDs differ from V6. Empirically determined:
	//   verb 6 -> look_at  (e.g. "Nice cannon balls.")
	//   verb 7 -> pick_up  (e.g. "They're too heavy to carry.")
	//   verb 8 -> talk_to  (opens dialog wheel)
	//   verb 13 -> walk_to (default cursor action)
	// Must be checked before the V6+ canonical lookup which would otherwise map
	// look_at to verb 5 (the V6 canonical id, which is wrong for V8).

	// Debug helper (gated by mcp_debug): accept "v_N"/"verb_N" to dispatch
	// arbitrary verb IDs for testing.
	if (_debugToolsEnabled && (normalized.hasPrefix("v_") || normalized.hasPrefix("verb_"))) {
		const char *p = normalized.c_str() + (normalized.hasPrefix("v_") ? 2 : 5);
		int id = atoi(p);
		if (id > 0) {
			verbId = id;
			return true;
		}
	}
	static const struct { const char *name; int id; } kCMIVerbs[] = {
		{"walk_to", 13}, {"look_at", 6}, {"pick_up", 7},
		{"talk_to",  8}, {"use",     7}, {nullptr,   0}
	};
	for (int ci = 0; kCMIVerbs[ci].name; ++ci) {
		if (normalized == kCMIVerbs[ci].name) {
			verbId = kCMIVerbs[ci].id;
			debug(1, "mcp: resolveVerb CMI '%s' -> verbid=%d", normalized.c_str(), verbId);
			return true;
		}
	}
	return false;
}

void McpBridgeComi::classifyGameEntity(int numId, bool &isPathway) const {
	// CMI (V8): exit hotspots have no action handlers (ep 6/7/8 all 0) — mark as pathway.
	if (!isPathway &&
	    vmGetVerbEntrypoint(numId, 6) == 0 &&
	    vmGetVerbEntrypoint(numId, 7) == 0 &&
	    vmGetVerbEntrypoint(numId, 8) == 0) {
		isPathway = true;
	}
}

void McpBridgeComi::pumpStreamGame() {
	// CMI cannon minigame aim. The room-4 control script (local 2008/2007) aims
	// the cannon through two of its own globals: VAR(234) is the barrel's target
	// column (the barrel actor walks toward it) and VAR(235) is the elevation
	// index 0..12. The fire script samples getObjectX(3) for the launch column
	// and shoots the ball straight up, so it lands at
	//     (barrelColumn, 320 - elevation*18).
	// Script 2007 normally slews those two globals from the mouse: it nudges
	// VAR(234) by +/-10 while the cursor X sits outside a +/-10 px deadzone of
	// screen centre (320), and slews VAR(235) toward VAR_VIRT_MOUSE_Y / 30.
	// Rather than fight that velocity-style control, we park the cursor in the
	// centre/elevation deadzone (so 2007 leaves the globals alone) and write the
	// exact target column and elevation ourselves. _sseCannonAimX is the desired
	// barrel column and _sseCannonAimY the elevation index (see toolShootCannon).
	if (!_sseCannonAiming)
		return;
	Actor *cannon = vmActorOrNull(3);
	int barrelX = cannon ? cannon->getRealPos().x : _sseCannonAimX;
	// Park the cursor: X in the centre deadzone (no var234 drift); Y on the
	// elevation row (y/30 == target so var235 does not slew).
	int holdX = 320;
	int holdY = _sseCannonAimY * 30 + 15;
	vmMouse().x        = holdX;
	vmMouse().y        = holdY;
	vmVirtualMouse().x = holdX;
	vmVirtualMouse().y = holdY;
	if (_vm->VAR_VIRT_MOUSE_X != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_X) = holdX;
	if (_vm->VAR_VIRT_MOUSE_Y != 0xFF) _vm->VAR(_vm->VAR_VIRT_MOUSE_Y) = holdY;
	vmLastInputScriptTime() = _vm->_system->getMillis();
	// Force the exact aim (the room-4 fire/aim globals are confirmed to live
	// at 234/235 for this game; gated to room 4 by toolShootCannon).
	if (vmNumVariables() > 235) {
		_vm->VAR(234) = _sseCannonAimX;   // barrel target column
		_vm->VAR(235) = _sseCannonAimY;   // elevation index
	}
	_sseLastEventFrame = _frameCounter; // not "stuck" while aiming
	// Fire once the barrel has actually reached the target column (the fire
	// script samples its live X) — or after a safety cap on the swing.
	if (ABS(_sseCannonAimX - barrelX) <= 3 || !cannon ||
	    _frameCounter >= _sseCannonGiveUpFrame) {
		vmLeftBtnPressed() |= 0x03; // msClicked | msDown — fire
		_sseButtonClearFrame = _frameCounter + 2;
		_sseCannonAiming = false;
	}
}

bool McpBridgeComi::toolShootCannon(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "shoot_cannon: another action is already in progress";
		return false;
	}
	// The aim drives the room-4 cannon's own control globals (VAR 234/235); those
	// indices mean other things elsewhere, so only fire inside the minigame room.
	if (_vm->_currentRoom != 4) {
		errorOut = "shoot_cannon: only available in the cannon minigame (use the "
		           "cannon from the cannon room to enter it)";
		return false;
	}
	if (vmUserPut() <= 0) {
		errorOut = "shoot_cannon: game is not accepting input right now";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "shoot_cannon: arguments must be an object";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "shoot_cannon: integer 'x' and 'y' coordinates are required";
		return false;
	}

	int x = (int)a["x"]->asIntegerNumber();
	int y = (int)a["y"]->asIntegerNumber();
	int maxX = (_vm->_screenWidth  > 0 ? _vm->_screenWidth  : 640) - 1;
	int maxY = (_vm->_screenHeight > 0 ? _vm->_screenHeight : 480) - 1;
	x = CLIP<int>(x, 0, maxX);
	y = CLIP<int>(y, 0, maxY);

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
	_sseButtonClearFrame = 0; // set when the deferred fire actually clicks
	_ssePendingV7Choice = 0;
	_ssePendingV7UseClick = false;
	_sseVerbScript = 0;
	_sseInitialVerbScript = 0;
	_sseVerbScriptChanged = false;
	_sseTargetObject = 0;
	// Map the target screen point (x, y) — a boat_N centre from state — onto the
	// cannon's two control parameters, derived from the room-4 fire script:
	// the ball launches straight up from the barrel column and lands at
	//     (barrelColumn, 320 - elevation*18),   elevation in 0..12.
	// So the barrel column equals the target X (clamped to the barrel's travel
	// range 140..520), and the elevation is the index whose landing row is
	// closest to the target Y. pumpStreamGame drives the cannon to these and fires.
	int barrelTarget = CLIP<int>(x, 140, 520);
	int elevIndex = (320 - y + 9) / 18;          // round((320 - y) / 18)
	elevIndex = CLIP<int>(elevIndex, 0, 12);
	_sseCannonAimX = barrelTarget;
	_sseCannonAimY = elevIndex;
	_sseCannonAiming = true;
	_sseCannonGiveUpFrame = _frameCounter + 240; // safety cap on the barrel swing
	_server->startStreaming();
	return true;
}

void McpBridgeComi::collectCmiCannonBoats(Common::Array<Common::Point> &centers,
                                          Common::Array<int> &repObjs) const {
	centers.clear();
	repObjs.clear();
	if (_vm->_currentRoom != 4)
		return;
	struct BoatCluster { int sumX; int sumY; int n; int repObj; };
	Common::Array<BoatCluster> boats;
	for (int i = 1; i < vmNumActors(); ++i) {
		Actor *a = vmActorOrNull(i);
		if (!a || !a->isInCurrentRoom() || !a->_visible)
			continue;
		int ax = a->getRealPos().x, ay = a->getRealPos().y;
		if (ax <= 0 || ay <= 0)
			continue;
		if (ay < 150 || ay > 360)                 // the sea band
			continue;
		if (a->_costume < 18 || a->_costume > 24) // war-canoe hull/crew sprites
			continue;
		bool placed = false;
		for (uint c = 0; c < boats.size(); ++c) {
			int cx = boats[c].sumX / boats[c].n;
			int cy = boats[c].sumY / boats[c].n;
			if (ABS(cx - ax) <= 45 && ABS(cy - ay) <= 60) {
				boats[c].sumX += ax;
				boats[c].sumY += ay;
				boats[c].n++;
				placed = true;
				break;
			}
		}
		if (!placed) {
			BoatCluster bc;
			bc.sumX = ax;
			bc.sumY = ay;
			bc.n = 1;
			bc.repObj = vmActorToObj(a->_number);
			boats.push_back(bc);
		}
	}
	// Stable left-to-right ordering so boat_1 is always the leftmost.
	for (uint p = 0; p + 1 < boats.size(); ++p)
		for (uint q = 0; q + 1 < boats.size() - p; ++q)
			if (boats[q].sumX / boats[q].n > boats[q + 1].sumX / boats[q + 1].n) {
				BoatCluster t = boats[q];
				boats[q] = boats[q + 1];
				boats[q + 1] = t;
			}
	for (uint c = 0; c < boats.size(); ++c) {
		centers.push_back(Common::Point(boats[c].sumX / boats[c].n, boats[c].sumY / boats[c].n));
		repObjs.push_back(boats[c].repObj);
	}
}

} // End of namespace Scumm
