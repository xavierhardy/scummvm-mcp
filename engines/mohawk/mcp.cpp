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


#include "mohawk/mcp.h"
#include "mohawk/mcp_names.h"

#include "mohawk/cstime.h"
#include "mohawk/cstime_game.h"

#include "common/events.h"
#include "common/system.h"

namespace Mohawk {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

static Common::JSONValue *objectArraySchema(Common::JSONObject &props) {
	Common::JSONObject item;
	item.setVal("type", mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject array;
	array.setVal("type", mcpJsonString("array"));
	array.setVal("items", new Common::JSONValue(item));
	return new Common::JSONValue(array);
}

MohawkMcpBridge *MohawkMcpBridge::create(MohawkEngine_CSTime *vm) {
	MohawkMcpBridge *bridge = new MohawkMcpBridge(vm);
	bridge->init();
	return bridge;
}

MohawkMcpBridge::MohawkMcpBridge(MohawkEngine_CSTime *vm) :
	MCP::McpBridge(vm),
	_vm(vm),
	_pendingClick(false),
	_pendingX(0),
	_pendingY(0),
	_pendingFrame(0),
	_skipStream(false),
	_sseTrackScene(-1) {
}

MohawkMcpBridge::~MohawkMcpBridge() {
}

// ---------------------------------------------------------------------------
// Engine state
// ---------------------------------------------------------------------------

bool MohawkMcpBridge::engineReady() const {
	return _vm != nullptr && _vm->getCase() != nullptr &&
	       _vm->getCase()->getCurrScene() != nullptr;
}

int MohawkMcpBridge::sceneId() const {
	return engineReady() ? (int)_vm->getCase()->getCurrScene()->getId() : -1;
}

bool MohawkMcpBridge::sceneBusy() const {
	// The scene's own answer for "something is playing out": an event running,
	// which is what covers a character speaking, a way out being taken, an
	// animation the player has to wait through.
	return !engineReady() || _vm->getCase()->getCurrScene()->eventIsActive();
}

// ---------------------------------------------------------------------------
// What is in the scene
// ---------------------------------------------------------------------------

void MohawkMcpBridge::collectTargets(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	CSTimeScene *scene = _vm->getCase()->getCurrScene();
	const Common::Array<CSTimeHotspot> &hotspots = scene->getHotspots();
	Common::Array<Common::String> seen;

	for (uint i = 0; i < hotspots.size(); i++) {
		// State 0 is the game's own switch for a region that is not there at
		// the moment - a door that is not a door yet, a thing already taken.
		if (hotspots[i].state == 0)
			continue;
		if (hotspots[i].region._rects.empty())
			continue;

		Common::String name = mohawkLabelToName(
			_vm->getCase()->getRolloverText(hotspots[i].stringId));
		if (name.empty())
			name = mohawkFallbackName("hotspot", (int)i);

		// The middle of the biggest rectangle the region is made of: a region
		// can be several pieces, and the largest is the one a click is most
		// likely to land inside.
		const Common::Rect *biggest = &hotspots[i].region._rects[0];
		for (uint r = 1; r < hotspots[i].region._rects.size(); r++) {
			const Common::Rect &rect = hotspots[i].region._rects[r];
			if (rect.width() * rect.height() > biggest->width() * biggest->height())
				biggest = &rect;
		}

		uint occurrence = 0;
		for (uint s = 0; s < seen.size(); s++) {
			if (seen[s] == name)
				occurrence++;
		}
		seen.push_back(name);

		Target target;
		target.name = mohawkDisambiguate(name, occurrence);
		target.id = (int)i;
		target.x = (biggest->left + biggest->right) / 2;
		target.y = (biggest->top + biggest->bottom) / 2;
		out.push_back(target);
	}
}

bool MohawkMcpBridge::resolveTarget(const Common::String &name, Target &out,
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
	Common::String known;
	for (uint i = 0; i < targets.size(); i++) {
		if (!known.empty())
			known += ", ";
		known += targets[i].name;
	}
	errorOut = Common::String::format(
		"nothing here is called '%s'. In this scene: %s",
		name.c_str(), known.empty() ? "nothing" : known.c_str());
	return false;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *MohawkMcpBridge::callTool(const Common::String &name,
                                             const Common::JSONValue &args,
                                             Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "the game is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

Common::JSONValue *MohawkMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(sceneId()));
	out.setVal("room", new Common::JSONValue(room));
	out.setVal("can_act", mcpJsonBool(!sceneBusy() && !_pendingClick));

	Common::JSONArray objects;
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject o;
		o.setVal("id", mcpJsonInt(targets[i].id));
		o.setVal("name", mcpJsonString(targets[i].name));
		o.setVal("x", mcpJsonInt(targets[i].x));
		o.setVal("y", mcpJsonInt(targets[i].y));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::JSONArray verbs;
	verbs.push_back(mcpJsonString("use"));
	out.setVal("verbs", new Common::JSONValue(verbs));

	Common::JSONArray messages;
	for (uint i = 0; i < _messages.size(); i++) {
		const Common::String text = MCP::mcpCleanGameText(safeUtf8(_messages[i].text));
		if (text.empty())
			continue;
		Common::JSONObject m;
		m.setVal("text", mcpJsonString(text));
		m.setVal("type", mcpJsonString(_messages[i].type));
		const Common::String actor = messageActorName(_messages[i].actorId);
		if (!actor.empty())
			m.setVal("actor", mcpJsonString(actor));
		messages.push_back(new Common::JSONValue(m));
	}
	_messages.clear();
	out.setVal("messages", new Common::JSONValue(messages));

	return new Common::JSONValue(out);
}

bool MohawkMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("target1") ||
	    !args.asObject()["target1"]->isString()) {
		errorOut = "act: a string 'target1' is required";
		return false;
	}
	if (sceneBusy()) {
		errorOut = "act: the game is not accepting input right now";
		return false;
	}
	if (args.asObject().contains("verb") && args.asObject()["verb"]->isString()) {
		const Common::String verb =
			MCP::McpBridge::normalizeActionName(args.asObject()["verb"]->asString());
		if (verb != "use") {
			errorOut = Common::String::format(
				"act: '%s' is not a verb here. This game has no verbs: a thing "
				"is clicked, and what that does is the thing's own business - "
				"a way out is taken, a person is spoken to, something is "
				"picked up.", verb.c_str());
			return false;
		}
	}

	Target target;
	if (!resolveTarget(args.asObject()["target1"]->asString(), target, errorOut)) {
		errorOut = Common::String("act: ") + errorOut;
		return false;
	}

	_skipStream = false;
	pointAndClick(target.x, target.y);
	beginStream();
	return true;
}

bool MohawkMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("x") || !args.asObject().contains("y") ||
	    !args.asObject()["x"]->isIntegerNumber() || !args.asObject()["y"]->isIntegerNumber()) {
		errorOut = "walk: integer 'x' and 'y' are required";
		return false;
	}
	if (sceneBusy()) {
		errorOut = "walk: the game is not accepting input right now";
		return false;
	}
	_skipStream = false;
	pointAndClick((int)args.asObject()["x"]->asIntegerNumber(),
	              (int)args.asObject()["y"]->asIntegerNumber());
	beginStream();
	return true;
}

bool MohawkMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	errorOut = "answer: this game's conversations are not offered as a list";
	return false;
}

bool MohawkMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// A click is what carries this game past a speech bubble; escape is what
	// cuts an animation short. Which is wanted depends on what is on screen,
	// so both are sent.
	injectKey(Common::KeyState(Common::KEYCODE_ESCAPE, 27));
	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

Common::JSONValue *MohawkMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	Common::JSONObject engine;
	engine.setVal("scene", mcpJsonInt(sceneId()));
	engine.setVal("case", mcpJsonInt(engineReady() ? (int)_vm->getCase()->getId() : -1));
	engine.setVal("event_active", mcpJsonBool(sceneBusy()));
	if (engineReady()) {
		engine.setVal("hotspots",
		              mcpJsonInt((int)_vm->getCase()->getCurrScene()->getHotspots().size()));
		engine.setVal("visits",
		              mcpJsonInt((int)_vm->getCase()->getCurrScene()->_visitCount));
	}
	out.setVal("engine", new Common::JSONValue(engine));
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

void MohawkMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void MohawkMcpBridge::moveCursorTo(int x, int y) {
	g_system->warpMouse(x, y);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

void MohawkMcpBridge::injectMouseMove(int x, int y) {
	moveCursorTo(x, y);
}

void MohawkMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
	moveCursorTo(x, y);
	const bool right = (button == "right");
	Common::Event down, up;
	down.type = right ? Common::EVENT_RBUTTONDOWN : Common::EVENT_LBUTTONDOWN;
	up.type   = right ? Common::EVENT_RBUTTONUP   : Common::EVENT_LBUTTONUP;
	down.mouse = up.mouse = Common::Point(x, y);
	const int clicks = isDouble ? 2 : 1;
	for (int i = 0; i < clicks; i++) {
		g_system->getEventManager()->pushEvent(down);
		g_system->getEventManager()->pushEvent(up);
	}
}

void MohawkMcpBridge::pointAndClick(int x, int y) {
	// Point first, press after: the scene resolves a click against the region
	// the cursor is already in, so arriving and pressing at once resolves it
	// against wherever the cursor was before.
	moveCursorTo(x, y);
	_pendingClick = true;
	_pendingX = x;
	_pendingY = y;
	_pendingFrame = _frameCounter;
}

void MohawkMcpBridge::pumpPendingClick() {
	if (!_pendingClick || (_frameCounter - _pendingFrame) < kPointFrames)
		return;
	_pendingClick = false;
	injectMouseClick(_pendingX, _pendingY, "left", false);
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void MohawkMcpBridge::onGameText(const Common::String &text, int charId) {
	if (!isEnabled() || text.empty())
		return;
	int slot = -1;
	for (uint i = 0; i < _messageActors.size(); i++) {
		if (_messageActors[i] == charId)
			slot = (int)i;
	}
	if (slot < 0) {
		_messageActors.push_back(charId);
		slot = (int)_messageActors.size() - 1;
	}
	pushMessage(charId >= 0 ? "speech" : "narration", slot, text);
}

Common::String MohawkMcpBridge::messageActorName(int actorId) const {
	if (actorId < 0 || (uint)actorId >= _messageActors.size())
		return Common::String();
	const int who = _messageActors[actorId];
	if (who < 0)
		return Common::String();
	// The game identifies a speaker by a character number its own case data
	// assigns, and nothing turns that back into a name.
	return Common::String::format("character_%d", who);
}

int MohawkMcpBridge::currentRoomForMessages() const {
	return sceneId();
}

// ---------------------------------------------------------------------------
// Per-frame and streaming
// ---------------------------------------------------------------------------

void MohawkMcpBridge::pumpGame() {
	pumpPendingClick();
}

void MohawkMcpBridge::snapshotPreAction() {
	noteStreamStart();
	_ssePreTargets.clear();
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);
	_sseTrackScene = sceneId();
	_ssePreRoom = _sseTrackScene;
}

Common::JSONObject MohawkMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(sceneId()));
	room.setVal("changed", mcpJsonBool(sceneId() != _ssePreRoom));
	out.setVal("room", new Common::JSONValue(room));

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
	out.setVal("can_act", mcpJsonBool(!sceneBusy() && !_pendingClick));
	return out;
}

bool MohawkMcpBridge::isActionDone() const {
	if (_skipStream)
		return (_frameCounter - _sseStartFrame) >= kSkipFrames;
	return !_pendingClick && !sceneBusy();
}

bool MohawkMcpBridge::hasPendingQuestion() const {
	return false;
}

bool MohawkMcpBridge::streamRoomChanged() const {
	return sceneId() != _ssePreRoom;
}

void MohawkMcpBridge::pumpStreamTrack() {
	const int scene = sceneId();
	if (scene != _sseTrackScene) {
		_sseTrackScene = scene;
		_sseLastEventFrame = _frameCounter;
	}
	if (_sseWorkDoneFrame == 0 && isActionDone())
		_sseWorkDoneFrame = _frameCounter;
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String MohawkMcpBridge::stateToolDescription() const {
	return "The scene as it is now: everything in it that can be pointed at, "
	       "named the way the game names it when the cursor rests on it, with "
	       "where each one is, and every line said since the last call "
	       "(reading them clears them).";
}

Common::String MohawkMcpBridge::actToolDescription() const {
	return "Click something state() named. There are no verbs here: what a "
	       "click does is the thing's own business - a way out is taken, a "
	       "person is spoken to, something is picked up - and what happened "
	       "comes back in the result. " + streamingToolNote();
}

Common::String MohawkMcpBridge::walkToolDescription() const {
	return "Click a point rather than a named thing, in the coordinates "
	       "state() reports positions in. " + streamingToolNote();
}

Common::String MohawkMcpBridge::skipToolDescription() const {
	return "Cut short whatever is playing itself out - an animation, a line "
	       "being spoken - by pressing escape once.";
}

Common::String MohawkMcpBridge::debugToolDescription() const {
	return "Diagnostics: which case and scene are loaded, how many regions "
	       "the scene has, and whether the game is in the middle of something.";
}

Common::JSONValue *MohawkMcpBridge::buildDebugSchema() const {
	Common::JSONObject schema;
	schema.setVal("type", mcpJsonString("object"));
	Common::JSONObject props;
	schema.setVal("properties", new Common::JSONValue(props));
	return new Common::JSONValue(schema);
}

void MohawkMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", Networking::mcpProp("boolean",
	    "False while the game is in the middle of something - an animation, a "
	    "line being spoken. act and walk are refused then."));

	Common::JSONObject obj;
	obj.setVal("id",   Networking::mcpProp("integer", "Identifier in this scene."));
	obj.setVal("name", Networking::mcpProp("string",  "Name to use in act()."));
	obj.setVal("x",    Networking::mcpProp("integer", "Where it is, in screen coordinates."));
	obj.setVal("y",    Networking::mcpProp("integer", "Where it is, in screen coordinates."));
	outputProps.setVal("objects", objectArraySchema(obj));
}

void MohawkMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	Common::JSONObject room;
	room.setVal("type", mcpJsonString("object"));
	Common::JSONObject roomProps;
	roomProps.setVal("id", Networking::mcpProp("integer", "The scene now."));
	roomProps.setVal("changed", Networking::mcpProp("boolean",
	    "Whether this action left the scene it started in."));
	room.setVal("properties", new Common::JSONValue(roomProps));
	props.setVal("room", new Common::JSONValue(room));

	props.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is ready for another action now."));

	Common::JSONObject appeared;
	appeared.setVal("type", mcpJsonString("array"));
	Common::JSONObject name;
	name.setVal("type", mcpJsonString("string"));
	appeared.setVal("items", new Common::JSONValue(name));
	appeared.setVal("description",
	                mcpJsonString("Things in the scene now that were not before."));
	props.setVal("objects_appeared", new Common::JSONValue(appeared));

	Common::JSONObject gone;
	gone.setVal("type", mcpJsonString("array"));
	Common::JSONObject gname;
	gname.setVal("type", mcpJsonString("string"));
	gone.setVal("items", new Common::JSONValue(gname));
	gone.setVal("description",
	            mcpJsonString("Things that were there before and are not now."));
	props.setVal("objects_gone", new Common::JSONValue(gone));
}

} // End of namespace Mohawk
