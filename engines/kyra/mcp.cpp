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

#include "kyra/mcp.h"
#include "kyra/mcp_names.h"

#include "kyra/detection.h"
#include "kyra/kyra_v1.h"
#include "kyra/engine/kyra_lok.h"
#include "kyra/engine/kyra_v2.h"

#include "common/events.h"
#include "common/system.h"

namespace Kyra {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

KyraMcpBridge *KyraMcpBridge::create(KyraEngine_v1 *vm) {
	KyraMcpBridge *bridge = new KyraMcpBridge(vm);
	bridge->init();
	return bridge;
}

KyraMcpBridge::KyraMcpBridge(KyraEngine_v1 *vm) :
	MCP::McpBridge(vm, "scummvm", "1.0"),
	_vm(vm),
	_inStallPump(false),
	_skipStream(false),
	_pendingClick(false),
	_pendingRight(false),
	_pendingX(0), _pendingY(0),
	_pendingFrame(0),
	_ssePreRoom(-1),
	_ssePreX(0), _ssePreY(0),
	_sseTrackRoom(-1), _sseTrackX(0), _sseTrackY(0) {
}

KyraMcpBridge::~KyraMcpBridge() {
}

bool KyraMcpBridge::engineReady() const {
	return _vm != nullptr;
}

bool KyraMcpBridge::isFirstGame() const {
	return _vm != nullptr && _vm->game() == GI_KYRA1;
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

void KyraMcpBridge::pump() {
	if (!isEnabled())
		return;
	MCP::McpBridge::pump();
}

void KyraMcpBridge::pumpFromStall() {
	if (!isEnabled() || _inStallPump)
		return;
	_inStallPump = true;
	pumpTransportOnly();
	_inStallPump = false;
}

void KyraMcpBridge::pumpGame() {
	// The queued click, once the pointer has been in place long enough for the
	// game's own hit-testing to have seen it arrive.
	if (!_pendingClick)
		return;
	if ((_frameCounter - _pendingFrame) < kPointFrames)
		return;
	_pendingClick = false;
	injectMouseClick(_pendingX, _pendingY, _pendingRight ? "right" : "left", false);
}

// ---------------------------------------------------------------------------
// Reading the game
// ---------------------------------------------------------------------------

int KyraMcpBridge::roomNumber() const {
	if (!engineReady())
		return -1;
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		if (lok->_currentCharacter == nullptr)
			return -1;
		return lok->_currentCharacter->sceneId;
	}
	return static_cast<const KyraEngine_v2 *>(_vm)->_currentScene;
}

void KyraMcpBridge::heroPosition(int &x, int &y) const {
	x = y = 0;
	if (!engineReady())
		return;
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		if (lok->_currentCharacter == nullptr)
			return;
		x = lok->_currentCharacter->x1;
		y = lok->_currentCharacter->y1;
		return;
	}
	const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
	x = v2->_mainCharacter.x1;
	y = v2->_mainCharacter.y1;
}

bool KyraMcpBridge::playerHasControl() const {
	// Kyrandia keeps no "the player is in control" flag of its own, and the
	// nearest thing it has - the hand-item state - is about what is being
	// carried rather than about who is driving. What every engine does answer
	// is whether it would let the game be saved right now, and for an
	// adventure engine that is the same question asked another way: a
	// cutscene, a spoken line and a modal panel all say no, and standing in a
	// room waiting for a click says yes.
	return engineReady() && _vm->canSaveGameStateCurrently(nullptr);
}

Common::String KyraMcpBridge::itemLabel(int itemId) const {
	if (!engineReady() || kyraIsEmptyItem(itemId))
		return Common::String();
	if (!isFirstGame())
		return Common::String();
	// Only the first game keeps a plain table of item names. The later two
	// look their names up through a script string map that is only valid
	// while a particular buffer is loaded, and reaching into that from
	// outside the interpreter is not something a snapshot should do - so
	// those games' items are published by number and an agent learns what
	// they are the way a player does, by looking at them.
	const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
	if (lok->_itemList == nullptr)
		return Common::String();
	const int index = const_cast<KyraEngine_LoK *>(lok)->getItemListIndex((Item)itemId);
	if (index < 0 || index >= lok->_itemList_Size)
		return Common::String();
	const char *name = lok->_itemList[index];
	return name != nullptr ? Common::String(name) : Common::String();
}

void KyraMcpBridge::collectTargets(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	const int room = roomNumber();
	if (room < 0)
		return;

	Common::Array<Common::String> seen;
	// The two generations keep the scene's items in different places: the
	// first game in a table belonging to the room, the later two in one flat
	// list where each entry says which scene it is lying in.
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		if (lok->_roomTable == nullptr)
			return;
		const Room &here = lok->_roomTable[room];
		for (int i = 0; i < 12; i++) {
			const int item = here.itemsTable[i];
			if (kyraIsEmptyItem(item))
				continue;
			Target target;
			target.label = itemLabel(item);
			Common::String name = kyraItemName(target.label);
			if (name.empty())
				name = Common::String::format("item_%d", item);
			uint occurrence = 0;
			for (uint j = 0; j < seen.size(); j++) {
				if (seen[j] == name)
					occurrence++;
			}
			seen.push_back(name);
			target.name = kyraDisambiguate(name, occurrence);
			target.x = here.itemsXPos[i];
			target.y = here.itemsYPos[i];
			target.isExit = false;
			out.push_back(target);
		}
	} else {
		const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
		if (v2->_itemList == nullptr)
			return;
		for (int i = 0; i < v2->_itemListSize; i++) {
			const KyraEngine_v2::ItemDefinition &item = v2->_itemList[i];
			if (item.sceneId != room || kyraIsEmptyItem(item.id))
				continue;
			Target target;
			target.label = itemLabel(item.id);
			Common::String name = kyraItemName(target.label);
			if (name.empty())
				name = Common::String::format("item_%d", item.id);
			uint occurrence = 0;
			for (uint j = 0; j < seen.size(); j++) {
				if (seen[j] == name)
					occurrence++;
			}
			seen.push_back(name);
			target.name = kyraDisambiguate(name, occurrence);
			target.x = item.x;
			target.y = item.y;
			target.isExit = false;
			out.push_back(target);
		}
	}

	// The four ways out. They have no names in either generation - a scene
	// simply says which scene lies north of it, and where on the edge of the
	// picture to click to go there - so the names are the bridge's.
	int exits[4] = { -1, -1, -1, -1 };
	int exitX[4] = { 0, 0, 0, 0 };
	int exitY[4] = { 0, 0, 0, 0 };
	if (isFirstGame()) {
		const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
		if (lok->_roomTable != nullptr) {
			const Room &here = lok->_roomTable[room];
			exits[0] = here.northExit; exits[1] = here.eastExit;
			exits[2] = here.southExit; exits[3] = here.westExit;
			exitX[0] = lok->_sceneExits.northXPos; exitY[0] = lok->_sceneExits.northYPos;
			exitX[1] = lok->_sceneExits.eastXPos;  exitY[1] = lok->_sceneExits.eastYPos;
			exitX[2] = lok->_sceneExits.southXPos; exitY[2] = lok->_sceneExits.southYPos;
			exitX[3] = lok->_sceneExits.westXPos;  exitY[3] = lok->_sceneExits.westYPos;
		}
	} else {
		const KyraEngine_v2 *v2 = static_cast<const KyraEngine_v2 *>(_vm);
		exits[0] = v2->_sceneExit1; exits[1] = v2->_sceneExit2;
		exits[2] = v2->_sceneExit3; exits[3] = v2->_sceneExit4;
		exitX[0] = v2->_sceneEnterX1; exitY[0] = v2->_sceneEnterY1;
		exitX[1] = v2->_sceneEnterX2; exitY[1] = v2->_sceneEnterY2;
		exitX[2] = v2->_sceneEnterX3; exitY[2] = v2->_sceneEnterY3;
		exitX[3] = v2->_sceneEnterX4; exitY[3] = v2->_sceneEnterY4;
	}
	for (int i = 0; i < 4; i++) {
		// 0xFFFF is how both generations spell "there is nothing that way".
		if (exits[i] < 0 || exits[i] == 0xFFFF)
			continue;
		Target target;
		target.name = kyraExitName(i);
		target.label = Common::String::format("to scene %d", exits[i]);
		target.x = exitX[i];
		target.y = exitY[i];
		target.isExit = true;
		out.push_back(target);
	}
}

void KyraMcpBridge::collectInventory(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	Common::Array<Common::String> seen;
	const int slots = isFirstGame() ? 10 : 20;
	for (int i = 0; i < slots; i++) {
		int item;
		if (isFirstGame()) {
			const KyraEngine_LoK *lok = static_cast<const KyraEngine_LoK *>(_vm);
			if (lok->_currentCharacter == nullptr)
				return;
			item = lok->_currentCharacter->inventoryItems[i];
		} else {
			item = static_cast<const KyraEngine_v2 *>(_vm)->_mainCharacter.inventory[i];
		}
		if (kyraIsEmptyItem(item))
			continue;
		Target entry;
		entry.label = itemLabel(item);
		Common::String name = kyraItemName(entry.label);
		if (name.empty())
			name = Common::String::format("item_%d", item);
		uint occurrence = 0;
		for (uint j = 0; j < seen.size(); j++) {
			if (seen[j] == name)
				occurrence++;
		}
		seen.push_back(name);
		entry.name = kyraDisambiguate(name, occurrence);
		entry.x = entry.y = 0;
		entry.isExit = false;
		out.push_back(entry);
	}
}

bool KyraMcpBridge::resolveTarget(const Common::String &name, Target &out,
                                  Common::String &errorOut) const {
	Common::Array<Target> targets;
	collectTargets(targets);
	const Common::String wanted = MCP::McpBridge::normalizeActionName(name);
	for (uint i = 0; i < targets.size(); i++) {
		if (MCP::McpBridge::normalizeActionName(targets[i].name) == wanted) {
			out = targets[i];
			return true;
		}
	}
	Common::String here;
	for (uint i = 0; i < targets.size(); i++) {
		if (!here.empty())
			here += ", ";
		here += targets[i].name;
	}
	if (here.empty())
		here = "nothing";
	errorOut = Common::String::format("nothing here is called '%s'. In this room: %s",
	                                  name.c_str(), here.c_str());
	return false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void KyraMcpBridge::pointAndClick(int x, int y, bool rightButton) {
	// A real mouse hovers before it clicks, and this engine's scripts key off
	// the hover: the click is resolved against whatever the game last saw the
	// pointer over.
	injectMouseMove(x, y);
	_pendingClick = true;
	_pendingRight = rightButton;
	_pendingX = x;
	_pendingY = y;
	_pendingFrame = _frameCounter;
}

void KyraMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event down;
	down.type = Common::EVENT_KEYDOWN;
	down.kbd = ks;
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = Common::EVENT_KEYUP;
	up.kbd = ks;
	g_system->getEventManager()->pushEvent(up);
}

void KyraMcpBridge::injectMouseMove(int x, int y) {
	Common::Event move;
	move.type = Common::EVENT_MOUSEMOVE;
	move.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(move);
	g_system->warpMouse(x, y);
}

void KyraMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool) {
	const bool right = (button == "right");
	Common::Event down;
	down.type = right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
	down.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = right ? Common::EVENT_RBUTTONUP : Common::EVENT_LBUTTONUP;
	up.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(up);
}

int KyraMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

void KyraMcpBridge::onGameText(const Common::String &text) {
	if (!isEnabled() || text.empty())
		return;
	onSystemLine(text);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *KyraMcpBridge::toolState(const Common::JSONValue &, Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "state: the game is still starting up";
		return nullptr;
	}
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	out.setVal("room", new Common::JSONValue(room));

	int x = 0, y = 0;
	heroPosition(x, y);
	Common::JSONObject position;
	position.setVal("x", mcpJsonInt(x));
	position.setVal("y", mcpJsonInt(y));
	out.setVal("position", new Common::JSONValue(position));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));

	Common::Array<Target> targets;
	collectTargets(targets);
	Common::JSONArray objects;
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(targets[i].name));
		entry.setVal("x", mcpJsonInt(targets[i].x));
		entry.setVal("y", mcpJsonInt(targets[i].y));
		entry.setVal("is_exit", mcpJsonBool(targets[i].isExit));
		if (!targets[i].label.empty())
			entry.setVal("label", mcpJsonString(targets[i].label));
		objects.push_back(new Common::JSONValue(entry));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::JSONArray inventory;
	for (uint i = 0; i < carried.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(carried[i].name));
		if (!carried[i].label.empty())
			entry.setVal("label", mcpJsonString(carried[i].label));
		inventory.push_back(new Common::JSONValue(entry));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));

	Common::JSONArray messages;
	for (uint i = 0; i < _messages.size(); i++) {
		const Common::String text = MCP::mcpCleanGameText(_messages[i].text);
		if (text.empty())
			continue;
		Common::JSONObject m;
		m.setVal("text", mcpJsonString(text));
		m.setVal("type", mcpJsonString(_messages[i].type));
		messages.push_back(new Common::JSONValue(m));
	}
	_messages.clear();
	out.setVal("messages", new Common::JSONValue(messages));

	return new Common::JSONValue(out);
}

bool KyraMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (!engineReady()) {
		errorOut = "act: the game is still starting up";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("target1") ||
	    !args.asObject()["target1"]->isString()) {
		errorOut = "act: a string 'target1' is required";
		return false;
	}
	if (!playerHasControl()) {
		errorOut = "act: the game is not accepting input right now";
		return false;
	}

	Common::String verb = "use";
	if (args.asObject().contains("verb") && args.asObject()["verb"]->isString())
		verb = MCP::McpBridge::normalizeActionName(args.asObject()["verb"]->asString());

	Target target;
	if (!resolveTarget(args.asObject()["target1"]->asString(), target, errorOut)) {
		errorOut = Common::String("act: ") + errorOut;
		return false;
	}

	// Two buttons and no verb bar: the left one does whatever the thing is
	// for, the right one looks at it. "take" and "talk_to" are the left
	// button too - picking a thing up and speaking to somebody are both just
	// "do the obvious thing with this" here.
	bool right = false;
	if (verb == "look_at") {
		right = true;
	} else if (verb != "use" && verb != "take" && verb != "talk_to" &&
	           verb != "walk_to" && verb != "open" && verb != "close") {
		errorOut = Common::String::format(
			"act: '%s' is not a verb here. This game has one cursor and two "
			"buttons: 'use' does whatever the thing is for, 'look_at' looks "
			"at it. To use one thing on another, pick the first up with 'take' "
			"and then 'use' it on the second.", verb.c_str());
		return false;
	}

	_skipStream = false;
	pointAndClick(target.x, target.y, right);
	beginStream();
	return true;
}

bool KyraMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	errorOut = "answer: this game never puts a numbered choice to the player";
	return false;
}

bool KyraMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!engineReady()) {
		errorOut = "walk: the game is still starting up";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	if (!playerHasControl()) {
		errorOut = "walk: the game is not accepting input right now";
		return false;
	}
	// Walking is a left click on the floor, which is what it is for a player.
	_skipStream = false;
	pointAndClick((int)args.asObject()["x"]->asIntegerNumber(),
	              (int)args.asObject()["y"]->asIntegerNumber(), false);
	beginStream();
	return true;
}

bool KyraMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	Common::KeyState escape(Common::KEYCODE_ESCAPE, 27);
	injectKey(escape);
	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

Common::JSONValue *KyraMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	out.setVal("ready", mcpJsonBool(engineReady()));
	if (!engineReady())
		return new Common::JSONValue(out);
	out.setVal("game", mcpJsonInt(_vm->game()));
	out.setVal("room", mcpJsonInt(roomNumber()));
	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	out.setVal("item_in_hand", mcpJsonInt(_vm->_mouseState));
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String KyraMcpBridge::stateToolDescription() const {
	return "The room as it is now: its number, where the hero stands, "
	       "everything lying in it that can be clicked, the four ways out, "
	       "what is being carried, and every line said since the last call "
	       "(reading them clears them). The names here are the names act() "
	       "takes.";
}

Common::String KyraMcpBridge::actToolDescription() const {
	return "Act on something state() named. One cursor and two buttons: 'use' "
	       "does whatever the thing is for - picking it up, opening it, "
	       "speaking to it - and 'look_at' looks at it. To use one thing on "
	       "another, 'use' the first (which takes it into the hand) and then "
	       "'use' it on the second.";
}

Common::String KyraMcpBridge::walkToolDescription() const {
	return "Walk the hero to a point by clicking the floor there, which is "
	       "what a player does. To leave the room, act() on one of the exits "
	       "state() lists instead.";
}

Common::String KyraMcpBridge::debugToolDescription() const {
	return "Raw engine state: which of the three games this is, the scene "
	       "number, whether the player has control and what is in the hand.";
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

static Common::JSONValue *kyraObjectArraySchema(bool withPosition) {
	Common::JSONObject props;
	props.setVal("name", Networking::mcpProp("string", "Name to use in act()."));
	props.setVal("label", Networking::mcpProp("string",
	    "What the game itself calls it, where it has a name for it. The later "
	    "two games keep their item names in a script table the snapshot cannot "
	    "safely read, so their items are named by number instead."));
	if (withPosition) {
		props.setVal("x", Networking::mcpProp("integer", "Where it is, in game coordinates."));
		props.setVal("y", Networking::mcpProp("integer", "Where it is, in game coordinates."));
		props.setVal("is_exit", Networking::mcpProp("boolean",
		    "Whether acting on it leaves the room."));
	}
	Common::JSONObject item;
	item.setVal("type", Networking::mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject array;
	array.setVal("type", Networking::mcpJsonString("array"));
	array.setVal("items", new Common::JSONValue(item));
	return new Common::JSONValue(array);
}

void KyraMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input right now."));
	outputProps.setVal("objects", kyraObjectArraySchema(true));
	outputProps.setVal("inventory", kyraObjectArraySchema(false));
}

void KyraMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	props.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input now the action is over."));
	props.setVal("objects_appeared", Networking::mcpProp("array",
	    "Things in the room that were not there before."));
	props.setVal("objects_gone", Networking::mcpProp("array",
	    "Things that were in the room and are not now."));
	props.setVal("items_gained", Networking::mcpProp("array",
	    "Items picked up while the action ran."));
	props.setVal("items_lost", Networking::mcpProp("array",
	    "Items put down or used up while the action ran."));
}

void KyraMcpBridge::augmentActSchema(Common::JSONObject &props) {
	props.setVal("verb", Networking::mcpProp("string",
	    "'use' or 'look_at'. There is no verb bar in this game: those are the "
	    "two mouse buttons."));
}

Common::JSONValue *KyraMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	return Networking::mcpObjectSchema(props);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void KyraMcpBridge::snapshotPreAction() {
	_ssePreRoom = roomNumber();
	heroPosition(_ssePreX, _ssePreY);
	_ssePreTargets.clear();
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);
	_ssePreInventory.clear();
	Common::Array<Target> carried;
	collectInventory(carried);
	for (uint i = 0; i < carried.size(); i++)
		_ssePreInventory.push_back(carried[i].name);
	_sseTrackRoom = _ssePreRoom;
	_sseTrackX = _ssePreX;
	_sseTrackY = _ssePreY;
}

static void diffNames(const Common::Array<Common::String> &before,
                      const Common::Array<Common::String> &after,
                      Common::JSONArray &appeared, Common::JSONArray &gone) {
	for (uint i = 0; i < after.size(); i++) {
		bool was = false;
		for (uint j = 0; j < before.size(); j++)
			was = was || before[j] == after[i];
		if (!was)
			appeared.push_back(Networking::mcpJsonString(after[i]));
	}
	for (uint j = 0; j < before.size(); j++) {
		bool still = false;
		for (uint i = 0; i < after.size(); i++)
			still = still || after[i] == before[j];
		if (!still)
			gone.push_back(Networking::mcpJsonString(before[j]));
	}
}

Common::JSONObject KyraMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	const int room = roomNumber();
	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt(room));
	roomObj.setVal("changed", mcpJsonBool(room != _ssePreRoom));
	out.setVal("room", new Common::JSONValue(roomObj));

	int x = 0, y = 0;
	heroPosition(x, y);
	Common::JSONObject pos;
	pos.setVal("x", mcpJsonInt(x));
	pos.setVal("y", mcpJsonInt(y));
	pos.setVal("changed", mcpJsonBool(x != _ssePreX || y != _ssePreY));
	out.setVal("position", new Common::JSONValue(pos));

	Common::Array<Target> targets;
	collectTargets(targets);
	Common::Array<Common::String> now;
	for (uint i = 0; i < targets.size(); i++)
		now.push_back(targets[i].name);
	Common::JSONArray appeared, gone;
	diffNames(_ssePreTargets, now, appeared, gone);
	out.setVal("objects_appeared", new Common::JSONValue(appeared));
	out.setVal("objects_gone", new Common::JSONValue(gone));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::Array<Common::String> held;
	for (uint i = 0; i < carried.size(); i++)
		held.push_back(carried[i].name);
	Common::JSONArray gained, lost;
	diffNames(_ssePreInventory, held, gained, lost);
	out.setVal("items_gained", new Common::JSONValue(gained));
	out.setVal("items_lost", new Common::JSONValue(lost));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	return out;
}

bool KyraMcpBridge::isActionDone() const {
	if (_skipStream) {
		if ((_frameCounter - _sseStartFrame) >= 20)
			return true;
		return g_system != nullptr && (g_system->getMillis() - _sseStartMs) >= kSkipMs;
	}
	return !_pendingClick && playerHasControl();
}

bool KyraMcpBridge::hasPendingQuestion() const {
	return false;
}

bool KyraMcpBridge::streamRoomChanged() const {
	return roomNumber() != _ssePreRoom;
}

void KyraMcpBridge::pumpStreamTrack() {
	const int room = roomNumber();
	int x = 0, y = 0;
	heroPosition(x, y);
	if (room != _sseTrackRoom || x != _sseTrackX || y != _sseTrackY) {
		_sseTrackRoom = room;
		_sseTrackX = x;
		_sseTrackY = y;
		_sseLastEventFrame = _frameCounter;
	}
}

} // End of namespace Kyra
