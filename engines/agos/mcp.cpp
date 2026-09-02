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

#include "agos/mcp.h"
#include "agos/mcp_names.h"

#include "agos/agos.h"
#include "agos/intern.h"

#include "common/events.h"
#include "common/system.h"

namespace AGOS {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AgosMcpBridge *AgosMcpBridge::create(AGOSEngine *vm) {
	AgosMcpBridge *bridge = new AgosMcpBridge(vm);
	bridge->init();
	return bridge;
}

AgosMcpBridge::AgosMcpBridge(AGOSEngine *vm) :
	MCP::McpBridge(vm, "scummvm", "1.0"),
	_vm(vm),
	_inPump(false),
	_skipStream(false),
	_lastFrameMs(0),
	_ssePreRoom(-1),
	_sseTrackRoom(-1),
	_sseTrackSteps(0) {
}

AgosMcpBridge::~AgosMcpBridge() {
}

bool AgosMcpBridge::engineReady() const {
	// The bridge is built in the engine's constructor so the port binds before
	// anything can block on startup; the item table is the last of the pieces
	// it reads to be built.
	return _vm != nullptr && _vm->_itemArrayPtr != nullptr;
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

void AgosMcpBridge::pumpFromDelay() {
	if (!isEnabled() || _inPump)
		return;
	_inPump = true;
	// delay() is called from the main loop, from waitForInput() and from every
	// blocking wait in between, at wildly different rates. A frame is defined
	// by the wall clock instead, so the streaming budgets mean the same thing
	// however hot the calling loop happens to be.
	const uint32 now = g_system != nullptr ? g_system->getMillis() : 0;
	if (now - _lastFrameMs >= kFrameMs) {
		_lastFrameMs = now;
		MCP::McpBridge::pump();
	} else {
		pumpTransportOnly();
	}
	_inPump = false;
}

void AgosMcpBridge::pumpGame() {
	// One queued step per frame, so a click is never sent before the hover
	// that should precede it has been taken.
	if (_steps.empty())
		return;
	Step &step = _steps[0];
	if (_frameCounter < step.notBeforeFrame)
		return;
	switch (step.kind) {
	case kStepHover:
		injectMouseMove(step.x, step.y);
		break;
	case kStepClick:
		injectMouseClick(step.x, step.y, "left", false);
		break;
	case kStepSettle:
		break;
	}
	_steps.remove_at(0);
	if (!_steps.empty())
		_steps[0].notBeforeFrame = _frameCounter + kStepFrames;
}

void AgosMcpBridge::queueStep(StepKind kind, int x, int y, uint32 delayFrames) {
	Step step;
	step.kind = kind;
	step.x = x;
	step.y = y;
	step.notBeforeFrame = _frameCounter + delayFrames;
	_steps.push_back(step);
}

// ---------------------------------------------------------------------------
// Reading the game
// ---------------------------------------------------------------------------

int AgosMcpBridge::roomNumber() const {
	if (!engineReady())
		return -1;
	// The room is the item the player is inside: this engine keeps its world
	// as a tree of items, and moving between rooms is reparenting the player.
	// `_currentRoom` looks like the answer and is not - it is Waxworks' own
	// bookkeeping (AGOSEngine::loadRoomItems), and stays 0 for the whole of
	// Simon the Sorcerer - so it is only the fallback, for the games that do
	// keep it.
	Item *player = _vm->me();
	if (player != nullptr && player->parent != 0)
		return (int)player->parent;
	return (int)_vm->_currentRoom;
}

bool AgosMcpBridge::playerHasControl() const {
	// AGOS implements no canSaveGameStateCurrently() of its own, so the answer
	// every other bridge here leans on is not available. What it does have is
	// waitForInput(), which is where its whole loop sits between actions -
	// being inside that is exactly what "the game is waiting for the player"
	// means, and it is false for the whole of a cutscene.
	return engineReady() && _vm->_mcpWaitingForInput;
}

Common::String AgosMcpBridge::itemLabel(const Item *item) const {
	if (!engineReady() || item == nullptr)
		return Common::String();
	// The same lookup displayName() makes when the pointer rests on something:
	// the item's object child carries a string id, and that string is the name
	// the player reads along the bottom of the screen.
	SubObject *object = (SubObject *)_vm->findChildOfType(const_cast<Item *>(item), kObjectType);
	if (object == nullptr)
		return Common::String();
	const byte *text = _vm->getStringPtrByID(object->objectName);
	return text != nullptr ? Common::String((const char *)text) : Common::String();
}

void AgosMcpBridge::collectTargets(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	Common::Array<Common::String> seen;
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea &area = _vm->_hitAreas[i];
		// A dead box is one the scripts have switched off; a box with no item
		// behind it is a piece of interface (the verb bar, the scroll arrows)
		// rather than a thing in the room.
		if (area.flags & kBFBoxDead)
			continue;
		if (area.itemPtr == nullptr)
			continue;
		if (area.width == 0 || area.height == 0)
			continue;
		const Common::String label = itemLabel(area.itemPtr);
		Common::String name = agosObjectName(label);
		if (name.empty())
			continue;
		uint occurrence = 0;
		for (uint j = 0; j < seen.size(); j++) {
			if (seen[j] == name)
				occurrence++;
		}
		seen.push_back(name);

		Target target;
		target.name = agosDisambiguate(name, occurrence);
		target.label = label;
		target.x = area.x + area.width / 2;
		target.y = area.y + area.height / 2;
		target.hitAreaId = area.id;
		out.push_back(target);
	}
}

void AgosMcpBridge::collectInventory(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	Item *player = _vm->me();
	if (player == nullptr)
		return;
	Common::Array<Common::String> seen;
	// The engine keeps what is carried as the player item's children, threaded
	// through `next`.
	for (Item *held = _vm->derefItem(player->child); held != nullptr;
	     held = _vm->derefItem(held->next)) {
		const Common::String label = itemLabel(held);
		Common::String name = agosObjectName(label);
		if (name.empty())
			continue;
		uint occurrence = 0;
		for (uint j = 0; j < seen.size(); j++) {
			if (seen[j] == name)
				occurrence++;
		}
		seen.push_back(name);

		Target entry;
		entry.name = agosDisambiguate(name, occurrence);
		entry.label = label;
		entry.x = entry.y = 0;
		entry.hitAreaId = 0;
		out.push_back(entry);
	}
}

bool AgosMcpBridge::resolveTarget(const Common::String &name, Target &out,
                                  Common::String &errorOut) const {
	Common::Array<Target> targets;
	collectTargets(targets);
	collectInventory(targets);
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

bool AgosMcpBridge::verbButtonPosition(int index, int &x, int &y) const {
	if (!engineReady() || index < 0)
		return false;
	// The bar is hit areas like everything else, and each carries the verb it
	// stands for. Finding the button by what it does - rather than by where it
	// is - means a game that lays its bar out differently still works.
	for (uint i = 0; i < ARRAYSIZE(_vm->_hitAreas); i++) {
		const HitArea &area = _vm->_hitAreas[i];
		if (area.flags & kBFBoxDead)
			continue;
		if (area.width == 0 || area.height == 0)
			continue;
		// The engine numbers its verbs from 1 in the order the bar is written.
		if ((area.verb & 0x3FFF) != (uint16)(index + 1))
			continue;
		x = area.x + area.width / 2;
		y = area.y + area.height / 2;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void AgosMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event down;
	down.type = Common::EVENT_KEYDOWN;
	down.kbd = ks;
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = Common::EVENT_KEYUP;
	up.kbd = ks;
	g_system->getEventManager()->pushEvent(up);
}

void AgosMcpBridge::injectMouseMove(int x, int y) {
	Common::Event move;
	move.type = Common::EVENT_MOUSEMOVE;
	move.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(move);
	g_system->warpMouse(x, y);
}

void AgosMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool) {
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

int AgosMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

void AgosMcpBridge::onGameText(const Common::String &text) {
	if (!isEnabled() || text.empty())
		return;
	onSystemLine(text);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *AgosMcpBridge::toolState(const Common::JSONValue &, Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "state: the game is still starting up";
		return nullptr;
	}
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	out.setVal("room", new Common::JSONValue(room));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));

	// All twelve, always. The bar is the same in every game this engine runs,
	// and an agent that can read it never has to guess which words are taken.
	Common::JSONArray verbs;
	for (int i = 0; i < agosVerbCount(); i++)
		verbs.push_back(mcpJsonString(agosVerbName(i)));
	out.setVal("verbs", new Common::JSONValue(verbs));

	Common::Array<Target> targets;
	collectTargets(targets);
	Common::JSONArray objects;
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(targets[i].name));
		entry.setVal("label", mcpJsonString(targets[i].label));
		entry.setVal("x", mcpJsonInt(targets[i].x));
		entry.setVal("y", mcpJsonInt(targets[i].y));
		objects.push_back(new Common::JSONValue(entry));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::JSONArray inventory;
	for (uint i = 0; i < carried.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(carried[i].name));
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

bool AgosMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
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

	Common::String verb = "look_at";
	if (args.asObject().contains("verb") && args.asObject()["verb"]->isString())
		verb = MCP::McpBridge::normalizeActionName(args.asObject()["verb"]->asString());

	const int index = agosVerbIndex(verb);
	if (index < 0) {
		Common::String list;
		for (int i = 0; i < agosVerbCount(); i++) {
			if (!list.empty())
				list += ", ";
			list += agosVerbName(i);
		}
		errorOut = Common::String::format("act: '%s' is not a verb here. The bar has: %s.",
		                                  verb.c_str(), list.c_str());
		return false;
	}

	Target target;
	if (!resolveTarget(args.asObject()["target1"]->asString(), target, errorOut)) {
		errorOut = Common::String("act: ") + errorOut;
		return false;
	}

	// Asked *after* the name has been checked, deliberately. A name this room
	// does not have is wrong whatever the game happens to be doing, and saying
	// so is the only way a caller learns it; answering "not accepting input"
	// instead sends it away to wait for a moment that would not have helped.
	if (!playerHasControl()) {
		errorOut = "act: the game is not accepting input right now";
		return false;
	}

	int verbX = 0, verbY = 0;
	if (!verbButtonPosition(index, verbX, verbY)) {
		errorOut = Common::String::format(
			"act: the verb bar is not showing, so '%s' cannot be chosen. "
			"This happens during a cutscene and while a panel is open.",
			verb.c_str());
		return false;
	}

	// Exactly what a player does: click the word, then click the thing. Each
	// click is preceded by a hover, because the scripts act on the hover -
	// clicking without one resolves against wherever the pointer was before.
	_steps.clear();
	queueStep(kStepHover, verbX, verbY, 0);
	queueStep(kStepClick, verbX, verbY, kStepFrames);
	queueStep(kStepHover, target.x, target.y, kStepFrames);
	queueStep(kStepClick, target.x, target.y, kStepFrames);
	queueStep(kStepSettle, 0, 0, kStepFrames);

	_skipStream = false;
	beginStream();
	return true;
}

bool AgosMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	errorOut = "answer: this game never puts a numbered choice to the player";
	return false;
}

bool AgosMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
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

	const int x = (int)args.asObject()["x"]->asIntegerNumber();
	const int y = (int)args.asObject()["y"]->asIntegerNumber();

	// Walking is "Walk to" on the bar and then a click on the floor, which is
	// how a player walks here too.
	int verbX = 0, verbY = 0;
	_steps.clear();
	if (verbButtonPosition(agosVerbIndex("walk_to"), verbX, verbY)) {
		queueStep(kStepHover, verbX, verbY, 0);
		queueStep(kStepClick, verbX, verbY, kStepFrames);
	}
	queueStep(kStepHover, x, y, kStepFrames);
	queueStep(kStepClick, x, y, kStepFrames);
	queueStep(kStepSettle, 0, 0, kStepFrames);

	_skipStream = false;
	beginStream();
	return true;
}

bool AgosMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// Two different waits, and skipping means getting past either: a running
	// cutscene ends through the engine's own exit-cutscene flag, and a line
	// sitting there to be dismissed goes away on a keypress.
	if (_vm != nullptr)
		_vm->mcpExitCutscene();
	Common::KeyState escape(Common::KEYCODE_ESCAPE, 27);
	injectKey(escape);
	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

Common::JSONValue *AgosMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	out.setVal("ready", mcpJsonBool(engineReady()));
	if (!engineReady())
		return new Common::JSONValue(out);
	out.setVal("room", mcpJsonInt(roomNumber()));
	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	out.setVal("verb_hit_area", mcpJsonInt(_vm->_verbHitArea));
	out.setVal("queued_steps", mcpJsonInt((int)_steps.size()));
	Common::Array<Target> targets;
	collectTargets(targets);
	out.setVal("clickable", mcpJsonInt((int)targets.size()));
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String AgosMcpBridge::stateToolDescription() const {
	return "The room as it is now: its number, everything in it that can be "
	       "clicked with the words the game itself shows for them, the twelve "
	       "verbs on the bar, what is being carried, and every line said since "
	       "the last call (reading them clears them). The names here are the "
	       "names act() takes.";
}

Common::String AgosMcpBridge::actToolDescription() const {
	return "Act on something state() named. This game has a bar of twelve "
	       "verbs and an action is one of them applied to one thing, which is "
	       "done the way a player does it: the word is clicked and then the "
	       "thing is. state() lists the twelve.";
}

Common::String AgosMcpBridge::walkToolDescription() const {
	return "Walk to a point by choosing 'Walk to' on the bar and clicking the "
	       "floor there, which is what a player does. To go through a door, "
	       "act() on it instead.";
}

Common::String AgosMcpBridge::debugToolDescription() const {
	return "Raw engine state: the room, whether the player has control, the "
	       "verb the engine currently has selected, how many things are "
	       "clickable and how much queued input is still to be played out.";
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

static Common::JSONValue *agosObjectArraySchema(bool withPosition) {
	Common::JSONObject props;
	props.setVal("name", Networking::mcpProp("string", "Name to use in act()."));
	props.setVal("label", Networking::mcpProp("string",
	    "The words the game itself writes for it when the pointer rests on it."));
	if (withPosition) {
		props.setVal("x", Networking::mcpProp("integer", "Where it is, in game coordinates."));
		props.setVal("y", Networking::mcpProp("integer", "Where it is, in game coordinates."));
	}
	Common::JSONObject item;
	item.setVal("type", Networking::mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject array;
	array.setVal("type", Networking::mcpJsonString("array"));
	array.setVal("items", new Common::JSONValue(item));
	return new Common::JSONValue(array);
}

void AgosMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input right now."));
	outputProps.setVal("verbs", Networking::mcpProp("array",
	    "The twelve verbs on the bar, which are the verbs act() takes."));
	outputProps.setVal("objects", agosObjectArraySchema(true));
	outputProps.setVal("inventory", agosObjectArraySchema(false));
}

void AgosMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	// The base schema carries `room_changed` as a plain flag; this bridge
	// answers with the room itself, because an agent that has just left one
	// wants to know where it came out.
	Common::JSONObject room;
	room.setVal("type", Networking::mcpJsonString("object"));
	Common::JSONObject roomProps;
	roomProps.setVal("id", Networking::mcpProp("integer", "The room now."));
	roomProps.setVal("changed", Networking::mcpProp("boolean",
	    "Whether this action left the room it started in."));
	room.setVal("properties", new Common::JSONValue(roomProps));
	props.setVal("room", new Common::JSONValue(room));

	props.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input now the action is over."));
	props.setVal("objects_appeared", Networking::mcpProp("array",
	    "Things in the room that were not there before."));
	props.setVal("objects_gone", Networking::mcpProp("array",
	    "Things that were in the room and are not now."));
	props.setVal("items_gained", Networking::mcpProp("array",
	    "Items picked up while the action ran."));
	props.setVal("items_lost", Networking::mcpProp("array",
	    "Items put down or given away while the action ran."));
}

void AgosMcpBridge::augmentActSchema(Common::JSONObject &props) {
	Common::String list;
	for (int i = 0; i < agosVerbCount(); i++) {
		if (!list.empty())
			list += ", ";
		list += agosVerbName(i);
	}
	const Common::String desc = "One of the twelve verbs on the bar: " + list + ".";
	props.setVal("verb", Networking::mcpProp("string", desc.c_str()));
}

Common::JSONValue *AgosMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	return Networking::mcpObjectSchema(props);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void AgosMcpBridge::snapshotPreAction() {
	_ssePreRoom = roomNumber();
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
	_sseTrackSteps = _steps.size();
}

static void agosDiffNames(const Common::Array<Common::String> &before,
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

Common::JSONObject AgosMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	const int room = roomNumber();
	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt(room));
	roomObj.setVal("changed", mcpJsonBool(room != _ssePreRoom));
	out.setVal("room", new Common::JSONValue(roomObj));

	Common::Array<Target> targets;
	collectTargets(targets);
	Common::Array<Common::String> now;
	for (uint i = 0; i < targets.size(); i++)
		now.push_back(targets[i].name);
	Common::JSONArray appeared, gone;
	agosDiffNames(_ssePreTargets, now, appeared, gone);
	out.setVal("objects_appeared", new Common::JSONValue(appeared));
	out.setVal("objects_gone", new Common::JSONValue(gone));

	Common::Array<Target> carried;
	collectInventory(carried);
	Common::Array<Common::String> held;
	for (uint i = 0; i < carried.size(); i++)
		held.push_back(carried[i].name);
	Common::JSONArray gained, lost;
	agosDiffNames(_ssePreInventory, held, gained, lost);
	out.setVal("items_gained", new Common::JSONValue(gained));
	out.setVal("items_lost", new Common::JSONValue(lost));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	return out;
}

bool AgosMcpBridge::isActionDone() const {
	if (_skipStream) {
		if ((_frameCounter - _sseStartFrame) >= 12)
			return true;
		return g_system != nullptr && (g_system->getMillis() - _sseStartMs) >= kSkipMs;
	}
	// Not until every queued click has been played out, and then not until the
	// game is taking input again.
	return _steps.empty() && playerHasControl();
}

bool AgosMcpBridge::hasPendingQuestion() const {
	return false;
}

bool AgosMcpBridge::streamRoomChanged() const {
	return roomNumber() != _ssePreRoom;
}

void AgosMcpBridge::pumpStreamTrack() {
	const int room = roomNumber();
	if (room != _sseTrackRoom || _steps.size() != _sseTrackSteps) {
		_sseTrackRoom = room;
		_sseTrackSteps = _steps.size();
		_sseLastEventFrame = _frameCounter;
	}
}

} // End of namespace AGOS
