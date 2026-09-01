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

#include "asylum/mcp.h"
#include "asylum/mcp_names.h"

#include "asylum/asylum.h"
#include "asylum/resources/actor.h"
#include "asylum/resources/object.h"
#include "asylum/resources/worldstats.h"
#include "asylum/views/scene.h"

#include "common/events.h"
#include "common/system.h"

namespace Asylum {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AsylumMcpBridge *AsylumMcpBridge::create(AsylumEngine *vm) {
	AsylumMcpBridge *bridge = new AsylumMcpBridge(vm);
	bridge->init();
	return bridge;
}

AsylumMcpBridge::AsylumMcpBridge(AsylumEngine *vm) :
	MCP::McpBridge(vm, "scummvm", "1.0"),
	_vm(vm),
	_inPump(false),
	_skipStream(false),
	_lastFrameMs(0),
	_pendingClick(false),
	_pendingRight(false),
	_pendingX(0), _pendingY(0),
	_pendingFrame(0),
	_ssePreRoom(-1),
	_ssePreX(0), _ssePreY(0),
	_sseTrackRoom(-1), _sseTrackX(0), _sseTrackY(0) {
}

AsylumMcpBridge::~AsylumMcpBridge() {
}

bool AsylumMcpBridge::engineReady() const {
	// The bridge is built before the subsystems are, so the port binds before
	// anything can block on startup. A scene with its world loaded is the
	// earliest point anything here can be read.
	return _vm != nullptr && _vm->scene() != nullptr &&
	       _vm->scene()->worldstats() != nullptr;
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

void AsylumMcpBridge::pumpFromEvents() {
	if (!isEnabled() || _inPump)
		return;
	_inPump = true;
	// handleEvents() is reached from the game, the menu, the video player and
	// the puzzles, at wildly different rates. A frame is defined by the wall
	// clock instead, so the streaming budgets mean the same thing wherever the
	// game happens to be.
	const uint32 now = g_system != nullptr ? g_system->getMillis() : 0;
	if (now - _lastFrameMs >= kFrameMs) {
		_lastFrameMs = now;
		MCP::McpBridge::pump();
	} else {
		pumpTransportOnly();
	}
	_inPump = false;
}

void AsylumMcpBridge::pumpGame() {
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

int AsylumMcpBridge::roomNumber() const {
	if (!engineReady())
		return -1;
	// The scene's resource pack is what identifies it; the chapter is the
	// coarser number and is reported beside it in state().
	return (int)_vm->scene()->getPackId();
}

void AsylumMcpBridge::heroPosition(int &x, int &y) const {
	x = y = 0;
	if (!engineReady())
		return;
	Actor *player = _vm->scene()->getActor();
	if (player == nullptr)
		return;
	const Common::Point *point = player->getPoint1();
	if (point == nullptr)
		return;
	x = point->x;
	y = point->y;
}

bool AsylumMcpBridge::playerHasControl() const {
	// Sanitarium keeps no flag of its own for this. What the engine does
	// answer is whether it would let the game be saved, which for an adventure
	// engine is the same question: a cutscene, a video and a puzzle panel all
	// say no, and standing in a room waiting for a click says yes.
	return engineReady() && _vm->canSaveGameStateCurrently(nullptr);
}

void AsylumMcpBridge::collectTargets(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	WorldStats *world = _vm->scene()->worldstats();
	Common::Array<Common::String> seen;
	for (uint i = 0; i < world->objects.size(); i++) {
		Object *object = world->objects[i];
		if (object == nullptr)
			continue;
		// An object the scripts have switched off is not in the room as far as
		// the player is concerned.
		if (!object->isOnScreen())
			continue;
		const char *raw = object->getName();
		if (raw == nullptr)
			continue;
		const Common::String label(raw);
		if (asylumIsPlaceholderName(label))
			continue;
		Common::String name = asylumObjectName(label);
		if (name.empty())
			continue;
		uint occurrence = 0;
		for (uint j = 0; j < seen.size(); j++) {
			if (seen[j] == name)
				occurrence++;
		}
		seen.push_back(name);

		Target target;
		target.name = asylumDisambiguate(name, occurrence);
		target.label = label;
		// The bounding rectangle is where the game hit-tests, so its middle is
		// where a player would aim.
		Common::Rect *bounds = object->getBoundingRect();
		if (bounds != nullptr && bounds->width() > 0 && bounds->height() > 0) {
			target.x = object->x + bounds->left + bounds->width() / 2;
			target.y = object->y + bounds->top + bounds->height() / 2;
		} else {
			target.x = object->x;
			target.y = object->y;
		}
		out.push_back(target);
	}
}

bool AsylumMcpBridge::resolveTarget(const Common::String &name, Target &out,
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

void AsylumMcpBridge::pointAndClick(int x, int y, bool rightButton) {
	injectMouseMove(x, y);
	_pendingClick = true;
	_pendingRight = rightButton;
	_pendingX = x;
	_pendingY = y;
	_pendingFrame = _frameCounter;
}

void AsylumMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event down;
	down.type = Common::EVENT_KEYDOWN;
	down.kbd = ks;
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = Common::EVENT_KEYUP;
	up.kbd = ks;
	g_system->getEventManager()->pushEvent(up);
}

void AsylumMcpBridge::injectMouseMove(int x, int y) {
	Common::Event move;
	move.type = Common::EVENT_MOUSEMOVE;
	move.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(move);
	g_system->warpMouse(x, y);
}

void AsylumMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool) {
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

int AsylumMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

void AsylumMcpBridge::onGameText(const Common::String &text) {
	if (!isEnabled() || text.empty())
		return;
	onSystemLine(text);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *AsylumMcpBridge::toolState(const Common::JSONValue &, Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "state: the game is still starting up";
		return nullptr;
	}
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	room.setVal("chapter", mcpJsonInt((int)_vm->scene()->worldstats()->chapter));
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
		entry.setVal("label", mcpJsonString(targets[i].label));
		entry.setVal("x", mcpJsonInt(targets[i].x));
		entry.setVal("y", mcpJsonInt(targets[i].y));
		objects.push_back(new Common::JSONValue(entry));
	}
	out.setVal("objects", new Common::JSONValue(objects));

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

bool AsylumMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
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

	// No verb bar and no verbs: the cursor changes shape to say what a click
	// would do, and the two buttons are the whole vocabulary.
	bool right = false;
	if (verb == "look_at") {
		right = true;
	} else if (verb != "use" && verb != "take" && verb != "talk_to" &&
	           verb != "walk_to" && verb != "open" && verb != "close") {
		errorOut = Common::String::format(
			"act: '%s' is not a verb here. This game has no verbs at all: "
			"'use' does whatever the thing under the cursor is for and "
			"'look_at' looks at it.", verb.c_str());
		return false;
	}

	_skipStream = false;
	pointAndClick(target.x, target.y, right);
	beginStream();
	return true;
}

bool AsylumMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	errorOut = "answer: this game never puts a numbered choice to the player";
	return false;
}

bool AsylumMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
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
	_skipStream = false;
	pointAndClick((int)args.asObject()["x"]->asIntegerNumber(),
	              (int)args.asObject()["y"]->asIntegerNumber(), false);
	beginStream();
	return true;
}

bool AsylumMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
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

Common::JSONValue *AsylumMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	out.setVal("ready", mcpJsonBool(engineReady()));
	if (!engineReady())
		return new Common::JSONValue(out);
	out.setVal("scene", mcpJsonInt(roomNumber()));
	out.setVal("chapter", mcpJsonInt((int)_vm->scene()->worldstats()->chapter));
	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	out.setVal("objects_in_world",
	           mcpJsonInt((int)_vm->scene()->worldstats()->objects.size()));
	Common::Array<Target> targets;
	collectTargets(targets);
	out.setVal("named_here", mcpJsonInt((int)targets.size()));
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String AsylumMcpBridge::stateToolDescription() const {
	return "The room as it is now: which scene and chapter it is, where the "
	       "player character stands, everything on screen that has a name, and "
	       "every line said since the last call (reading them clears them). "
	       "The names come from the game's own data rather than from anything "
	       "painted on screen, and they are the names act() takes.";
}

Common::String AsylumMcpBridge::actToolDescription() const {
	return "Act on something state() named. This game has no verbs: the "
	       "cursor changes shape to say what a click would do, so 'use' does "
	       "whatever the thing is for and 'look_at' looks at it.";
}

Common::String AsylumMcpBridge::walkToolDescription() const {
	return "Walk the player character to a point by clicking the floor there, "
	       "which is what a player does. To leave the room, act() on the way "
	       "out instead.";
}

Common::String AsylumMcpBridge::debugToolDescription() const {
	return "Raw engine state: the scene and chapter, whether the player has "
	       "control, how many objects the world holds and how many of them "
	       "have a name worth offering.";
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

void AsylumMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input right now."));
	Common::JSONObject props;
	props.setVal("name", Networking::mcpProp("string", "Name to use in act()."));
	props.setVal("label", Networking::mcpProp("string",
	    "The name the game's own data carries for it."));
	props.setVal("x", Networking::mcpProp("integer", "Where it is, in game coordinates."));
	props.setVal("y", Networking::mcpProp("integer", "Where it is, in game coordinates."));
	Common::JSONObject item;
	item.setVal("type", Networking::mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject array;
	array.setVal("type", Networking::mcpJsonString("array"));
	array.setVal("items", new Common::JSONValue(item));
	outputProps.setVal("objects", new Common::JSONValue(array));
}

void AsylumMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	props.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking input now the action is over."));
	props.setVal("objects_appeared", Networking::mcpProp("array",
	    "Things on screen that were not there before."));
	props.setVal("objects_gone", Networking::mcpProp("array",
	    "Things that were on screen and are not now."));
}

void AsylumMcpBridge::augmentActSchema(Common::JSONObject &props) {
	props.setVal("verb", Networking::mcpProp("string",
	    "'use' or 'look_at'. This game has no verb bar: those are the two "
	    "mouse buttons."));
}

Common::JSONValue *AsylumMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	return Networking::mcpObjectSchema(props);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void AsylumMcpBridge::snapshotPreAction() {
	_ssePreRoom = roomNumber();
	heroPosition(_ssePreX, _ssePreY);
	_ssePreTargets.clear();
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);
	_sseTrackRoom = _ssePreRoom;
	_sseTrackX = _ssePreX;
	_sseTrackY = _ssePreY;
}

Common::JSONObject AsylumMcpBridge::buildStateChanges() const {
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
	Common::JSONArray appeared, gone;
	for (uint i = 0; i < targets.size(); i++) {
		bool was = false;
		for (uint j = 0; j < _ssePreTargets.size(); j++)
			was = was || _ssePreTargets[j] == targets[i].name;
		if (!was)
			appeared.push_back(mcpJsonString(targets[i].name));
	}
	for (uint j = 0; j < _ssePreTargets.size(); j++) {
		bool still = false;
		for (uint i = 0; i < targets.size(); i++)
			still = still || targets[i].name == _ssePreTargets[j];
		if (!still)
			gone.push_back(mcpJsonString(_ssePreTargets[j]));
	}
	out.setVal("objects_appeared", new Common::JSONValue(appeared));
	out.setVal("objects_gone", new Common::JSONValue(gone));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	return out;
}

bool AsylumMcpBridge::isActionDone() const {
	if (_skipStream) {
		if ((_frameCounter - _sseStartFrame) >= 12)
			return true;
		return g_system != nullptr && (g_system->getMillis() - _sseStartMs) >= kSkipMs;
	}
	return !_pendingClick && playerHasControl();
}

bool AsylumMcpBridge::hasPendingQuestion() const {
	return false;
}

bool AsylumMcpBridge::streamRoomChanged() const {
	return roomNumber() != _ssePreRoom;
}

void AsylumMcpBridge::pumpStreamTrack() {
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

} // End of namespace Asylum
