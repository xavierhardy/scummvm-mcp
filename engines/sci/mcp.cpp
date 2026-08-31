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


#include "sci/mcp.h"
#include "sci/mcp_names.h"

#include "sci/sci.h"
#include "sci/engine/kernel.h"
#include "sci/engine/object.h"
#include "sci/engine/seg_manager.h"
#include "sci/engine/segment.h"
#include "sci/engine/selector.h"
#include "sci/engine/state.h"
#include "sci/engine/vm.h"

#include "common/events.h"
#include "common/system.h"

namespace Sci {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

SciMcpBridge *SciMcpBridge::create(SciEngine *vm) {
	SciMcpBridge *bridge = new SciMcpBridge(vm);
	bridge->init();
	return bridge;
}

SciMcpBridge::SciMcpBridge(SciEngine *vm) :
	MCP::McpBridge(vm),
	_vm(vm),
	_pendingClick(false),
	_pendingRight(false),
	_pendingX(0),
	_pendingY(0),
	_pendingFrame(0),
	_skipStream(false),
	_ssePreScore(-1),
	_sseTrackRoom(-1),
	_sseTrackPosX(-1),
	_sseTrackPosY(-1),
	_sseTrackScore(-1) {
}

SciMcpBridge::~SciMcpBridge() {
}

// ---------------------------------------------------------------------------
// Engine state helpers
// ---------------------------------------------------------------------------

bool SciMcpBridge::engineReady() const {
	EngineState *s = _vm->getEngineState();
	return s != nullptr && s->_segMan != nullptr && s->variables[VAR_GLOBAL] != nullptr &&
	       s->currentRoomNumber() != 0;
}

reg_t SciMcpBridge::global(int index) const {
	EngineState *s = _vm->getEngineState();
	if (s == nullptr || s->variables[VAR_GLOBAL] == nullptr)
		return NULL_REG;
	return s->variables[VAR_GLOBAL][index];
}

Common::String SciMcpBridge::objectName(reg_t object) const {
	EngineState *s = _vm->getEngineState();
	if (s == nullptr || s->_segMan == nullptr || object.isNull())
		return Common::String();
	if (!s->_segMan->isObject(object))
		return Common::String();
	const char *name = s->_segMan->getObjectName(object);
	// The segment manager answers with a placeholder in angle brackets rather
	// than with nothing when an object carries no readable name, and a
	// placeholder is not a name.
	if (name == nullptr || *name == '\0' || *name == '<')
		return Common::String();
	return Common::String(name);
}

int SciMcpBridge::selector(reg_t object, int selectorId, int missing) const {
	EngineState *s = _vm->getEngineState();
	if (s == nullptr || s->_segMan == nullptr || selectorId < 0)
		return missing;
	const Object *obj = s->_segMan->getObject(object);
	if (obj == nullptr || obj->locateVarSelector(s->_segMan, selectorId) < 0)
		return missing;
	return (int16)readSelectorValue(s->_segMan, object, selectorId);
}

int SciMcpBridge::roomNumber() const {
	EngineState *s = _vm->getEngineState();
	return s != nullptr ? (int)s->currentRoomNumber() : 0;
}

Common::String SciMcpBridge::roomName() const {
	return sciRoomName(objectName(global(kGlobalVarCurrentRoom)));
}

int SciMcpBridge::score() const {
	const reg_t value = global(kGlobalVarScore);
	// A game that keeps no score leaves the global alone, and a pointer there
	// is not a number: only a plain integer is a score.
	if (value.getSegment() != 0)
		return -1;
	return (int16)value.getOffset();
}

bool SciMcpBridge::egoPosition(int &x, int &y) const {
	const reg_t ego = global(kGlobalVarEgo);
	if (ego.isNull())
		return false;
	x = selector(ego, SELECTOR(x), -1);
	y = selector(ego, SELECTOR(y), -1);
	return x >= 0 && y >= 0;
}

bool SciMcpBridge::egoMoving() const {
	EngineState *s = _vm->getEngineState();
	const reg_t ego = global(kGlobalVarEgo);
	if (s == nullptr || s->_segMan == nullptr || ego.isNull())
		return false;
	const Object *obj = s->_segMan->getObject(ego);
	if (obj == nullptr || obj->locateVarSelector(s->_segMan, SELECTOR(mover)) < 0)
		return false;
	// A mover is attached for as long as the character is on its way
	// somewhere, and detached when it arrives.
	return !readSelector(s->_segMan, ego, SELECTOR(mover)).isNull();
}

bool SciMcpBridge::playerHasControl() const {
	return engineReady() && !egoMoving() && !_pendingClick;
}

// ---------------------------------------------------------------------------
// What is in the room
// ---------------------------------------------------------------------------

// The raw list of everything on screen, or a null reference when the game is
// between rooms and there is none.
//
// The global does not hold the same kind of thing in every SCI version. In the
// earliest it is the list itself; from SCI1 on it is a Set - an ordinary
// script object whose `elements` selector holds the list. Both are followed
// here, and neither is followed blindly: the segment manager's own
// lookupList/lookupNode call error() when handed an address that is not the
// kind of thing they expect, and error() stops the engine dead. A bridge
// answering a question must never be able to do that, so every address is
// checked against its segment type before it is looked up.
reg_t SciMcpBridge::castList() const {
	EngineState *s = _vm->getEngineState();
	if (s == nullptr || s->_segMan == nullptr)
		return NULL_REG;
	reg_t cast = global(kGlobalVarCast);
	if (cast.isNull())
		return NULL_REG;
	if (s->_segMan->getSegmentType(cast.getSegment()) == SEG_TYPE_LISTS)
		return cast;
	const Object *object = s->_segMan->getObject(cast);
	if (object == nullptr || object->locateVarSelector(s->_segMan, SELECTOR(elements)) < 0)
		return NULL_REG;
	cast = readSelector(s->_segMan, cast, SELECTOR(elements));
	if (cast.isNull() || s->_segMan->getSegmentType(cast.getSegment()) != SEG_TYPE_LISTS)
		return NULL_REG;
	return cast;
}

void SciMcpBridge::collectTargets(Common::Array<Target> &out) const {
	EngineState *s = _vm->getEngineState();
	if (s == nullptr || s->_segMan == nullptr)
		return;
	const reg_t castReg = castList();
	if (castReg.isNull())
		return;
	List *cast = s->_segMan->lookupList(castReg);
	if (cast == nullptr)
		return;

	Common::Array<Common::String> seen;
	reg_t nodeReg = cast->first;
	// The cast is a linked list the interpreter walks every cycle; walking it
	// with a bound rather than to its end keeps a corrupt list from hanging
	// the server along with the game.
	for (int guard = 0; guard < 512 && !nodeReg.isNull(); guard++) {
		if (s->_segMan->getSegmentType(nodeReg.getSegment()) != SEG_TYPE_NODES)
			break;
		const Node *node = s->_segMan->lookupNode(nodeReg, false);
		if (node == nullptr)
			break;
		const reg_t object = node->value;
		nodeReg = node->succ;
		if (!s->_segMan->isObject(object))
			continue;

		const Common::String script = objectName(object);
		if (sciIsInternalName(script))
			continue;
		Common::String name = sciObjectName(script);
		if (name.empty())
			continue;

		Target target;
		target.object = object;
		target.bounds = Common::Rect(selector(object, SELECTOR(nsLeft), 0),
		                             selector(object, SELECTOR(nsTop), 0),
		                             selector(object, SELECTOR(nsRight), 0),
		                             selector(object, SELECTOR(nsBottom), 0));
		target.x = selector(object, SELECTOR(x), -1);
		target.y = selector(object, SELECTOR(y), -1);
		// A view's own x/y is its anchor, which for a standing figure is at
		// its feet. Clicking there is clicking the floor in front of it, so
		// aim at the middle of what is drawn whenever the game says what that
		// is.
		if (target.bounds.isValidRect() && !target.bounds.isEmpty()) {
			target.x = (target.bounds.left + target.bounds.right) / 2;
			target.y = (target.bounds.top + target.bounds.bottom) / 2;
		}
		if (target.x < 0 || target.y < 0)
			continue;

		uint occurrence = 0;
		for (uint i = 0; i < seen.size(); i++) {
			if (seen[i] == name)
				occurrence++;
		}
		seen.push_back(name);
		target.name = sciDisambiguate(name, occurrence);
		out.push_back(target);
	}
}

bool SciMcpBridge::resolveTarget(const Common::String &name, Target &out,
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
		"nothing here is called '%s'. In this room: %s",
		name.c_str(), known.empty() ? "nothing" : known.c_str());
	return false;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *SciMcpBridge::callTool(const Common::String &name,
                                          const Common::JSONValue &args,
                                          Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "the game is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

Common::JSONValue *SciMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	const Common::String name = roomName();
	if (!name.empty())
		room.setVal("name", mcpJsonString(name));
	out.setVal("room", new Common::JSONValue(room));

	int px = 0, py = 0;
	if (egoPosition(px, py)) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(px));
		pos.setVal("y", mcpJsonInt(py));
		out.setVal("position", new Common::JSONValue(pos));
	}

	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	const int points = score();
	if (points >= 0)
		out.setVal("score", mcpJsonInt(points));

	Common::JSONArray objects;
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject o;
		o.setVal("name", mcpJsonString(targets[i].name));
		o.setVal("x", mcpJsonInt(targets[i].x));
		o.setVal("y", mcpJsonInt(targets[i].y));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	// The lines said since the last read, cleared after reading: without this
	// an agent could never see what was said while can_act was false.
	Common::JSONArray messages;
	for (uint i = 0; i < _messages.size(); i++) {
		const Common::String text = MCP::mcpCleanGameText(safeUtf8(_messages[i].text));
		if (text.empty())
			continue;
		Common::JSONObject m;
		m.setVal("text", mcpJsonString(text));
		const Common::String actor = messageActorName(_messages[i].actorId);
		if (!actor.empty())
			m.setVal("actor", mcpJsonString(actor));
		m.setVal("type", mcpJsonString(_messages[i].type));
		messages.push_back(new Common::JSONValue(m));
	}
	_messages.clear();
	out.setVal("messages", new Common::JSONValue(messages));

	return new Common::JSONValue(out);
}

bool SciMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "act: another action is already in progress";
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

	// One cursor, two buttons: the left one does whatever the thing is for,
	// the right one looks at it. The verb chooses the button and nothing else,
	// so an agent is told plainly which two words mean anything here.
	bool right = false;
	if (verb == "look_at") {
		right = true;
	} else if (verb != "use" && verb != "walk_to" && verb != "talk_to") {
		errorOut = Common::String::format(
			"act: '%s' is not a verb here. This game has one cursor and two "
			"buttons: 'use' does whatever the thing is for, 'look_at' looks "
			"at it.", verb.c_str());
		return false;
	}

	_skipStream = false;
	pointAndClick(target.x, target.y, right);
	beginStream();
	return true;
}

bool SciMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "walk: another action is already in progress";
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

	_skipStream = false;
	pointAndClick(x, y, false);
	beginStream();
	return true;
}

bool SciMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	// Registered only when the game asks questions; nothing here does yet.
	errorOut = "answer: this game does not put a list of things to say to the "
	           "player";
	return false;
}

bool SciMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// Escape is what a player presses to cut a sequence short, so that is what
	// this sends: one press, and what it did is reported after a short window
	// rather than waited on, because the sequence it ends may be several
	// rooms long.
	Common::KeyState escape(Common::KEYCODE_ESCAPE, 27);
	injectKey(escape);

	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

Common::JSONValue *SciMcpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject globals;
	// The handful the interpreter itself gives names to. Everything past them
	// is game-specific and means nothing without that game's source.
	static const struct { const char *name; int index; } kNamed[] = {
		{ "ego",           kGlobalVarEgo },
		{ "game",          kGlobalVarGame },
		{ "current_room",  kGlobalVarCurrentRoom },
		{ "quit",          kGlobalVarQuit },
		{ "cast",          kGlobalVarCast },
		{ "room_number",   kGlobalVarCurrentRoomNo },
		{ "previous_room", kGlobalVarPreviousRoomNo },
		{ "new_room",      kGlobalVarNewRoomNo },
		{ "score",         kGlobalVarScore },
		{ "user",          kGlobalVarUser }
	};
	for (uint i = 0; i < ARRAYSIZE(kNamed); i++) {
		const reg_t value = global(kNamed[i].index);
		Common::JSONObject entry;
		entry.setVal("index", mcpJsonInt(kNamed[i].index));
		entry.setVal("value", mcpJsonInt((int16)value.getOffset()));
		if (value.getSegment() != 0) {
			entry.setVal("segment", mcpJsonInt(value.getSegment()));
			const Common::String named = objectName(value);
			if (!named.empty())
				entry.setVal("object", mcpJsonString(named));
		}
		globals.setVal(kNamed[i].name, new Common::JSONValue(entry));
	}
	out.setVal("globals", new Common::JSONValue(globals));

	// Any global by number, for a game whose own source says what it holds.
	if (args.isObject() && args.asObject().contains("globals") &&
	    args.asObject()["globals"]->isArray()) {
		const Common::JSONArray &wanted = args.asObject()["globals"]->asArray();
		Common::JSONObject asked;
		for (uint i = 0; i < wanted.size(); i++) {
			if (!wanted[i]->isIntegerNumber())
				continue;
			const int index = (int)wanted[i]->asIntegerNumber();
			if (index < 0 || index >= 1000)
				continue;
			asked.setVal(Common::String::format("%d", index),
			             mcpJsonInt((int16)global(index).getOffset()));
		}
		out.setVal("asked", new Common::JSONValue(asked));
	}

	Common::JSONObject engine;
	engine.setVal("sci_version", mcpJsonInt((int)getSciVersion()));
	engine.setVal("game_id", mcpJsonString(_vm->getGameIdStr()));
	engine.setVal("has_parser", mcpJsonBool(_vm->hasParser()));
	engine.setVal("is_demo", mcpJsonBool(_vm->isDemo()));
	out.setVal("engine", new Common::JSONValue(engine));

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

void SciMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void SciMcpBridge::moveCursorTo(int x, int y) {
	g_system->warpMouse(x, y);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

void SciMcpBridge::injectMouseMove(int x, int y) {
	moveCursorTo(x, y);
}

void SciMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
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

void SciMcpBridge::pointAndClick(int x, int y, bool rightButton) {
	// Point first, click after: the game hit-tests a click against where the
	// pointer already is, so arriving and pressing in the same cycle resolves
	// the press against wherever the pointer was before.
	moveCursorTo(x, y);
	_pendingClick = true;
	_pendingRight = rightButton;
	_pendingX = x;
	_pendingY = y;
	_pendingFrame = _frameCounter;
}

void SciMcpBridge::pumpPendingClick() {
	if (!_pendingClick || (_frameCounter - _pendingFrame) < kPointFrames)
		return;
	_pendingClick = false;
	injectMouseClick(_pendingX, _pendingY, _pendingRight ? "right" : "left", false);
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

void SciMcpBridge::onGameText(const Common::String &text, int talkerId) {
	if (!isEnabled() || text.empty())
		return;
	int slot = -1;
	for (uint i = 0; i < _messageActors.size(); i++) {
		if (_messageActors[i] == talkerId) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0) {
		_messageActors.push_back(talkerId);
		slot = (int)_messageActors.size() - 1;
	}
	pushMessage(talkerId >= 0 ? "speech" : "narration", slot, text);
}

Common::String SciMcpBridge::messageActorName(int actorId) const {
	if (actorId < 0 || (uint)actorId >= _messageActors.size())
		return Common::String();
	const int talker = _messageActors[actorId];
	if (talker < 0)
		return Common::String();
	// SCI identifies a speaker by a talker number the game's own scripts
	// assign, and nothing in the data turns that back into a name.
	return Common::String::format("talker_%d", talker);
}

int SciMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

// ---------------------------------------------------------------------------
// Per-frame and streaming
// ---------------------------------------------------------------------------

void SciMcpBridge::pumpGame() {
	pumpPendingClick();
}

void SciMcpBridge::snapshotPreAction() {
	noteStreamStart();
	_ssePreScore = score();
	_ssePreTargets.clear();
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);

	_sseTrackRoom = roomNumber();
	_sseTrackScore = _ssePreScore;
	egoPosition(_sseTrackPosX, _sseTrackPosY);
	_ssePreRoom = _sseTrackRoom;
	_ssePrePosX = _sseTrackPosX;
	_ssePrePosY = _sseTrackPosY;
}

Common::JSONObject SciMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	const Common::String name = roomName();
	if (!name.empty())
		room.setVal("name", mcpJsonString(name));
	room.setVal("changed", mcpJsonBool(roomNumber() != _ssePreRoom));
	out.setVal("room", new Common::JSONValue(room));

	int px = 0, py = 0;
	if (egoPosition(px, py)) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(px));
		pos.setVal("y", mcpJsonInt(py));
		pos.setVal("changed", mcpJsonBool(px != _ssePrePosX || py != _ssePrePosY));
		out.setVal("position", new Common::JSONValue(pos));
	}

	const int points = score();
	if (points >= 0 && points != _ssePreScore) {
		Common::JSONObject scored;
		scored.setVal("from", mcpJsonInt(_ssePreScore));
		scored.setVal("to", mcpJsonInt(points));
		out.setVal("score", new Common::JSONValue(scored));
	}

	// What is in the room now that was not before, and the other way round.
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

bool SciMcpBridge::isActionDone() const {
	if (_skipStream)
		return (_frameCounter - _sseStartFrame) >= kSkipFrames;
	return !_pendingClick && playerHasControl();
}

bool SciMcpBridge::hasPendingQuestion() const {
	return false;
}

bool SciMcpBridge::streamRoomChanged() const {
	return roomNumber() != _ssePreRoom;
}

void SciMcpBridge::pumpStreamTrack() {
	// Only a change counts as progress. A condition that simply stays true —
	// control off for the whole of a cutscene — must not keep the deadline
	// alive by itself.
	const int room = roomNumber();
	int px = 0, py = 0;
	egoPosition(px, py);
	const int points = score();
	if (room != _sseTrackRoom || px != _sseTrackPosX || py != _sseTrackPosY ||
	    points != _sseTrackScore) {
		_sseTrackRoom = room;
		_sseTrackPosX = px;
		_sseTrackPosY = py;
		_sseTrackScore = points;
		_sseLastEventFrame = _frameCounter;
	}
	if (_sseWorkDoneFrame == 0 && isActionDone())
		_sseWorkDoneFrame = _frameCounter;
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String SciMcpBridge::stateToolDescription() const {
	return "The room as it is now: its number and the name its script carries, "
	       "where the player character stands, the score if the game keeps one, "
	       "everything in the room that can be named, and every line said since "
	       "the last call (reading them clears them). Read this before every "
	       "action: the names it lists are the names act() takes.";
}

Common::String SciMcpBridge::actToolDescription() const {
	return "Act on something state() named. One cursor, two buttons: "
	       "verb='use' does whatever the thing is for, verb='look_at' looks at "
	       "it. target1 is the name from state(). " + streamingToolNote();
}

Common::String SciMcpBridge::walkToolDescription() const {
	return "Go to a point on the floor, in the coordinates state() reports "
	       "positions in. A point covered by something is not open floor: the "
	       "game will act on what is there instead, so aim at ground. " +
	       streamingToolNote();
}

Common::String SciMcpBridge::skipToolDescription() const {
	return "Cut short whatever is playing itself out - an opening sequence, a "
	       "line being spoken - by pressing escape once. What one press did is "
	       "reported after a short window rather than waited on, because the "
	       "sequence it ends may run on for several rooms.";
}

Common::String SciMcpBridge::debugToolDescription() const {
	return "Diagnostics: the interpreter's own named global variables, any "
	       "other globals asked for by number, and what the engine says about "
	       "the running game.";
}

Common::JSONValue *SciMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	Common::JSONObject globals;
	globals.setVal("type", mcpJsonString("array"));
	Common::JSONObject items;
	items.setVal("type", mcpJsonString("integer"));
	globals.setVal("items", new Common::JSONValue(items));
	globals.setVal("description",
	               mcpJsonString("Global variables to read, by number."));
	props.setVal("globals", new Common::JSONValue(globals));

	Common::JSONObject schema;
	schema.setVal("type", mcpJsonString("object"));
	schema.setVal("properties", new Common::JSONValue(props));
	return new Common::JSONValue(schema);
}

void SciMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	Common::JSONObject score;
	score.setVal("type", mcpJsonString("integer"));
	score.setVal("description",
	             mcpJsonString("The score the game keeps, absent when it keeps none."));
	outputProps.setVal("score", new Common::JSONValue(score));
}

void SciMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	Common::JSONObject appeared;
	appeared.setVal("type", mcpJsonString("array"));
	Common::JSONObject name;
	name.setVal("type", mcpJsonString("string"));
	appeared.setVal("items", new Common::JSONValue(name));
	appeared.setVal("description",
	                mcpJsonString("Things in the room now that were not before."));
	props.setVal("objects_appeared", new Common::JSONValue(appeared));

	Common::JSONObject gone;
	gone.setVal("type", mcpJsonString("array"));
	Common::JSONObject gname;
	gname.setVal("type", mcpJsonString("string"));
	gone.setVal("items", new Common::JSONValue(gname));
	gone.setVal("description",
	            mcpJsonString("Things that were in the room before and are not now."));
	props.setVal("objects_gone", new Common::JSONValue(gone));
}

} // End of namespace Sci
