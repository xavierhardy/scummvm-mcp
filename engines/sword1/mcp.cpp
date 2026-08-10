/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "sword1/mcp.h"
#include "sword1/mcp_names.h"

#include "sword1/control.h"
#include "sword1/logic.h"
#include "sword1/menu.h"
#include "sword1/mouse.h"
#include "sword1/object.h"
#include "sword1/objectman.h"
#include "sword1/sword1.h"
#include "sword1/sworddefs.h"

#include "common/debug.h"
#include "common/events.h"
#include "common/system.h"

namespace Sword1 {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpPropOneOf;
using Networking::mcpObjectSchema;

// The verbs the bridge accepts. Broken Sword has no verb bar: a left click runs
// an object's interaction script and a right click looks at it, so every verb
// but look_at maps onto the same left-click path. They are still listed
// separately because an agent reasons in verbs, and because state.verbs is what
// tells it which words the server will accept.
static const char *const kVerbs[] = {
	"look_at", "interact", "use", "talk_to", "pick_up", "walk_to", nullptr
};

static bool isLookVerb(const Common::String &verb) {
	return verb == "look_at";
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Sword1McpBridge::Sword1McpBridge(SwordEngine *vm)
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _ssePreScreen(0) {
}

Sword1McpBridge::~Sword1McpBridge() {
}

Sword1McpBridge *Sword1McpBridge::create(SwordEngine *vm) {
	Sword1McpBridge *bridge = new Sword1McpBridge(vm);
	bridge->init();
	return bridge;
}

// ---------------------------------------------------------------------------
// Engine state helpers
// ---------------------------------------------------------------------------

bool Sword1McpBridge::canAct() const {
	// Mouse::engine() bails unless bit 0 is set; fnLockMouse sets bit 1.
	uint32 status = Logic::_scriptVars[MOUSE_STATUS];
	return (status & 1) && !(status & 2);
}

bool Sword1McpBridge::panelShown() const {
	return _vm->_control && _vm->_control->isPanelShown();
}

void Sword1McpBridge::worldToScreen(int worldX, int worldY, int &screenX, int &screenY) const {
	// Inverse of the transform at the top of Mouse::engine().
	screenX = worldX - (int)Logic::_scriptVars[SCROLL_OFFSET_X] - 128;
	screenY = worldY - (int)Logic::_scriptVars[SCROLL_OFFSET_Y] - 128 + 40;
}

void Sword1McpBridge::collectScreenObjects(Common::Array<uint32> &ids, bool mouseableOnly) const {
	ids.clear();
	uint32 screen = Logic::_scriptVars[SCREEN];
	// The same sweep Logic::engine() performs each cycle.
	for (uint16 sect = 0; sect < TOTAL_SECTIONS; sect++) {
		if (!_vm->_objectMan->sectionAlive(sect))
			continue;
		uint32 numCpts = _vm->_objectMan->fetchNoObjects(sect);
		for (uint32 cnt = 0; cnt < numCpts; cnt++) {
			uint32 id = sect * ITM_PER_SEC + cnt;
			Object *cpt = _vm->_objectMan->fetchObject(id);
			if (!cpt || (uint32)cpt->o_screen != screen)
				continue;
			if (mouseableOnly && !(cpt->o_status & STAT_MOUSE))
				continue;
			ids.push_back(id);
		}
	}
}

void Sword1McpBridge::collectInventory(Common::Array<int> &pockets) const {
	pockets.clear();
	for (int i = 0; i < TOTAL_pockets; i++) {
		if (Logic::_scriptVars[POCKET_1 + i])
			pockets.push_back(i + 1);
	}
}

Common::String Sword1McpBridge::pocketDescription(int pocketNo) const {
	if (pocketNo < 1 || pocketNo > TOTAL_pockets)
		return Common::String();
	int32 textDesc = Menu::_objectDefs[pocketNo].textDesc;
	if (!textDesc)
		return Common::String();
	char *text = _vm->_objectMan->lockText((uint32)textDesc);
	Common::String out;
	if (text)
		out = safeUtf8(Common::String(text));
	_vm->_objectMan->unlockText((uint32)textDesc);
	return out;
}

// Classify a compact for the agent. o_type is the game's own broad category.
static const char *kindForType(int32 type) {
	switch (type) {
	case TYPE_FLOOR:   return "floor";
	case TYPE_MEGA:
	case TYPE_PLAYER:  return "character";
	case TYPE_SPRITE:
	case TYPE_NON_MEGA: return "object";
	case TYPE_MOUSE:   return "hotspot";
	default:           return "object";
	}
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

void Sword1McpBridge::onSpeech(int compactId, const Common::String &text, bool isVoiceOver) {
	// A voice-over has no speaking character on screen; fnISpeak itself renders
	// those centred at the bottom rather than over an actor, so report them as
	// narration instead of dialogue.
	if (isVoiceOver)
		onSystemLine(text);
	else
		onActorLine(compactId, text);
}

Common::String Sword1McpBridge::messageActorName(int actorId) const {
	const char *name = sword1CompactName((uint32)actorId);
	return name ? Common::String(name) : Common::String();
}

int Sword1McpBridge::currentRoomForMessages() const {
	return (int)Logic::_scriptVars[SCREEN];
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

void Sword1McpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not accepting input (cutscene, fade, control panel). "
	    "act/walk/answer are rejected until it turns true again."));
}

Common::JSONValue *Sword1McpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	uint32 screen = Logic::_scriptVars[SCREEN];
	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt((int)screen));
	if (const char *screenName = sword1ScreenName(screen))
		roomObj.setVal("name", mcpJsonString(screenName));
	out.setVal("room", new Common::JSONValue(roomObj));

	Object *player = _vm->_objectMan->fetchObject(PLAYER);
	if (player) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(player->o_xcoord));
		pos.setVal("y", mcpJsonInt(player->o_ycoord));
		pos.setVal("dir", mcpJsonInt(player->o_dir));
		out.setVal("position", new Common::JSONValue(pos));
	}

	out.setVal("can_act", mcpJsonBool(canAct() && !panelShown()));

	// Objects: everything the game would let the player click on this screen.
	Common::JSONArray objects;
	Common::Array<uint32> ids;
	collectScreenObjects(ids, true);
	for (uint i = 0; i < ids.size(); i++) {
		uint32 id = ids[i];
		if (id == PLAYER)
			continue;  // reported as `position`
		Object *cpt = _vm->_objectMan->fetchObject(id);
		if (cpt->o_type == TYPE_TEXT)
			continue;  // transient speech sprites
		Common::JSONObject o;
		o.setVal("id",   mcpJsonInt((int)id));
		o.setVal("name", mcpJsonString(sword1ObjectName(id)));
		o.setVal("kind", mcpJsonString(kindForType(cpt->o_type)));
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt((cpt->o_mouse_x1 + cpt->o_mouse_x2) / 2));
		pos.setVal("y", mcpJsonInt((cpt->o_mouse_y1 + cpt->o_mouse_y2) / 2));
		o.setVal("position", new Common::JSONValue(pos));
		Common::JSONArray box;
		box.push_back(mcpJsonInt(cpt->o_mouse_x1));
		box.push_back(mcpJsonInt(cpt->o_mouse_y1));
		box.push_back(mcpJsonInt(cpt->o_mouse_x2));
		box.push_back(mcpJsonInt(cpt->o_mouse_y2));
		o.setVal("box", new Common::JSONValue(box));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	// Inventory, with the game's own description for each item.
	Common::JSONArray inventory;
	Common::Array<int> pockets;
	collectInventory(pockets);
	for (uint i = 0; i < pockets.size(); i++) {
		Common::JSONObject item;
		const char *name = sword1PocketName(pockets[i]);
		item.setVal("name", mcpJsonString(name ? name
		    : Common::String::format("item_%d", pockets[i])));
		item.setVal("pocket", mcpJsonInt(pockets[i]));
		Common::String desc = pocketDescription(pockets[i]);
		if (!desc.empty())
			item.setVal("description", mcpJsonString(desc));
		inventory.push_back(new Common::JSONValue(item));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));

	Common::JSONArray verbs;
	for (int i = 0; kVerbs[i]; i++)
		verbs.push_back(mcpJsonString(kVerbs[i]));
	out.setVal("verbs", new Common::JSONValue(verbs));

	// Pending conversation topics, if the subject bar is open.
	Common::Array<uint32> subjects;
	if (_vm->_menu)
		_vm->_menu->mcpSubjectIds(subjects);
	if (!subjects.empty()) {
		Common::JSONArray choices;
		for (uint i = 0; i < subjects.size(); i++) {
			Common::JSONObject c;
			c.setVal("id", mcpJsonInt((int)i + 1));
			const char *name = sword1SubjectName(subjects[i]);
			c.setVal("label", mcpJsonString(name ? Common::String(name)
			    : Common::String::format("topic_%u", subjects[i] - BASE_SUBJECT)));
			choices.push_back(new Common::JSONValue(c));
		}
		Common::JSONObject question;
		question.setVal("choices", new Common::JSONValue(choices));
		out.setVal("question", new Common::JSONValue(question));
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

bool Sword1McpBridge::resolveTarget(const Common::String &name, uint32 &compactId,
                                    int &pocketNo, Common::String &errorOut) const {
	compactId = 0;
	pocketNo = 0;

	Common::String normalized = MCP::McpBridge::normalizeActionName(name);

	// An inventory item the player is carrying.
	int pocket = sword1PocketNumber(normalized);
	if (pocket && Logic::_scriptVars[POCKET_1 + pocket - 1]) {
		pocketNo = pocket;
		return true;
	}

	uint32 id = 0;
	if (sword1ResolveName(name, id)) {
		Object *cpt = _vm->_objectMan->fetchObject(id);
		if (!cpt) {
			errorOut = "unknown target '" + name + "'";
			return false;
		}
		compactId = id;
		return true;
	}

	// Not a known name: help the agent by naming what is actually reachable.
	if (pocket) {
		errorOut = "'" + name + "' is not in the inventory";
		return false;
	}
	Common::Array<uint32> ids;
	collectScreenObjects(ids, true);
	Common::String available;
	for (uint i = 0; i < ids.size() && i < 12; i++) {
		if (ids[i] == PLAYER)
			continue;
		if (!available.empty())
			available += ", ";
		available += sword1ObjectName(ids[i]);
	}
	errorOut = "unknown target '" + name + "'; objects on this screen: " + available;
	return false;
}

// ---------------------------------------------------------------------------
// Click injection
// ---------------------------------------------------------------------------

bool Sword1McpBridge::clickCompact(uint32 id, bool rightClick, Common::String &errorOut) {
	Object *cpt = _vm->_objectMan->fetchObject(id);
	if (!cpt) {
		errorOut = "target does not exist";
		return false;
	}
	if ((uint32)cpt->o_screen != Logic::_scriptVars[SCREEN]) {
		errorOut = "target is not on the current screen";
		return false;
	}
	if (!(cpt->o_status & STAT_MOUSE)) {
		errorOut = "target cannot be clicked right now";
		return false;
	}

	int wx = (cpt->o_mouse_x1 + cpt->o_mouse_x2) / 2;
	int wy = (cpt->o_mouse_y1 + cpt->o_mouse_y2) / 2;

	// Replay Mouse::engine()'s dispatch. Writing the script vars makes the
	// current cycle correct; warping the real cursor makes the *next* cycles
	// correct too, because Mouse::engine() recomputes MOUSE_X/MOUSE_Y and
	// SPECIAL_ITEM from the physical cursor at the end of every cycle. Without
	// the warp it would immediately reset SPECIAL_ITEM to whatever sits under
	// the never-moved cursor and fire this object's o_mouse_off script.
	Logic::_scriptVars[MOUSE_X] = wx;
	Logic::_scriptVars[MOUSE_Y] = wy;
	int sx = 0, sy = 0;
	worldToScreen(wx, wy, sx, sy);
	if (sx >= 0 && sy >= 0)
		g_system->warpMouse(sx, sy);

	if (Logic::_scriptVars[SPECIAL_ITEM] != id) {
		Logic::_scriptVars[SPECIAL_ITEM] = id;
		// The hover script is what normally populates CLICK_ID and the cursor.
		if (cpt->o_mouse_on)
			_vm->_logic->runMouseScript(cpt, cpt->o_mouse_on);
	}

	Logic::_scriptVars[MOUSE_BUTTON] = rightClick ? BS1R_BUTTON_DOWN : BS1L_BUTTON_DOWN;
	if (cpt->o_mouse_click)
		_vm->_logic->runMouseScript(cpt, cpt->o_mouse_click);
	return true;
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

bool Sword1McpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("verb") ||
	    !args.asObject()["verb"]->isString()) {
		errorOut = "act: 'verb' string is required";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	Common::String verb = MCP::McpBridge::normalizeActionName(a["verb"]->asString());

	bool known = false;
	for (int i = 0; kVerbs[i]; i++)
		if (verb == kVerbs[i]) { known = true; break; }
	if (!known) {
		errorOut = "act: unknown verb '" + verb + "'";
		return false;
	}

	auto targetName = [&](const char *key) -> Common::String {
		if (!a.contains(key))
			return Common::String();
		if (a[key]->isString())
			return a[key]->asString();
		if (a[key]->isIntegerNumber())
			return Common::String::format("%d", (int)a[key]->asIntegerNumber());
		return Common::String();
	};
	Common::String name1 = targetName("target1");
	Common::String name2 = targetName("target2");

	if (name1.empty()) {
		errorOut = "act: 'target1' is required";
		return false;
	}

	uint32 compact1 = 0, compact2 = 0;
	int pocket1 = 0, pocket2 = 0;
	if (!resolveTarget(name1, compact1, pocket1, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}
	if (!name2.empty() && !resolveTarget(name2, compact2, pocket2, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}

	// Look at an inventory item: exactly what Menu::checkMenuClick + Mouse::engine
	// do for a right click on the inventory bar.
	if (pocket1 && name2.empty() && isLookVerb(verb)) {
		Logic::_scriptVars[OBJECT_HELD]       = pocket1;
		Logic::_scriptVars[MENU_LOOKING]      = 1;
		Logic::_scriptVars[DEFAULT_ICON_TEXT] = Menu::_objectDefs[pocket1].textDesc;
		_vm->_logic->cfnPresetScript(nullptr, -1, PLAYER, SCR_menu_look, 0, 0, 0, 0);
		beginStream();
		return true;
	}

	// Use one inventory item on another: Mouse::engine()'s SECOND_ITEM branch.
	if (pocket1 && pocket2) {
		if (Logic::_scriptVars[GEORGE_DOING_REST_ANIM] == 1)
			Logic::_scriptVars[GEORGE_DOING_REST_ANIM] = 0;
		else if (Logic::_scriptVars[GEORGE_WALKING])
			Logic::_scriptVars[GEORGE_WALKING] = 2;
		Logic::_scriptVars[OBJECT_HELD] = pocket1;
		Logic::_scriptVars[SECOND_ITEM] = pocket2;
		_vm->_logic->runMouseScript(nullptr, Menu::_objectDefs[pocket2].useScript);
		beginStream();
		return true;
	}

	// Use an inventory item on a scene object: arm the held item, then click the
	// object — its o_mouse_click script branches on OBJECT_HELD.
	if (pocket1 && compact2) {
		Logic::_scriptVars[OBJECT_HELD] = pocket1;
		if (!clickCompact(compact2, false, errorOut)) {
			Logic::_scriptVars[OBJECT_HELD] = 0;
			errorOut = "act: " + errorOut;
			return false;
		}
		beginStream();
		return true;
	}

	if (!compact1) {
		errorOut = "act: '" + name1 + "' cannot be the target of " + verb;
		return false;
	}

	// A plain scene interaction. Nothing may be held, or the game reads it as
	// "use <held item> on X".
	if (!canAct() || panelShown()) {
		errorOut = "act: game is not accepting input right now";
		return false;
	}
	Logic::_scriptVars[OBJECT_HELD] = 0;
	if (!clickCompact(compact1, isLookVerb(verb), errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool Sword1McpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	if (!canAct() || panelShown()) {
		errorOut = "walk: game is not accepting input right now";
		return false;
	}
	int x = (int)args.asObject()["x"]->asIntegerNumber();
	int y = (int)args.asObject()["y"]->asIntegerNumber();

	// Find the floor compact under (x, y) and drive its click script. Calling
	// Logic::fnWalk directly would skip the floor script's own handshake, which
	// is what sets CHANGE_X/Y/DIR/STANCE/PLACE — bypassing it leaves the player's
	// o_place inconsistent with the floor it stands on and breaks later get-to
	// routing. Going through the click also means walk and act share one path.
	Common::Array<uint32> ids;
	collectScreenObjects(ids, true);
	uint32 floorId = 0;
	Common::String boxes;
	for (uint i = 0; i < ids.size(); i++) {
		Object *cpt = _vm->_objectMan->fetchObject(ids[i]);
		if (cpt->o_type != TYPE_FLOOR)
			continue;
		if (!boxes.empty())
			boxes += ", ";
		boxes += Common::String::format("[%d,%d,%d,%d]", cpt->o_mouse_x1, cpt->o_mouse_y1,
		                                cpt->o_mouse_x2, cpt->o_mouse_y2);
		if (x >= cpt->o_mouse_x1 && x <= cpt->o_mouse_x2 &&
		    y >= cpt->o_mouse_y1 && y <= cpt->o_mouse_y2) {
			floorId = ids[i];
			break;
		}
	}
	if (!floorId) {
		errorOut = Common::String::format(
		    "walk: (%d, %d) is not on a walkable floor; floor areas on this screen: ", x, y) + boxes;
		return false;
	}

	Object *floorCpt = _vm->_objectMan->fetchObject(floorId);
	Logic::_scriptVars[OBJECT_HELD]  = 0;
	Logic::_scriptVars[MOUSE_X]      = x;
	Logic::_scriptVars[MOUSE_Y]      = y;
	int sx = 0, sy = 0;
	worldToScreen(x, y, sx, sy);
	if (sx >= 0 && sy >= 0)
		g_system->warpMouse(sx, sy);
	if (Logic::_scriptVars[SPECIAL_ITEM] != floorId) {
		Logic::_scriptVars[SPECIAL_ITEM] = floorId;
		if (floorCpt->o_mouse_on)
			_vm->_logic->runMouseScript(floorCpt, floorCpt->o_mouse_on);
	}
	Logic::_scriptVars[MOUSE_BUTTON] = BS1L_BUTTON_DOWN;
	if (floorCpt->o_mouse_click)
		_vm->_logic->runMouseScript(floorCpt, floorCpt->o_mouse_click);

	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

bool Sword1McpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "answer: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("id") ||
	    !args.asObject()["id"]->isIntegerNumber()) {
		errorOut = "answer: integer 'id' is required";
		return false;
	}
	int id = (int)args.asObject()["id"]->asIntegerNumber();
	if (!hasPendingQuestion()) {
		errorOut = "answer: no dialog question is pending";
		return false;
	}
	if (id < 1 || (uint32)id > Logic::_scriptVars[IN_SUBJECT]) {
		errorOut = Common::String::format("answer: 'id' must be between 1 and %u",
		                                  Logic::_scriptVars[IN_SUBJECT]);
		return false;
	}
	if (!_vm->_menu || !_vm->_menu->mcpChooseSubject((uint8)id)) {
		errorOut = "answer: the subject bar is not open";
		return false;
	}
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool Sword1McpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}

	if (SwordEngine::_systemVars.textRunning || SwordEngine::_systemVars.speechRunning) {
		// Exactly what Logic::speechDriver does when the player clicks during a
		// line. Setting the flag directly avoids having to smuggle a mouse event
		// past the one-cycle delay in Mouse::engine(), and it correctly does
		// nothing while _speechClickDelay is still counting down.
		SwordEngine::_systemVars.speechFinished = true;
	} else {
		// MoviePlayer::playVideo() exits on EVENT_LBUTTONUP or kActionEscape. Use
		// the button: checkKeys() maps Escape to the control panel, which would
		// pop the panel open if the movie had already finished.
		Common::Event event;
		event.type = Common::EVENT_LBUTTONUP;
		g_system->getEventManager()->pushEvent(event);
	}

	if (!isStreaming())
		beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool dispatch / gating
// ---------------------------------------------------------------------------

Common::JSONValue *Sword1McpBridge::callTool(const Common::String &name,
                                             const Common::JSONValue &args,
                                             Common::String &errorOut) {
	// The bridge is constructed before SwordEngine::init() builds the subsystems
	// (see the comment at its construction site), so a call arriving that early
	// has nothing to read.
	if (!_vm->_objectMan || !_vm->_logic) {
		errorOut = name + ": the engine is still starting up";
		return nullptr;
	}
	// The server is also pumped from inside SwordEngine::pollInput() and the
	// cutscene player, so a tool call can land while Control owns the screen or a
	// movie owns the loop. Reads stay available; anything that mutates game state
	// is refused rather than corrupting a half-finished engine state.
	if (panelShown() && name != "state" && name != "debug" && name != "screenshot") {
		errorOut = name + ": the control panel is open";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// Input injection (debug tools)
// ---------------------------------------------------------------------------

void Sword1McpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
}

void Sword1McpBridge::injectMouseMove(int x, int y) {
	// Debug tools speak screen coordinates, which is what the cursor uses.
	g_system->warpMouse(x, y);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

void Sword1McpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	injectMouseMove(x, y);
	bool right = (button == "right");
	Common::Event down, up;
	down.type = right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
	up.type   = right ? Common::EVENT_RBUTTONUP   : Common::EVENT_LBUTTONUP;
	down.mouse = up.mouse = Common::Point(x, y);
	int clicks = isDouble ? 2 : 1;
	for (int i = 0; i < clicks; i++) {
		g_system->getEventManager()->pushEvent(down);
		g_system->getEventManager()->pushEvent(up);
	}
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void Sword1McpBridge::snapshotPreAction() {
	_sseActionStarted = false;
	_ssePreScreen = (int)Logic::_scriptVars[SCREEN];
	_ssePreRoom = _ssePreScreen;
	collectInventory(_ssePreInventory);
	_ssePreInventoryNames.clear();
	for (uint i = 0; i < _ssePreInventory.size(); i++) {
		const char *name = sword1PocketName(_ssePreInventory[i]);
		_ssePreInventoryNames.push_back(name ? Common::String(name)
		    : Common::String::format("item_%d", _ssePreInventory[i]));
	}
	Object *player = _vm->_objectMan->fetchObject(PLAYER);
	_ssePrePosX = player ? player->o_xcoord : 0;
	_ssePrePosY = player ? player->o_ycoord : 0;
}

void Sword1McpBridge::pumpStreamTrack() {
	// Latch "the action produced something" the moment George starts walking, a
	// line is said, or the screen changes. isActionDone() stays false until then,
	// so a stream cannot settle-close in the ~1-2 frames before the clicked
	// script has run. Each of these also bumps the event frame so the timeout is
	// measured from real activity.
	if (Logic::_scriptVars[GEORGE_WALKING] || SwordEngine::_systemVars.textRunning ||
	    SwordEngine::_systemVars.speechRunning ||
	    (int)Logic::_scriptVars[SCREEN] != _ssePreScreen) {
		if (!_sseActionStarted)
			_sseActionStarted = true;
		_sseLastEventFrame = _frameCounter;
	}
}

bool Sword1McpBridge::streamRoomChanged() const {
	return (int)Logic::_scriptVars[SCREEN] != _ssePreScreen;
}

bool Sword1McpBridge::isStreamStuck() const {
	// Input locked with nothing being said and the player standing still. A
	// cutscene keeps textRunning/speechRunning or GEORGE_WALKING alive, so this
	// only fires when the action genuinely produced nothing.
	return !canAct() && !SwordEngine::_systemVars.textRunning &&
	       !SwordEngine::_systemVars.speechRunning && !Logic::_scriptVars[GEORGE_WALKING];
}

bool Sword1McpBridge::hasPendingQuestion() const {
	if (!_vm->_menu)
		return false;
	Common::Array<uint32> subjects;
	_vm->_menu->mcpSubjectIds(subjects);
	return !subjects.empty();
}

bool Sword1McpBridge::isActionDone() const {
	if (panelShown())
		return false;
	// Still speaking or showing a subtitle.
	if (SwordEngine::_systemVars.textRunning || SwordEngine::_systemVars.speechRunning)
		return false;
	// Still walking.
	if (Logic::_scriptVars[GEORGE_WALKING])
		return false;
	// Input not handed back yet (cutscene, fade, scripted sequence).
	if (!canAct())
		return false;
	// Give a clicked script a short window to produce its first visible effect
	// (start a walk, say a line, change screen) before concluding the action was
	// a no-op — the effect usually lands a frame or two after the click.
	if (!_sseActionStarted && _frameCounter - _sseStartFrame < 8)
		return false;
	// Note: George's o_logic is not a completion signal — his mega runs its
	// standing script (LOGIC_script) continuously even when idle, so gating on
	// LOGIC_idle would never let a stream close. GEORGE_WALKING plus the speech
	// flags plus can_act are the reliable signals.
	return true;
}

Common::JSONObject Sword1McpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	Common::Array<int> nowInventory;
	collectInventory(nowInventory);
	auto contains = [](const Common::Array<int> &arr, int v) -> bool {
		for (uint i = 0; i < arr.size(); i++)
			if (arr[i] == v) return true;
		return false;
	};

	Common::JSONArray added;
	for (uint i = 0; i < nowInventory.size(); i++) {
		if (!contains(_ssePreInventory, nowInventory[i])) {
			const char *name = sword1PocketName(nowInventory[i]);
			added.push_back(mcpJsonString(name ? Common::String(name)
			    : Common::String::format("item_%d", nowInventory[i])));
		}
	}
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	Common::JSONArray removed;
	for (uint i = 0; i < _ssePreInventory.size(); i++) {
		if (!contains(nowInventory, _ssePreInventory[i]))
			removed.push_back(mcpJsonString(_ssePreInventoryNames[i]));
	}
	if (!removed.empty())
		changes.setVal("inventory_removed", new Common::JSONValue(removed));

	int screen = (int)Logic::_scriptVars[SCREEN];
	if (screen != _ssePreScreen)
		changes.setVal("room_changed", mcpJsonInt(screen));

	Object *player = _vm->_objectMan->fetchObject(PLAYER);
	if (player && (player->o_xcoord != _ssePrePosX || player->o_ycoord != _ssePrePosY)) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(player->o_xcoord));
		pos.setVal("y", mcpJsonInt(player->o_ycoord));
		changes.setVal("position", new Common::JSONValue(pos));
	}

	if (!_sseMessages.empty()) {
		Common::JSONArray messages;
		for (uint i = 0; i < _sseMessages.size(); i++) {
			Common::JSONObject m;
			Common::String text = MCP::mcpCleanGameText(safeUtf8(_sseMessages[i].text));
			if (text.empty())
				continue;
			m.setVal("text", mcpJsonString(text));
			if (_sseMessages[i].actorId >= 0) {
				Common::String actor = messageActorName(_sseMessages[i].actorId);
				if (!actor.empty())
					m.setVal("actor", mcpJsonString(actor));
			}
			m.setVal("type", mcpJsonString(_sseMessages[i].type));
			messages.push_back(new Common::JSONValue(m));
		}
		if (!messages.empty())
			changes.setVal("messages", new Common::JSONValue(messages));
	}

	// A conversation that opened during the action.
	Common::Array<uint32> subjects;
	if (_vm->_menu)
		_vm->_menu->mcpSubjectIds(subjects);
	if (!subjects.empty()) {
		Common::JSONArray choices;
		for (uint i = 0; i < subjects.size(); i++) {
			Common::JSONObject c;
			c.setVal("id", mcpJsonInt((int)i + 1));
			const char *name = sword1SubjectName(subjects[i]);
			c.setVal("label", mcpJsonString(name ? Common::String(name)
			    : Common::String::format("topic_%u", subjects[i] - BASE_SUBJECT)));
			choices.push_back(new Common::JSONValue(c));
		}
		Common::JSONObject question;
		question.setVal("choices", new Common::JSONValue(choices));
		changes.setVal("question", new Common::JSONValue(question));
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Tool: debug
// ---------------------------------------------------------------------------

Common::String Sword1McpBridge::stateToolDescription() const {
	return "Returns the current game state: the screen, where the player character "
	       "stands, everything on screen that can be clicked, what is carried, the "
	       "verbs, the lines said since the last read (cleared by reading them) and "
	       "any conversation choice waiting to be picked. The player character "
	       "itself is reported as position, not as an object. Each object carries a "
	       "'kind' and the point a click lands on; objects and inventory items are "
	       "targeted in act() by name or by id. This is a two-verb game at heart: "
	       "'look_at' examines and 'interact' does whatever the thing invites, with "
	       "'use X on Y' for combining. Nothing is accepted while can_act is false.";
}

Common::String Sword1McpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics. Sections are selected by "
	       "flag: 'vars' (a slice of the script variables, with 'from'/'to'), "
	       "'compacts' (every live thing on the current screen with its id, type, "
	       "status, click box and script ids), 'pockets' (every inventory slot) "
	       "and 'system' (the game's own read-out). Defaults to 'system'.";
}

Common::JSONValue *Sword1McpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("vars",     mcpProp("boolean", "Include script variables."));
	props.setVal("from",     mcpProp("integer", "First script variable index (default 0)."));
	props.setVal("to",       mcpProp("integer", "Last script variable index, inclusive (default 63)."));
	props.setVal("compacts", mcpProp("boolean", "Include every live compact on the current screen."));
	props.setVal("pockets",  mcpProp("boolean", "Include all inventory slots."));
	props.setVal("system",   mcpProp("boolean", "Include the engine state summary (default true)."));
	return mcpObjectSchema(props);
}

// The script variables worth naming in a dump. The full enum is 1179 entries
// (sworddefs.h); the rest are reported by index, which is enough to read them.
struct NamedVar { int index; const char *name; };
static const NamedVar kNamedVars[] = {
	{ RETURN_VALUE,       "RETURN_VALUE" },
	{ DEFAULT_ICON_TEXT,  "DEFAULT_ICON_TEXT" },
	{ MENU_LOOKING,       "MENU_LOOKING" },
	{ TOP_MENU_DISABLED,  "TOP_MENU_DISABLED" },
	{ GEORGE_WALKING,     "GEORGE_WALKING" },
	{ NEW_SCREEN,         "NEW_SCREEN" },
	{ CUR_ID,             "CUR_ID" },
	{ MOUSE_STATUS,       "MOUSE_STATUS" },
	{ MOUSE_X,            "MOUSE_X" },
	{ MOUSE_Y,            "MOUSE_Y" },
	{ SPECIAL_ITEM,       "SPECIAL_ITEM" },
	{ CLICK_ID,           "CLICK_ID" },
	{ MOUSE_BUTTON,       "MOUSE_BUTTON" },
	{ CHANGE_X,           "CHANGE_X" },
	{ CHANGE_Y,           "CHANGE_Y" },
	{ CHANGE_PLACE,       "CHANGE_PLACE" },
	{ CHANGE_DIR,         "CHANGE_DIR" },
	{ CHANGE_STANCE,      "CHANGE_STANCE" },
	{ SCROLL_OFFSET_X,    "SCROLL_OFFSET_X" },
	{ SCROLL_OFFSET_Y,    "SCROLL_OFFSET_Y" },
	{ SECOND_ITEM,        "SECOND_ITEM" },
	{ SUBJECT_CHOSEN,     "SUBJECT_CHOSEN" },
	{ IN_SUBJECT,         "IN_SUBJECT" },
	{ OBJECT_HELD,        "OBJECT_HELD" },
	{ SCREEN,             "SCREEN" },
	{ TALK_FLAG,          "TALK_FLAG" },
	{ PARIS_FLAG,         "PARIS_FLAG" }
};

static const char *namedVar(int index) {
	for (int i = 0; i < ARRAYSIZE(kNamedVars); i++)
		if (kNamedVars[i].index == index)
			return kNamedVars[i].name;
	return nullptr;
}

Common::JSONValue *Sword1McpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
	const bool hasArgs = args.isObject();
	auto flag = [&](const char *key, bool dflt) -> bool {
		if (!hasArgs || !args.asObject().contains(key) || !args.asObject()[key]->isBool())
			return dflt;
		return args.asObject()[key]->asBool();
	};
	auto number = [&](const char *key, int dflt) -> int {
		if (!hasArgs || !args.asObject().contains(key) || !args.asObject()[key]->isIntegerNumber())
			return dflt;
		return (int)args.asObject()[key]->asIntegerNumber();
	};

	Common::JSONObject out;

	if (flag("system", true)) {
		Common::JSONObject sys;
		sys.setVal("screen",          mcpJsonInt((int)Logic::_scriptVars[SCREEN]));
		sys.setVal("new_screen",      mcpJsonInt((int)Logic::_scriptVars[NEW_SCREEN]));
		sys.setVal("mouse_status",    mcpJsonInt((int)Logic::_scriptVars[MOUSE_STATUS]));
		sys.setVal("special_item",    mcpJsonInt((int)Logic::_scriptVars[SPECIAL_ITEM]));
		sys.setVal("click_id",        mcpJsonInt((int)Logic::_scriptVars[CLICK_ID]));
		sys.setVal("object_held",     mcpJsonInt((int)Logic::_scriptVars[OBJECT_HELD]));
		sys.setVal("second_item",     mcpJsonInt((int)Logic::_scriptVars[SECOND_ITEM]));
		sys.setVal("in_subject",      mcpJsonInt((int)Logic::_scriptVars[IN_SUBJECT]));
		sys.setVal("george_walking",  mcpJsonInt((int)Logic::_scriptVars[GEORGE_WALKING]));
		sys.setVal("scroll_x",        mcpJsonInt((int)Logic::_scriptVars[SCROLL_OFFSET_X]));
		sys.setVal("scroll_y",        mcpJsonInt((int)Logic::_scriptVars[SCROLL_OFFSET_Y]));
		sys.setVal("text_running",    mcpJsonBool(SwordEngine::_systemVars.textRunning));
		sys.setVal("speech_running",  mcpJsonBool(SwordEngine::_systemVars.speechRunning != 0));
		sys.setVal("show_text",       mcpJsonBool(SwordEngine::_systemVars.showText));
		sys.setVal("text_number",     mcpJsonInt(SwordEngine::_systemVars.textNumber));
		sys.setVal("game_cycle",      mcpJsonInt((int)SwordEngine::_systemVars.gameCycle));
		sys.setVal("control_panel",   mcpJsonBool(panelShown()));
		sys.setVal("can_act",         mcpJsonBool(canAct()));
		Object *player = _vm->_objectMan->fetchObject(PLAYER);
		if (player) {
			sys.setVal("player_x",     mcpJsonInt(player->o_xcoord));
			sys.setVal("player_y",     mcpJsonInt(player->o_ycoord));
			sys.setVal("player_dir",   mcpJsonInt(player->o_dir));
			sys.setVal("player_logic", mcpJsonInt(player->o_logic));
			sys.setVal("player_place", mcpJsonInt(player->o_place));
		}
		out.setVal("system", new Common::JSONValue(sys));
	}

	if (flag("vars", false)) {
		int from = MAX(0, number("from", 0));
		int to   = MIN(NUM_SCRIPT_VARS - 1, number("to", 63));
		Common::JSONArray vars;
		for (int i = from; i <= to; i++) {
			Common::JSONObject v;
			v.setVal("index", mcpJsonInt(i));
			if (const char *name = namedVar(i))
				v.setVal("name", mcpJsonString(name));
			v.setVal("value", mcpJsonInt((int)Logic::_scriptVars[i]));
			vars.push_back(new Common::JSONValue(v));
		}
		out.setVal("vars", new Common::JSONValue(vars));
	}

	if (flag("compacts", false)) {
		Common::JSONArray comps;
		Common::Array<uint32> ids;
		collectScreenObjects(ids, false);
		for (uint i = 0; i < ids.size(); i++) {
			uint32 id = ids[i];
			Object *cpt = _vm->_objectMan->fetchObject(id);
			Common::JSONObject c;
			c.setVal("id",       mcpJsonInt((int)id));
			c.setVal("id_hex",   mcpJsonString(Common::String::format("0x%X", id)));
			c.setVal("section",  mcpJsonInt((int)(id / ITM_PER_SEC)));
			c.setVal("index",    mcpJsonInt((int)(id & ITM_ID)));
			c.setVal("name",     mcpJsonString(sword1ObjectName(id)));
			c.setVal("type",     mcpJsonInt(cpt->o_type));
			c.setVal("kind",     mcpJsonString(kindForType(cpt->o_type)));
			c.setVal("status",   mcpJsonInt(cpt->o_status));
			c.setVal("mouseable", mcpJsonBool((cpt->o_status & STAT_MOUSE) != 0));
			c.setVal("logic",    mcpJsonInt(cpt->o_logic));
			c.setVal("screen",   mcpJsonInt(cpt->o_screen));
			c.setVal("x",        mcpJsonInt(cpt->o_xcoord));
			c.setVal("y",        mcpJsonInt(cpt->o_ycoord));
			Common::JSONArray box;
			box.push_back(mcpJsonInt(cpt->o_mouse_x1));
			box.push_back(mcpJsonInt(cpt->o_mouse_y1));
			box.push_back(mcpJsonInt(cpt->o_mouse_x2));
			box.push_back(mcpJsonInt(cpt->o_mouse_y2));
			c.setVal("box",      new Common::JSONValue(box));
			c.setVal("priority",       mcpJsonInt(cpt->o_priority));
			c.setVal("mouse_on",       mcpJsonInt(cpt->o_mouse_on));
			c.setVal("mouse_off",      mcpJsonInt(cpt->o_mouse_off));
			c.setVal("mouse_click",    mcpJsonInt(cpt->o_mouse_click));
			c.setVal("interact",       mcpJsonInt(cpt->o_interact));
			c.setVal("get_to_script",  mcpJsonInt(cpt->o_get_to_script));
			c.setVal("dir",            mcpJsonInt(cpt->o_dir));
			comps.push_back(new Common::JSONValue(c));
		}
		out.setVal("compacts", new Common::JSONValue(comps));
	}

	if (flag("pockets", false)) {
		Common::JSONArray pocketArr;
		for (int i = 1; i <= TOTAL_pockets; i++) {
			Common::JSONObject p;
			p.setVal("pocket", mcpJsonInt(i));
			p.setVal("value",  mcpJsonInt((int)Logic::_scriptVars[POCKET_1 + i - 1]));
			const char *name = sword1PocketName(i);
			p.setVal("name",   mcpJsonString(name ? name : ""));
			p.setVal("text_desc", mcpJsonInt(Menu::_objectDefs[i].textDesc));
			Common::String desc = pocketDescription(i);
			if (!desc.empty())
				p.setVal("description", mcpJsonString(desc));
			pocketArr.push_back(new Common::JSONValue(p));
		}
		out.setVal("pockets", new Common::JSONValue(pocketArr));
	}

	return new Common::JSONValue(out);
}

} // End of namespace Sword1
