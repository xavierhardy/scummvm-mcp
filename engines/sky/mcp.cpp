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

#include "sky/mcp.h"
#include "sky/mcp_names.h"

#include "sky/compact.h"
#include "sky/logic.h"
#include "sky/mouse.h"
#include "sky/sky.h"
#include "sky/skydefs.h"
#include "sky/sound.h"
#include "sky/struc.h"
#include "sky/text.h"

#include "common/debug.h"
#include "common/events.h"
#include "common/system.h"

namespace Sky {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// The verbs the bridge accepts. BASS has no verb bar: one button examines a
// hotspot, the other walks there and runs its interaction script, so every
// verb but look_at maps onto the same action-click path. They are still listed
// separately because an agent reasons in verbs, and because state.verbs is
// what tells it which words the server will accept.
static const char *const kVerbs[] = {
	"look_at", "interact", "use", "talk_to", "pick_up", "walk_to", nullptr
};

static bool isLookVerb(const Common::String &verb) {
	return verb == "look_at";
}

// Sky::Mouse button codes (see SkyEngine::delay()): the left button reports 2,
// the right button 1 — and in BASS the LEFT button examines while the RIGHT
// button walks/interacts (the scripts read BUTTON==2 as look, BUTTON==1 as
// action, same split the touch UI uses).
enum { kLookButton = 2, kActionButton = 1 };

// The chooser owns text compacts FIRST_TEXT_COMPACT .. +10 (fnTextKill's own
// range).
enum { kNumTextCompacts = 10 };

// The script variable the game-menu inventory list starts at (what the game
// scripts pass to fnStartMenu for the main menu; observed live). fnStartMenu
// re-latches it, which also covers the LINC-space menus.
enum { kGameMenuVarsBase = 69 };

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SkyMcpBridge::SkyMcpBridge(SkyEngine *vm)
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _logic(nullptr),
	  _mouse(nullptr),
	  _text(nullptr),
	  _skyCompact(nullptr),
	  _control(nullptr),
	  _sound(nullptr),
	  _invVarsBase(kGameMenuVarsBase),
	  _ssePreScreen(0),
	  _sseActionStarted(false) {
}

SkyMcpBridge::~SkyMcpBridge() {
}

SkyMcpBridge *SkyMcpBridge::create(SkyEngine *vm) {
	SkyMcpBridge *bridge = new SkyMcpBridge(vm);
	bridge->init();
	return bridge;
}

void SkyMcpBridge::attach(Logic *logic, Mouse *mouse, Text *text, SkyCompact *skyCompact,
                          Control *control, Sound *sound) {
	_logic = logic;
	_mouse = mouse;
	_text = text;
	_skyCompact = skyCompact;
	_control = control;
	_sound = sound;
}

// ---------------------------------------------------------------------------
// Engine state helpers
// ---------------------------------------------------------------------------

bool SkyMcpBridge::canAct() const {
	// Mouse::mouseEngine()'s own gate: pointer engine needs bit 1, buttons
	// need bit 2, and MOUSE_STOP freezes everything.
	if (Logic::_scriptVariables[MOUSE_STOP])
		return false;
	uint32 status = Logic::_scriptVariables[MOUSE_STATUS];
	return (status & 2) && (status & 4);
}

Common::String SkyMcpBridge::textString(uint32 textNum) const {
	if (!textNum || !_text)
		return Common::String();
	return safeUtf8(_text->getTextString(textNum));
}

Common::String SkyMcpBridge::compactDisplayName(uint16 id) const {
	Compact *cpt = _skyCompact->fetchCpt(id);
	if (cpt && cpt->cursorText) {
		Common::String name = normalizeActionName(textString(cpt->cursorText));
		if (!name.empty())
			return name;
	}
	char cptName[64];
	cptName[0] = '\0';
	_skyCompact->fetchCptInfo(id, nullptr, nullptr, cptName, sizeof(cptName));
	Common::String name = normalizeActionName(Common::String(cptName));
	// The compact store names unknown entries "unknown".
	if (!name.empty() && !name.hasPrefix("unknown"))
		return name;
	return Common::String::format("object_%u", id);
}

void SkyMcpBridge::mouseBoxCenter(const Compact *cpt, int &x, int &y) const {
	x = cpt->xcood + ((int16)cpt->mouseRelX) + cpt->mouseSizeX / 2;
	y = cpt->ycood + ((int16)cpt->mouseRelY) + cpt->mouseSizeY / 2;
}

void SkyMcpBridge::collectScreenObjects(Common::Array<ScreenObject> &out) const {
	out.clear();
	uint32 screen = Logic::_scriptVariables[SCREEN];

	Common::Array<uint16> inventory;
	collectInventory(inventory);

	// The same chained-list sweep Mouse::pointerEngine() performs.
	uint32 currentListNum = Logic::_scriptVariables[MOUSE_LIST_NO];
	if (!currentListNum)
		return;
	uint16 *currentList;
	do {
		currentList = (uint16 *)_skyCompact->fetchCpt((uint16)currentListNum);
		while ((*currentList != 0) && (*currentList != 0xFFFF)) {
			uint16 itemNum = *currentList;
			Compact *itemData = _skyCompact->fetchCpt(itemNum);
			currentList++;
			if (!itemData || itemData->screen != screen || !(itemData->status & ST_MOUSE))
				continue;
			if (itemNum == ID_FOSTER)
				continue; // the player is reported as `position`
			// Chooser texts are reported through `question`, not as objects.
			if (itemNum >= FIRST_TEXT_COMPACT && itemNum < FIRST_TEXT_COMPACT + kNumTextCompacts)
				continue;
			// The icon-bar strip and its scroll arrows are UI, not scene.
			if (itemNum == CPT_MENU_BAR || itemNum == 47 || itemNum == 48)
				continue;
			// Icon-bar entries are reported through `inventory`.
			bool isInv = false;
			for (uint i = 0; i < inventory.size(); i++)
				if (inventory[i] == itemNum) { isInv = true; break; }
			if (isInv)
				continue;

			ScreenObject obj;
			obj.id = itemNum;
			obj.isFloor = (itemData->mouseOn == 0);
			obj.isCharacter = skyIsCharacter(itemNum);
			obj.name = obj.isFloor ? Common::String("floor") : compactDisplayName(itemNum);
			obj.x1 = itemData->xcood + (int16)itemData->mouseRelX;
			obj.y1 = itemData->ycood + (int16)itemData->mouseRelY;
			obj.x2 = obj.x1 + itemData->mouseSizeX;
			obj.y2 = obj.y1 + itemData->mouseSizeY;
			out.push_back(obj);
		}
		if (*currentList == 0xFFFF)
			currentListNum = currentList[1];
	} while (*currentList != 0);

	// De-duplicate names so they resolve unambiguously: door, door_2, door_3 …
	// Count occurrences of each *base* name (renamed entries no longer match).
	Common::Array<Common::String> baseNames;
	for (uint i = 0; i < out.size(); i++)
		baseNames.push_back(out[i].name);
	for (uint i = 0; i < out.size(); i++) {
		uint n = 1;
		for (uint j = 0; j < i; j++)
			if (baseNames[j] == baseNames[i])
				n++;
		if (n > 1)
			out[i].name = Common::String::format("%s_%u", baseNames[i].c_str(), n);
	}
}

void SkyMcpBridge::collectInventory(Common::Array<uint16> &items) const {
	items.clear();
	if (!_invVarsBase)
		return;
	// The same range fnStartMenu copies into the icon bar (30 entries).
	for (uint32 i = _invVarsBase; i < _invVarsBase + 30 && i < NUM_SKY_SCRIPTVARS; i++) {
		if (Logic::_scriptVariables[i])
			items.push_back((uint16)Logic::_scriptVariables[i]);
	}
}

void SkyMcpBridge::collectChoices(Common::Array<uint16> &textCompacts) const {
	textCompacts.clear();
	for (uint16 id = FIRST_TEXT_COMPACT; id < FIRST_TEXT_COMPACT + kNumTextCompacts; id++) {
		Compact *cpt = _skyCompact->fetchCpt(id);
		if (cpt && (cpt->status & ST_MOUSE))
			textCompacts.push_back(id);
	}
	// Display order is top to bottom.
	for (uint i = 1; i < textCompacts.size(); i++)
		for (uint j = i; j > 0; j--) {
			Compact *a = _skyCompact->fetchCpt(textCompacts[j - 1]);
			Compact *b = _skyCompact->fetchCpt(textCompacts[j]);
			if (a->ycood > b->ycood)
				SWAP(textCompacts[j - 1], textCompacts[j]);
		}
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

void SkyMcpBridge::onSpeech(uint16 compactId, uint32 textNum) {
	if (!isEnabled())
		return; // don't decode text for nothing when mcp=false
	Common::String line = textString(textNum);
	if (line.empty())
		return;
	onActorLine((int)compactId, line);
}

void SkyMcpBridge::onStartMenu(uint32 firstVar) {
	if (_invVarsBase != firstVar) {
		debug(1, "mcp: inventory variables start at %u", firstVar);
		_invVarsBase = firstVar;
	}
}

Common::String SkyMcpBridge::messageActorName(int actorId) const {
	if (actorId == ID_FOSTER)
		return "foster";
	// Use the authored compact name ("joey", "full_so"), not cursorText: the
	// scripts repoint a talker's cursorText at arbitrary strings
	// (fnChangeName), so it is not a stable identity.
	char cptName[64];
	cptName[0] = '\0';
	_skyCompact->fetchCptInfo((uint16)actorId, nullptr, nullptr, cptName, sizeof(cptName));
	Common::String name = normalizeActionName(Common::String(cptName));
	if (!name.empty() && !name.hasPrefix("unknown"))
		return name;
	return Common::String::format("actor_%d", actorId);
}

int SkyMcpBridge::currentRoomForMessages() const {
	return (int)Logic::_scriptVariables[SCREEN];
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

void SkyMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not accepting input (cutscene, scripted sequence). "
	    "act/walk/answer are rejected until it turns true again."));
}

Common::JSONValue *SkyMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	uint32 screen = Logic::_scriptVariables[SCREEN];
	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt((int)screen));
	if (const char *screenName = skyScreenName(screen))
		roomObj.setVal("name", mcpJsonString(screenName));
	out.setVal("room", new Common::JSONValue(roomObj));

	Compact *foster = _skyCompact->fetchCpt(ID_FOSTER);
	if (foster) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(foster->xcood));
		pos.setVal("y", mcpJsonInt(foster->ycood));
		pos.setVal("dir", mcpJsonInt(foster->dir));
		out.setVal("position", new Common::JSONValue(pos));
	}

	out.setVal("can_act", mcpJsonBool(canAct()));

	Common::JSONArray objects;
	Common::Array<ScreenObject> screenObjects;
	collectScreenObjects(screenObjects);
	for (uint i = 0; i < screenObjects.size(); i++) {
		const ScreenObject &so = screenObjects[i];
		Common::JSONObject o;
		o.setVal("id",   mcpJsonInt(so.id));
		o.setVal("name", mcpJsonString(so.name));
		o.setVal("kind", mcpJsonString(so.isFloor ? "floor"
		    : (so.isCharacter ? "character" : "hotspot")));
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt((so.x1 + so.x2) / 2));
		pos.setVal("y", mcpJsonInt((so.y1 + so.y2) / 2));
		o.setVal("position", new Common::JSONValue(pos));
		Common::JSONArray box;
		box.push_back(mcpJsonInt(so.x1));
		box.push_back(mcpJsonInt(so.y1));
		box.push_back(mcpJsonInt(so.x2));
		box.push_back(mcpJsonInt(so.y2));
		o.setVal("box", new Common::JSONValue(box));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::JSONArray inventory;
	Common::Array<uint16> items;
	collectInventory(items);
	for (uint i = 0; i < items.size(); i++) {
		Common::JSONObject item;
		item.setVal("name", mcpJsonString(compactDisplayName(items[i])));
		item.setVal("id", mcpJsonInt(items[i]));
		inventory.push_back(new Common::JSONValue(item));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));

	Common::JSONArray verbs;
	for (int i = 0; kVerbs[i]; i++)
		verbs.push_back(mcpJsonString(kVerbs[i]));
	out.setVal("verbs", new Common::JSONValue(verbs));

	Common::Array<uint16> choices;
	collectChoices(choices);
	if (!choices.empty()) {
		Common::JSONArray choiceArr;
		for (uint i = 0; i < choices.size(); i++) {
			Compact *cpt = _skyCompact->fetchCpt(choices[i]);
			Common::JSONObject c;
			c.setVal("id", mcpJsonInt((int)i + 1));
			// fnChooser stores the choice's text number in getToFlag.
			c.setVal("label", mcpJsonString(MCP::mcpCleanGameText(textString(cpt->getToFlag))));
			choiceArr.push_back(new Common::JSONValue(c));
		}
		Common::JSONObject question;
		question.setVal("choices", new Common::JSONValue(choiceArr));
		out.setVal("question", new Common::JSONValue(question));
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Target resolution
// ---------------------------------------------------------------------------

bool SkyMcpBridge::resolveTarget(const Common::String &name, uint16 &screenId, uint16 &itemId,
                                 Common::String &errorOut) const {
	screenId = 0;
	itemId = 0;
	Common::String normalized = normalizeActionName(name);

	// A numeric id is accepted as-is when it names something reachable.
	int numeric = (int)strtol(normalized.c_str(), nullptr, 10);

	Common::Array<ScreenObject> screenObjects;
	collectScreenObjects(screenObjects);
	for (uint i = 0; i < screenObjects.size(); i++) {
		if (screenObjects[i].name == normalized ||
		    (numeric > 0 && screenObjects[i].id == (uint16)numeric)) {
			screenId = screenObjects[i].id;
			return true;
		}
	}

	Common::Array<uint16> items;
	collectInventory(items);
	for (uint i = 0; i < items.size(); i++) {
		if (compactDisplayName(items[i]) == normalized ||
		    (numeric > 0 && items[i] == (uint16)numeric)) {
			itemId = items[i];
			return true;
		}
	}

	// Not found: help the agent by naming what is reachable.
	Common::String available;
	for (uint i = 0; i < screenObjects.size() && i < 14; i++) {
		if (screenObjects[i].isFloor)
			continue;
		if (!available.empty())
			available += ", ";
		available += screenObjects[i].name;
	}
	errorOut = "unknown target '" + name + "'; objects on this screen: " + available;
	return false;
}

// ---------------------------------------------------------------------------
// Synthetic input
// ---------------------------------------------------------------------------

void SkyMcpBridge::warpTo(int gameX, int gameY) {
	// Mouse::mouseEngine() re-adds TOP_LEFT_X/Y, so feed it screen coordinates
	// clamped to the visible area.
	int sx = CLIP(gameX - TOP_LEFT_X, 0, GAME_SCREEN_WIDTH - 1);
	int sy = CLIP(gameY - TOP_LEFT_Y, 0, GAME_SCREEN_HEIGHT - 1);
	_mouse->mouseMoved((uint16)sx, (uint16)sy);
	// Also warp the physical cursor: scripts call warpMouse() themselves
	// (fnAddHuman) and the backend answers every warp with a real
	// EVENT_MOUSEMOVE, so a virtual position that disagrees with the physical
	// one would be overwritten a frame later.
	if (g_system)
		g_system->warpMouse(sx, sy);
}

void SkyMcpBridge::queueClickAt(int gameX, int gameY, uint8 button, uint32 delayFrames) {
	Step s;
	s.kind = kStepClickAt;
	s.x = (uint16)gameX;
	s.y = (uint16)gameY;
	s.target = 0;
	s.button = button;
	s.notBeforeFrame = _frameCounter + delayFrames;
	_steps.push_back(s);
}

void SkyMcpBridge::queueItemClick(uint16 itemCompact, uint8 button) {
	Step open;
	open.kind = kStepOpenMenu;
	open.x = open.y = 0;
	open.target = itemCompact;
	open.button = 0;
	open.notBeforeFrame = 0;
	_steps.push_back(open);

	Step click;
	click.kind = kStepClickItem;
	click.x = click.y = 0;
	click.target = itemCompact;
	click.button = button;
	click.notBeforeFrame = 0;
	_steps.push_back(click);
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

bool SkyMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (hasPendingQuestion()) {
		errorOut = "act: a dialog question is pending — use 'answer' first";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("verb") ||
	    !args.asObject()["verb"]->isString()) {
		errorOut = "act: 'verb' string is required";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	Common::String verb = normalizeActionName(a["verb"]->asString());

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
	if (!canAct()) {
		errorOut = "act: game is not accepting input right now";
		return false;
	}

	uint16 screen1 = 0, screen2 = 0, item1 = 0, item2 = 0;
	if (!resolveTarget(name1, screen1, item1, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}
	if (!name2.empty() && !resolveTarget(name2, screen2, item2, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}

	uint8 button = isLookVerb(verb) ? kLookButton : kActionButton;
	_steps.clear();

	if (item1) {
		// Inventory item as primary target: click its icon in the top bar.
		// The action button arms it (OBJECT_HELD), the look button examines it.
		queueItemClick(item1, name2.empty() ? button : kActionButton);
		if (screen2) {
			// use <item> on <scene object>: with the item armed, click the
			// object; its action script branches on OBJECT_HELD.
			Compact *cpt = _skyCompact->fetchCpt(screen2);
			int cx = 0, cy = 0;
			mouseBoxCenter(cpt, cx, cy);
			queueClickAt(cx, cy, kActionButton, 3);
		} else if (item2) {
			// use <item> on <item>: click the second icon with the first armed.
			queueItemClick(item2, kActionButton);
		}
		Step park;
		park.kind = kStepParkMouse;
		park.x = park.y = 0;
		park.target = 0;
		park.button = 0;
		park.notBeforeFrame = 0;
		_steps.push_back(park);
		beginStream();
		return true;
	}

	if (!screen1) {
		errorOut = "act: '" + name1 + "' cannot be the target of " + verb;
		return false;
	}

	// A plain scene interaction: clear any armed item so the click is not read
	// as "use held object on X", then click the hotspot.
	if (item2 || screen2) {
		errorOut = "act: to use an inventory item on something, pass the item as target1";
		return false;
	}
	Logic::_scriptVariables[OBJECT_HELD] = 0;
	Compact *cpt = _skyCompact->fetchCpt(screen1);
	int cx = 0, cy = 0;
	mouseBoxCenter(cpt, cx, cy);
	queueClickAt(cx, cy, button);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool SkyMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	if (!canAct()) {
		errorOut = "walk: game is not accepting input right now";
		return false;
	}
	int x = (int)args.asObject()["x"]->asIntegerNumber();
	int y = (int)args.asObject()["y"]->asIntegerNumber();

	// The click must land on a floor compact or nothing will move; check first
	// so a miss returns a helpful error instead of a silent no-op.
	Common::Array<ScreenObject> screenObjects;
	collectScreenObjects(screenObjects);
	bool onFloor = false;
	Common::String boxes;
	for (uint i = 0; i < screenObjects.size(); i++) {
		if (!screenObjects[i].isFloor)
			continue;
		const ScreenObject &f = screenObjects[i];
		if (!boxes.empty())
			boxes += ", ";
		boxes += Common::String::format("[%d,%d,%d,%d]", f.x1, f.y1, f.x2, f.y2);
		if (x >= f.x1 && x <= f.x2 && y >= f.y1 && y <= f.y2)
			onFloor = true;
	}
	if (!onFloor) {
		errorOut = Common::String::format(
		    "walk: (%d, %d) is not on a walkable floor; floor areas on this screen: ", x, y) + boxes;
		return false;
	}

	Logic::_scriptVariables[OBJECT_HELD] = 0;
	_steps.clear();
	queueClickAt(x, y, kActionButton);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

bool SkyMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
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
	Common::Array<uint16> choices;
	collectChoices(choices);
	if (choices.empty()) {
		errorOut = "answer: no dialog question is pending";
		return false;
	}
	if (id < 1 || (uint)id > choices.size()) {
		errorOut = Common::String::format("answer: 'id' must be between 1 and %u", choices.size());
		return false;
	}

	// Click the chooser text like a player would.
	Compact *cpt = _skyCompact->fetchCpt(choices[id - 1]);
	int cx = 0, cy = 0;
	mouseBoxCenter(cpt, cx, cy);
	_steps.clear();
	queueClickAt(cx, cy, kActionButton);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool SkyMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// Logic::talk() cuts the current line short on a click (kSkyActionSkipLine
	// does exactly this).
	_mouse->logicClick();
	if (!isStreaming())
		beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool dispatch / gating
// ---------------------------------------------------------------------------

Common::JSONValue *SkyMcpBridge::callTool(const Common::String &name,
                                          const Common::JSONValue &args,
                                          Common::String &errorOut) {
	// The bridge is constructed before SkyEngine::init() builds the
	// subsystems, so a call arriving that early has nothing to read.
	if (!_vm || !_logic || !_mouse || !_skyCompact) {
		errorOut = name + ": the engine is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// Input injection (debug tools)
// ---------------------------------------------------------------------------

void SkyMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void SkyMcpBridge::injectMouseMove(int x, int y) {
	// Debug tools speak screen coordinates, which is what the cursor uses.
	_mouse->mouseMoved((uint16)x, (uint16)y);
}

void SkyMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	(void)isDouble;
	_mouse->mouseMoved((uint16)x, (uint16)y);
	_mouse->buttonPressed(button == "right" ? 1 : 2);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void SkyMcpBridge::snapshotPreAction() {
	_sseActionStarted = false;
	_ssePreScreen = (int)Logic::_scriptVariables[SCREEN];
	_ssePreRoom = _ssePreScreen;
	collectInventory(_ssePreInventory);
	_ssePreInventoryNames.clear();
	for (uint i = 0; i < _ssePreInventory.size(); i++)
		_ssePreInventoryNames.push_back(compactDisplayName(_ssePreInventory[i]));
	_ssePreObjectNames.clear();
	Common::Array<ScreenObject> preObjects;
	collectScreenObjects(preObjects);
	for (uint i = 0; i < preObjects.size(); i++)
		_ssePreObjectNames.push_back(preObjects[i].name);
	Compact *foster = _skyCompact->fetchCpt(ID_FOSTER);
	_ssePrePosX = foster ? foster->xcood : 0;
	_ssePrePosY = foster ? foster->ycood : 0;
}

// True while Foster is doing something a stream should wait for: walking
// (L_AR*), turning, an animation (picking something up runs L_MOD_ANIMATE),
// talking, listening or choosing. Idle he sits at 0 (parked) or L_SCRIPT
// (which the logic engine consumes within the cycle), so everything else
// counts as busy.
static bool fosterBusy(SkyCompact *skyCompact) {
	Compact *foster = skyCompact->fetchCpt(ID_FOSTER);
	if (!foster)
		return false;
	return foster->logic != 0 && foster->logic != L_SCRIPT;
}

// True while any text compact is displaying a line (speech subtitle or
// chooser option). Pointer text — the object name that follows the cursor,
// running the L_CURSOR logic — does not count: it stays alive for as long as
// the (virtual) cursor rests on a hotspot.
static bool anyTextAlive(SkyCompact *skyCompact) {
	for (uint16 id = FIRST_TEXT_COMPACT; id < FIRST_TEXT_COMPACT + kNumTextCompacts; id++) {
		Compact *cpt = skyCompact->fetchCpt(id);
		if (cpt && cpt->status && cpt->logic != L_CURSOR)
			return true;
	}
	return false;
}

void SkyMcpBridge::pumpStreamGame() {
	// Play out the queued synthetic-input steps, one per frame at most. The
	// engine's own Mouse::mouseEngine() consumes the warp + button at the top
	// of the next game cycle, so this is a real click in every respect.
	if (_steps.empty())
		return;
	// pumpStream() also runs from the transport-only pump inside
	// SkyEngine::delay(), i.e. more than once per game cycle. Executing two
	// steps in one cycle would warp the cursor away before Mouse::mouseEngine()
	// has consumed the previous click, so hold this machine to one step per
	// frame-counter tick.
	if (_frameCounter == _lastStepFrame)
		return;
	Step &s = _steps[0];
	if (_frameCounter < s.notBeforeFrame)
		return;

	debug(2, "mcp: step kind=%d target=%u at frame %u (menu=%u)", s.kind, s.target,
	      _frameCounter, Logic::_scriptVariables[MENU]);
	_lastStepFrame = _frameCounter;
	switch (s.kind) {
	case kStepClickAt:
		warpTo(s.x, s.y);
		_mouse->buttonPressed(s.button);
		_steps.remove_at(0);
		break;
	case kStepOpenMenu: {
		// Hold the cursor on the top line until the icon bar is fully down
		// (MENU == 2) and the target item's icon is clickable at its final
		// position; while the bar is still dropping the icons are mouseable
		// but parked above the screen (ycood 112).
		warpTo(TOP_LEFT_X + GAME_SCREEN_WIDTH / 2, TOP_LEFT_Y);
		Compact *icon = _skyCompact->fetchCpt(s.target);
		if (Logic::_scriptVariables[MENU] == 2 &&
		    icon && (icon->status & ST_MOUSE) &&
		    icon->screen == Logic::_scriptVariables[SCREEN] &&
		    icon->ycood >= TOP_LEFT_Y) {
			debug(2, "mcp: icon %u ready at (%d,%d)", s.target, icon->xcood, icon->ycood);
			_steps.remove_at(0);
		}
		break;
	}
	case kStepClickItem: {
		Compact *icon = _skyCompact->fetchCpt(s.target);
		int cx = 0, cy = 0;
		mouseBoxCenter(icon, cx, cy);
		warpTo(cx, cy);
		_mouse->buttonPressed(s.button);
		_steps.remove_at(0);
		break;
	}
	case kStepParkMouse: {
		// Back into the play area so the icon bar closes.
		Compact *foster = _skyCompact->fetchCpt(ID_FOSTER);
		warpTo(foster ? foster->xcood : TOP_LEFT_X + 160,
		       foster ? foster->ycood : TOP_LEFT_Y + 96);
		_steps.remove_at(0);
		break;
	}
	}

	// Space the next step out: the engine consumes this step's warp + button on
	// the next Mouse::mouseEngine(), and the step after that must not stomp the
	// cursor before then. Clicks that arm an item also need a cycle for their
	// script to run.
	if (!_steps.empty() && _steps[0].notBeforeFrame < _frameCounter + 3)
		_steps[0].notBeforeFrame = _frameCounter + 3;
}

void SkyMcpBridge::pumpStreamTrack() {
	// Latch "the action produced something" the moment Foster starts moving or
	// talking, a line appears, or the screen changes; each also bumps the
	// event frame so the timeout is measured from real activity.
	if (fosterBusy(_skyCompact) || anyTextAlive(_skyCompact) || !_steps.empty() ||
	    (int)Logic::_scriptVariables[SCREEN] != _ssePreScreen) {
		if (!_sseActionStarted)
			_sseActionStarted = true;
		_sseLastEventFrame = _frameCounter;
	}
}

bool SkyMcpBridge::streamRoomChanged() const {
	return (int)Logic::_scriptVariables[SCREEN] != _ssePreScreen;
}

bool SkyMcpBridge::isStreamStuck() const {
	// Input locked with nothing being said, nothing queued and Foster idle. A
	// cutscene keeps Foster's logic off L_SCRIPT or a text compact alive, so
	// this only fires when the action genuinely produced nothing.
	if (canAct() || !_steps.empty())
		return false;
	if (fosterBusy(_skyCompact))
		return false;
	return !anyTextAlive(_skyCompact);
}

bool SkyMcpBridge::hasPendingQuestion() const {
	Common::Array<uint16> choices;
	collectChoices(choices);
	return !choices.empty();
}

bool SkyMcpBridge::isActionDone() const {
	if (!_steps.empty())
		return false;
	// A pending chooser is a settled state: the stream closes and reports the
	// question.
	if (hasPendingQuestion())
		return true;
	if (!canAct())
		return false;
	if (fosterBusy(_skyCompact))
		return false; // walking (L_AR*), talking (L_TALK), listening, choosing …
	if (anyTextAlive(_skyCompact))
		return false; // a subtitle is still up
	if (SkyEngine::isCDVersion() && _sound && !_sound->speechFinished())
		return false; // a voice line is still playing
	// Give the click a short window to produce its first visible effect before
	// concluding the action was a no-op.
	if (!_sseActionStarted && _frameCounter - _sseStartFrame < 8)
		return false;
	return true;
}

Common::JSONObject SkyMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	Common::Array<uint16> nowInventory;
	collectInventory(nowInventory);
	auto contains = [](const Common::Array<uint16> &arr, uint16 v) -> bool {
		for (uint i = 0; i < arr.size(); i++)
			if (arr[i] == v) return true;
		return false;
	};

	Common::JSONArray added;
	for (uint i = 0; i < nowInventory.size(); i++) {
		if (!contains(_ssePreInventory, nowInventory[i]))
			added.push_back(mcpJsonString(compactDisplayName(nowInventory[i])));
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

	int screen = (int)Logic::_scriptVariables[SCREEN];
	if (screen != _ssePreScreen)
		changes.setVal("room_changed", mcpJsonInt(screen));

	Compact *foster = _skyCompact->fetchCpt(ID_FOSTER);
	if (foster && (foster->xcood != _ssePrePosX || foster->ycood != _ssePrePosY)) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(foster->xcood));
		pos.setVal("y", mcpJsonInt(foster->ycood));
		changes.setVal("position", new Common::JSONValue(pos));
	}

	// Objects that became clickable or vanished on the same screen (a silent
	// scene change, e.g. picking something up or a hotspot being renamed).
	if (screen == _ssePreScreen) {
		Common::Array<ScreenObject> nowObjects;
		collectScreenObjects(nowObjects);
		auto hasNow = [&nowObjects](const Common::String &n) -> bool {
			for (uint i = 0; i < nowObjects.size(); i++)
				if (nowObjects[i].name == n) return true;
			return false;
		};
		auto hadBefore = [this](const Common::String &n) -> bool {
			for (uint i = 0; i < _ssePreObjectNames.size(); i++)
				if (_ssePreObjectNames[i] == n) return true;
			return false;
		};
		Common::JSONArray objChanges;
		for (uint i = 0; i < nowObjects.size(); i++) {
			if (!hadBefore(nowObjects[i].name)) {
				Common::JSONObject c;
				c.setVal("name", mcpJsonString(nowObjects[i].name));
				c.setVal("old_state", mcpJsonString("hidden"));
				c.setVal("new_state", mcpJsonString("visible"));
				objChanges.push_back(new Common::JSONValue(c));
			}
		}
		for (uint i = 0; i < _ssePreObjectNames.size(); i++) {
			if (!hasNow(_ssePreObjectNames[i])) {
				Common::JSONObject c;
				c.setVal("name", mcpJsonString(_ssePreObjectNames[i]));
				c.setVal("old_state", mcpJsonString("visible"));
				c.setVal("new_state", mcpJsonString("hidden"));
				objChanges.push_back(new Common::JSONValue(c));
			}
		}
		if (!objChanges.empty())
			changes.setVal("objects_changed", new Common::JSONValue(objChanges));
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

	// A chooser that opened during the action.
	Common::Array<uint16> choices;
	collectChoices(choices);
	if (!choices.empty()) {
		Common::JSONArray choiceArr;
		for (uint i = 0; i < choices.size(); i++) {
			Compact *cpt = _skyCompact->fetchCpt(choices[i]);
			Common::JSONObject c;
			c.setVal("id", mcpJsonInt((int)i + 1));
			c.setVal("label", mcpJsonString(MCP::mcpCleanGameText(textString(cpt->getToFlag))));
			choiceArr.push_back(new Common::JSONValue(c));
		}
		Common::JSONObject question;
		question.setVal("choices", new Common::JSONValue(choiceArr));
		changes.setVal("question", new Common::JSONValue(question));
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Tool: debug
// ---------------------------------------------------------------------------

Common::String SkyMcpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics. Sections are selected by "
	       "flag: 'vars' (a slice of Logic::_scriptVariables, with 'from'/'to'), "
	       "'compacts' (every compact in the current screen's mouse list with "
	       "its id, name, hit box and script ids), and 'system' (the engine's "
	       "own read-out: screen, mouse, Foster, chooser). Defaults to 'system'.";
}

Common::JSONValue *SkyMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("vars",     mcpProp("boolean", "Include script variables."));
	props.setVal("from",     mcpProp("integer", "First script variable index (default 0)."));
	props.setVal("to",       mcpProp("integer", "Last script variable index, inclusive (default 63)."));
	props.setVal("compacts", mcpProp("boolean", "Include the current screen's mouse-list compacts."));
	props.setVal("compact",  mcpProp("integer", "Dump one compact's raw fields by id."));
	props.setVal("system",   mcpProp("boolean", "Include the engine state summary (default true)."));
	return mcpObjectSchema(props);
}

Common::JSONValue *SkyMcpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
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
		sys.setVal("screen",       mcpJsonInt((int)Logic::_scriptVariables[SCREEN]));
		sys.setVal("mouse_status", mcpJsonInt((int)Logic::_scriptVariables[MOUSE_STATUS]));
		sys.setVal("mouse_stop",   mcpJsonInt((int)Logic::_scriptVariables[MOUSE_STOP]));
		sys.setVal("special_item", mcpJsonInt((int)Logic::_scriptVariables[SPECIAL_ITEM]));
		sys.setVal("get_off",      mcpJsonInt((int)Logic::_scriptVariables[GET_OFF]));
		sys.setVal("button",       mcpJsonInt((int)Logic::_scriptVariables[BUTTON]));
		sys.setVal("object_held",  mcpJsonInt((int)Logic::_scriptVariables[OBJECT_HELD]));
		sys.setVal("menu",         mcpJsonInt((int)Logic::_scriptVariables[MENU]));
		sys.setVal("menu_length",  mcpJsonInt((int)Logic::_scriptVariables[MENU_LENGTH]));
		sys.setVal("the_chosen_one", mcpJsonInt((int)Logic::_scriptVariables[THE_CHOSEN_ONE]));
		sys.setVal("cur_id",       mcpJsonInt((int)Logic::_scriptVariables[CUR_ID]));
		sys.setVal("inv_vars_base", mcpJsonInt((int)_invVarsBase));
		sys.setVal("mouse_x",      mcpJsonInt(_mouse->giveMouseX()));
		sys.setVal("mouse_y",      mcpJsonInt(_mouse->giveMouseY()));
		sys.setVal("can_act",      mcpJsonBool(canAct()));
		sys.setVal("pending_steps", mcpJsonInt((int)_steps.size()));
		sys.setVal("foster_busy",  mcpJsonBool(fosterBusy(_skyCompact)));
		sys.setVal("text_alive",   mcpJsonBool(anyTextAlive(_skyCompact)));
		{
			Common::JSONArray textStatus;
			for (uint16 id = FIRST_TEXT_COMPACT; id < FIRST_TEXT_COMPACT + kNumTextCompacts; id++) {
				Compact *cpt = _skyCompact->fetchCpt(id);
				textStatus.push_back(mcpJsonInt(cpt ? cpt->status : -1));
			}
			sys.setVal("text_status", new Common::JSONValue(textStatus));
		}
		Compact *foster = _skyCompact->fetchCpt(ID_FOSTER);
		if (foster) {
			sys.setVal("foster_x",     mcpJsonInt(foster->xcood));
			sys.setVal("foster_y",     mcpJsonInt(foster->ycood));
			sys.setVal("foster_logic", mcpJsonInt(foster->logic));
			sys.setVal("foster_mood",  mcpJsonInt(foster->mood));
		}
		out.setVal("system", new Common::JSONValue(sys));
	}

	if (flag("vars", false)) {
		int from = MAX(0, number("from", 0));
		int to   = MIN(NUM_SKY_SCRIPTVARS - 1, number("to", 63));
		Common::JSONArray vars;
		for (int i = from; i <= to; i++) {
			Common::JSONObject v;
			v.setVal("index", mcpJsonInt(i));
			v.setVal("value", mcpJsonInt((int)Logic::_scriptVariables[i]));
			vars.push_back(new Common::JSONValue(v));
		}
		out.setVal("vars", new Common::JSONValue(vars));
	}

	if (number("compact", 0) > 0) {
		uint16 id = (uint16)number("compact", 0);
		Compact *cpt = _skyCompact->fetchCpt(id);
		if (cpt) {
			Common::JSONObject c;
			c.setVal("id",         mcpJsonInt(id));
			c.setVal("name",       mcpJsonString(compactDisplayName(id)));
			c.setVal("logic",      mcpJsonInt(cpt->logic));
			c.setVal("status",     mcpJsonInt(cpt->status));
			c.setVal("screen",     mcpJsonInt(cpt->screen));
			c.setVal("x",          mcpJsonInt(cpt->xcood));
			c.setVal("y",          mcpJsonInt(cpt->ycood));
			c.setVal("mouse_rel_x", mcpJsonInt((int16)cpt->mouseRelX));
			c.setVal("mouse_rel_y", mcpJsonInt((int16)cpt->mouseRelY));
			c.setVal("mouse_size_x", mcpJsonInt(cpt->mouseSizeX));
			c.setVal("mouse_size_y", mcpJsonInt(cpt->mouseSizeY));
			c.setVal("cursor_text", mcpJsonInt(cpt->cursorText));
			c.setVal("mouse_on",    mcpJsonInt(cpt->mouseOn));
			c.setVal("mouse_off",   mcpJsonInt(cpt->mouseOff));
			c.setVal("mouse_click", mcpJsonInt(cpt->mouseClick));
			out.setVal("compact", new Common::JSONValue(c));
		}
	}

	if (flag("compacts", false)) {
		Common::JSONArray comps;
		Common::Array<ScreenObject> screenObjects;
		collectScreenObjects(screenObjects);
		for (uint i = 0; i < screenObjects.size(); i++) {
			const ScreenObject &so = screenObjects[i];
			Compact *cpt = _skyCompact->fetchCpt(so.id);
			Common::JSONObject c;
			c.setVal("id",     mcpJsonInt(so.id));
			c.setVal("name",   mcpJsonString(so.name));
			c.setVal("floor",  mcpJsonBool(so.isFloor));
			c.setVal("logic",  mcpJsonInt(cpt->logic));
			c.setVal("status", mcpJsonInt(cpt->status));
			c.setVal("cursor_text", mcpJsonInt(cpt->cursorText));
			c.setVal("mouse_on",    mcpJsonInt(cpt->mouseOn));
			c.setVal("mouse_click", mcpJsonInt(cpt->mouseClick));
			c.setVal("action_script", mcpJsonInt(cpt->actionScript));
			Common::JSONArray box;
			box.push_back(mcpJsonInt(so.x1));
			box.push_back(mcpJsonInt(so.y1));
			box.push_back(mcpJsonInt(so.x2));
			box.push_back(mcpJsonInt(so.y2));
			c.setVal("box", new Common::JSONValue(box));
			comps.push_back(new Common::JSONValue(c));
		}
		out.setVal("compacts", new Common::JSONValue(comps));
	}

	return new Common::JSONValue(out);
}

} // End of namespace Sky
