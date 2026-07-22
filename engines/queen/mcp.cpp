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

#include "queen/mcp.h"

#include "queen/command.h"
#include "queen/graphics.h"
#include "queen/grid.h"
#include "queen/input.h"
#include "queen/logic.h"
#include "queen/queen.h"
#include "queen/state.h"
#include "queen/structs.h"

#include "common/debug.h"
#include "common/events.h"
#include "common/system.h"

namespace Queen {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// The panel verbs, in the game's own Verb order, plus walk_to.
struct VerbName {
	Verb verb;
	const char *name;
};
static const VerbName kVerbs[] = {
	{ VERB_OPEN,    "open" },
	{ VERB_CLOSE,   "close" },
	{ VERB_MOVE,    "move" },
	{ VERB_GIVE,    "give" },
	{ VERB_USE,     "use" },
	{ VERB_PICK_UP, "pick_up" },
	{ VERB_TALK_TO, "talk_to" },
	{ VERB_LOOK_AT, "look_at" },
	{ VERB_WALK_TO, "walk_to" }
};

static Verb verbFromName(const Common::String &name) {
	for (uint i = 0; i < ARRAYSIZE(kVerbs); i++)
		if (name == kVerbs[i].name)
			return kVerbs[i].verb;
	return VERB_NONE;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

QueenMcpBridge::QueenMcpBridge(QueenEngine *vm)
	: MCP::McpBridge(vm),
	  _vm(vm),
	  _ssePreRoomNum(0),
	  _sseActionStarted(false) {
}

QueenMcpBridge::~QueenMcpBridge() {
}

QueenMcpBridge *QueenMcpBridge::create(QueenEngine *vm) {
	QueenMcpBridge *bridge = new QueenMcpBridge(vm);
	bridge->init();
	return bridge;
}

// ---------------------------------------------------------------------------
// Engine state helpers
// ---------------------------------------------------------------------------

// Joe's live position is his bob (sprite slot 0); Logic's _joe.x/y is only a
// staging value for room entry.
static void joeBobPos(QueenEngine *vm, int &x, int &y) {
	BobSlot *joe = vm->graphics()->bob(0);
	x = joe ? joe->x : 0;
	y = joe ? joe->y : 0;
}

// Strip the speech-command codes Talk sentences embed: '*' plus two
// characters (see Talk::getSpeakCommand).
static Common::String stripSpeakCommands(const char *sentence) {
	Common::String out;
	for (const char *p = sentence; *p; p++) {
		if (*p == '*') {
			if (p[1] && p[2])
				p += 2;
			else if (p[1])
				p += 1;
			continue;
		}
		out += *p;
	}
	return out;
}

// A normalized name safe to echo back as a target: normalizeActionName()
// plus everything outside [a-z0-9_] dropped (some authored names carry
// control characters, e.g. "COMEDY BREASTS^").
static Common::String cleanObjectName(const char *raw) {
	Common::String name = MCP::McpBridge::normalizeActionName(Common::String(raw));
	Common::String out;
	for (uint i = 0; i < name.size(); i++) {
		char c = name[i];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
			out += c;
	}
	// Collapse a trailing '_' left by a stripped character.
	while (out.size() && out[out.size() - 1] == '_')
		out.deleteLastChar();
	return out;
}

bool QueenMcpBridge::canAct() const {
	if (_vm->input()->cutawayRunning() || _vm->input()->isDialogueRunning())
		return false;
	if (!_talkOptions.empty())
		return false;
	return _vm->logic()->joeWalk() == JWM_NORMAL;
}

void QueenMcpBridge::collectRoomObjects(Common::Array<RoomObject> &out) const {
	out.clear();
	Logic *logic = _vm->logic();
	uint16 room = logic->currentRoom();
	uint16 roomData = logic->currentRoomData();
	uint16 objMax = _vm->grid()->objMax(room);

	for (int16 rel = 1; rel <= (int16)objMax; rel++) {
		uint16 abs = roomData + rel;
		ObjectData *od = logic->objectData(abs);
		if (!od || od->name <= 0)
			continue; // hidden or deleted
		RoomObject obj;
		obj.relNum = rel;
		obj.absNum = abs;
		obj.name = cleanObjectName(logic->objectName(od->name));
		if (obj.name.empty())
			continue;
		obj.isPerson = (od->image == -3 || od->image == -4);
		obj.isExit = (od->entryObj > 0);
		obj.x = od->x;
		obj.y = od->y;
		out.push_back(obj);
	}

	// De-duplicate names so they resolve unambiguously (crate, crate_2, …).
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

void QueenMcpBridge::collectInventory(Common::Array<uint16> &items,
                                      Common::Array<Common::String> &names) const {
	items.clear();
	names.clear();
	Logic *logic = _vm->logic();
	for (uint16 i = 1; i < logic->itemDataCount(); i++) {
		ItemData *id = logic->itemData(i);
		if (id->name <= 0)
			continue;
		items.push_back(i);
		names.push_back(cleanObjectName(logic->objectName(id->name)));
	}
}

bool QueenMcpBridge::resolveTarget(const Common::String &name, int16 &subject, int16 &relNum,
                                   Common::String &errorOut) const {
	subject = 0;
	relNum = 0;
	// Same cleanup the advertised names went through, so any state name (and
	// even the raw authored form) round-trips.
	Common::String normalized = cleanObjectName(name.c_str());

	Common::Array<RoomObject> objects;
	collectRoomObjects(objects);
	for (uint i = 0; i < objects.size(); i++) {
		if (objects[i].name == normalized) {
			subject = (int16)objects[i].absNum;
			relNum = objects[i].relNum;
			return true;
		}
	}

	Common::Array<uint16> items;
	Common::Array<Common::String> itemNames;
	collectInventory(items, itemNames);
	for (uint i = 0; i < items.size(); i++) {
		if (itemNames[i] == normalized) {
			subject = -(int16)items[i];
			return true;
		}
	}

	Common::String available;
	for (uint i = 0; i < objects.size() && i < 14; i++) {
		if (!available.empty())
			available += ", ";
		available += objects[i].name;
	}
	errorOut = "unknown target '" + name + "'; objects in this room: " + available;
	return false;
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

int QueenMcpBridge::actorId(const Common::String &name) {
	for (uint i = 0; i < _actorNames.size(); i++)
		if (_actorNames[i] == name)
			return (int)i;
	_actorNames.push_back(name);
	return (int)_actorNames.size() - 1;
}

void QueenMcpBridge::onSpeech(const char *actorName, const char *sentence) {
	if (!isEnabled())
		return;
	Common::String text = stripSpeakCommands(sentence);
	if (text.empty())
		return;
	Common::String name = normalizeActionName(Common::String(actorName ? actorName : "JOE"));
	onActorLine(actorId(name), text);
}

void QueenMcpBridge::onTalkOptions(const char options[5][256], int count) {
	(void)count;
	_talkOptions.clear();
	// Slots are 1-based; empty slots are kept so display order (what the
	// digit shortcuts select by) is preserved.
	for (int i = 1; i <= 4; i++)
		_talkOptions.push_back(Common::String(options[i]));
	debug(1, "mcp: talk options published");
}

void QueenMcpBridge::onTalkOptionsDone() {
	_talkOptions.clear();
}

Common::String QueenMcpBridge::messageActorName(int actorId) const {
	if (actorId >= 0 && (uint)actorId < _actorNames.size())
		return _actorNames[actorId];
	return Common::String();
}

int QueenMcpBridge::currentRoomForMessages() const {
	return _vm->logic() ? (int)_vm->logic()->currentRoom() : 0;
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

void QueenMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", mcpProp("boolean",
	    "False while the game is not accepting a new command (cutaway, dialogue). "
	    "act/walk are rejected until it turns true again."));
}

// The non-empty dialogue options as (1-based display id, label) pairs.
static void buildQuestion(const Common::Array<Common::String> &options,
                          Common::JSONObject &question) {
	Common::JSONArray choices;
	int shown = 0;
	for (uint i = 0; i < options.size(); i++) {
		if (options[i].empty())
			continue;
		shown++;
		Common::JSONObject c;
		c.setVal("id", mcpJsonInt(shown));
		c.setVal("label", mcpJsonString(MCP::mcpCleanGameText(
		    stripSpeakCommands(options[i].c_str()))));
		choices.push_back(new Common::JSONValue(c));
	}
	question.setVal("choices", new Common::JSONValue(choices));
}

Common::JSONValue *QueenMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	Logic *logic = _vm->logic();

	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt(logic->currentRoom()));
	const char *roomName = logic->roomName(logic->currentRoom());
	if (roomName && roomName[0])
		roomObj.setVal("name", mcpJsonString(normalizeActionName(Common::String(roomName))));
	out.setVal("room", new Common::JSONValue(roomObj));

	Common::JSONObject pos;
	int jx = 0, jy = 0;
	joeBobPos(_vm, jx, jy);
	pos.setVal("x", mcpJsonInt(jx));
	pos.setVal("y", mcpJsonInt(jy));
	out.setVal("position", new Common::JSONValue(pos));

	out.setVal("can_act", mcpJsonBool(canAct()));

	Common::JSONArray objects;
	Common::Array<RoomObject> roomObjects;
	collectRoomObjects(roomObjects);
	for (uint i = 0; i < roomObjects.size(); i++) {
		const RoomObject &ro = roomObjects[i];
		Common::JSONObject o;
		o.setVal("id",   mcpJsonInt(ro.relNum));
		o.setVal("name", mcpJsonString(ro.name));
		o.setVal("kind", mcpJsonString(ro.isPerson ? "person" : "object"));
		if (ro.isExit)
			o.setVal("pathway", mcpJsonBool(true));
		if (ro.x || ro.y) {
			Common::JSONObject p;
			p.setVal("x", mcpJsonInt(ro.x));
			p.setVal("y", mcpJsonInt(ro.y));
			o.setVal("position", new Common::JSONValue(p));
		}
		ObjectData *od = _vm->logic()->objectData(ro.absNum);
		Verb dflt = State::findDefaultVerb(od->state);
		if (dflt != VERB_NONE) {
			for (uint v = 0; v < ARRAYSIZE(kVerbs); v++)
				if (kVerbs[v].verb == dflt)
					o.setVal("default_verb", mcpJsonString(kVerbs[v].name));
		}
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::JSONArray inventory;
	Common::Array<uint16> items;
	Common::Array<Common::String> itemNames;
	collectInventory(items, itemNames);
	for (uint i = 0; i < items.size(); i++) {
		Common::JSONObject item;
		item.setVal("name", mcpJsonString(itemNames[i]));
		item.setVal("id", mcpJsonInt(items[i]));
		inventory.push_back(new Common::JSONValue(item));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));

	Common::JSONArray verbs;
	for (uint i = 0; i < ARRAYSIZE(kVerbs); i++)
		verbs.push_back(mcpJsonString(kVerbs[i].name));
	out.setVal("verbs", new Common::JSONValue(verbs));

	if (hasPendingQuestion()) {
		Common::JSONObject question;
		buildQuestion(_talkOptions, question);
		out.setVal("question", new Common::JSONValue(question));
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

bool QueenMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
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
	Common::String verbName = normalizeActionName(a["verb"]->asString());
	// interact is accepted as "do the object's default verb, or use".
	Verb verb = verbFromName(verbName == "interact" ? "use" : verbName.c_str());
	if (verb == VERB_NONE) {
		errorOut = "act: unknown verb '" + verbName + "'";
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

	int16 subj1 = 0, subj2 = 0, rel1 = 0, rel2 = 0;
	if (!resolveTarget(name1, subj1, rel1, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}
	if (!name2.empty() && !resolveTarget(name2, subj2, rel2, errorOut)) {
		errorOut = "act: " + errorOut;
		return false;
	}

	// The selected noun is the room-relative number of the primary room
	// object; dialogues are keyed on it (see Command::executeIfDialog).
	int16 noun = rel1 ? rel1 : rel2;
	// Where a click would have landed: the center of the primary object's
	// room zone (most scenery has no authored x/y, and the engine walks Joe
	// to this spot when it does not override it).
	int jx = 0, jy = 0;
	joeBobPos(_vm, jx, jy);
	int16 px = (int16)jx, py = (int16)jy;
	int16 zoneRel = rel1 ? rel1 : rel2;
	if (zoneRel) {
		const Box *b = _vm->grid()->zone(GS_ROOM, (uint16)zoneRel);
		if (b && (b->x2 || b->y2)) {
			px = (b->x1 + b->x2) / 2;
			py = (b->y1 + b->y2) / 2;
		}
	}

	_vm->command()->mcpExecute(verb, subj1, subj2, noun, px, py);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: walk
// ---------------------------------------------------------------------------

bool QueenMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
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

	// An empty command at a room position is exactly a plain click there:
	// executeCurrentAction()'s wrong-action path walks Joe to the spot.
	_vm->command()->mcpExecute(VERB_NONE, 0, 0, 0, x, y);
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

bool QueenMcpBridge::toolAnswer(const Common::JSONValue &args, Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "answer: another action is already in progress";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("id") ||
	    !args.asObject()["id"]->isIntegerNumber()) {
		errorOut = "answer: integer 'id' is required";
		return false;
	}
	if (!hasPendingQuestion()) {
		errorOut = "answer: no dialog question is pending";
		return false;
	}
	int id = (int)args.asObject()["id"]->asIntegerNumber();
	int shown = 0;
	for (uint i = 0; i < _talkOptions.size(); i++)
		if (!_talkOptions[i].empty())
			shown++;
	if (id < 1 || id > shown) {
		errorOut = Common::String::format("answer: 'id' must be between 1 and %d", shown);
		return false;
	}

	// Talk::selectSentence() polls the digit verbs; the digit is the 1-based
	// index among the *displayed* sentences, which is what `id` already is.
	_vm->input()->mcpSetKeyVerb((Verb)(VERB_DIGIT_FIRST + id - 1));
	beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool: skip
// ---------------------------------------------------------------------------

bool QueenMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// Speech skips on VERB_SKIP_TEXT; a running cutaway quits on the same
	// path the Escape key uses.
	_vm->input()->mcpSetKeyVerb(VERB_SKIP_TEXT);
	if (_vm->input()->cutawayRunning())
		_vm->input()->mcpQuitCutaway();
	if (!isStreaming())
		beginStream();
	return true;
}

// ---------------------------------------------------------------------------
// Tool dispatch / gating
// ---------------------------------------------------------------------------

Common::JSONValue *QueenMcpBridge::callTool(const Common::String &name,
                                            const Common::JSONValue &args,
                                            Common::String &errorOut) {
	// The bridge is constructed before QueenEngine::run() builds the
	// subsystems, so a call arriving that early has nothing to read.
	if (!_vm->logic() || !_vm->command() || !_vm->input() || !_vm->grid()) {
		errorOut = name + ": the engine is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// Input injection (debug tools)
// ---------------------------------------------------------------------------

void QueenMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void QueenMcpBridge::injectMouseMove(int x, int y) {
	if (g_system)
		g_system->warpMouse(x, y);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

void QueenMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
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

void QueenMcpBridge::snapshotPreAction() {
	_sseActionStarted = false;
	_ssePreRoomNum = _vm->logic()->currentRoom();
	_ssePreRoom = _ssePreRoomNum;
	collectInventory(_ssePreInventory, _ssePreInventoryNames);
	collectRoomObjects(_ssePreObjects);
	int jx = 0, jy = 0;
	joeBobPos(_vm, jx, jy);
	_ssePrePosX = jx;
	_ssePrePosY = jy;
}

void QueenMcpBridge::pumpStreamTrack() {
	// Latch "the action produced something" the moment Joe moves or speaks, a
	// cutaway or dialogue starts, or the room changes; each also bumps the
	// event frame so the timeout is measured from real activity.
	if (_vm->logic()->joeWalk() != JWM_NORMAL || _vm->input()->cutawayRunning() ||
	    _vm->input()->isDialogueRunning() || !_talkOptions.empty() ||
	    (int)_vm->logic()->currentRoom() != _ssePreRoomNum ||
	    _vm->logic()->newRoom() != 0) {
		if (!_sseActionStarted)
			_sseActionStarted = true;
		_sseLastEventFrame = _frameCounter;
	}
}

bool QueenMcpBridge::streamRoomChanged() const {
	// Cutaways travel through rooms; only report the change once the engine
	// has settled in the new room with the player in control.
	return (int)_vm->logic()->currentRoom() != _ssePreRoomNum &&
	       !_vm->input()->cutawayRunning() &&
	       _vm->logic()->newRoom() == 0 &&
	       _vm->logic()->joeWalk() == JWM_NORMAL;
}

bool QueenMcpBridge::hasPendingQuestion() const {
	for (uint i = 0; i < _talkOptions.size(); i++)
		if (!_talkOptions[i].empty())
			return true;
	return false;
}

bool QueenMcpBridge::isActionDone() const {
	// A pending dialogue chooser is a settled state: the stream closes and
	// reports the question.
	if (hasPendingQuestion())
		return true;
	if (_vm->input()->cutawayRunning() || _vm->input()->isDialogueRunning())
		return false;
	if (_vm->logic()->joeWalk() != JWM_NORMAL)
		return false;
	if (_vm->logic()->newRoom() != 0)
		return false;
	// Give the command a short window to produce its first visible effect
	// before concluding the action was a no-op.
	if (!_sseActionStarted && _frameCounter - _sseStartFrame < 6)
		return false;
	return true;
}

Common::JSONObject QueenMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	Common::Array<uint16> nowItems;
	Common::Array<Common::String> nowNames;
	collectInventory(nowItems, nowNames);
	auto contains = [](const Common::Array<uint16> &arr, uint16 v) -> bool {
		for (uint i = 0; i < arr.size(); i++)
			if (arr[i] == v) return true;
		return false;
	};

	Common::JSONArray added;
	for (uint i = 0; i < nowItems.size(); i++) {
		if (!contains(_ssePreInventory, nowItems[i]))
			added.push_back(mcpJsonString(nowNames[i]));
	}
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	Common::JSONArray removed;
	for (uint i = 0; i < _ssePreInventory.size(); i++) {
		if (!contains(nowItems, _ssePreInventory[i]))
			removed.push_back(mcpJsonString(_ssePreInventoryNames[i]));
	}
	if (!removed.empty())
		changes.setVal("inventory_removed", new Common::JSONValue(removed));

	int room = _vm->logic()->currentRoom();
	if (room != _ssePreRoomNum)
		changes.setVal("room_changed", mcpJsonInt(room));

	int jx = 0, jy = 0;
	joeBobPos(_vm, jx, jy);
	if (jx != _ssePrePosX || jy != _ssePrePosY) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(jx));
		pos.setVal("y", mcpJsonInt(jy));
		changes.setVal("position", new Common::JSONValue(pos));
	}

	// Objects that became visible or vanished (a silent scene change, e.g.
	// moving the curtain revealing the wig).
	{
		Common::Array<RoomObject> nowObjects;
		if ((int)_vm->logic()->currentRoom() == _ssePreRoomNum)
			collectRoomObjects(nowObjects);
		auto hasName = [](const Common::Array<RoomObject> &arr, const Common::String &n) -> bool {
			for (uint i = 0; i < arr.size(); i++)
				if (arr[i].name == n) return true;
			return false;
		};
		Common::JSONArray objChanges;
		if ((int)_vm->logic()->currentRoom() == _ssePreRoomNum) {
			for (uint i = 0; i < nowObjects.size(); i++) {
				if (!hasName(_ssePreObjects, nowObjects[i].name)) {
					Common::JSONObject c;
					c.setVal("name", mcpJsonString(nowObjects[i].name));
					c.setVal("old_state", mcpJsonString("hidden"));
					c.setVal("new_state", mcpJsonString("visible"));
					objChanges.push_back(new Common::JSONValue(c));
				}
			}
			for (uint i = 0; i < _ssePreObjects.size(); i++) {
				if (!hasName(nowObjects, _ssePreObjects[i].name)) {
					Common::JSONObject c;
					c.setVal("name", mcpJsonString(_ssePreObjects[i].name));
					c.setVal("old_state", mcpJsonString("visible"));
					c.setVal("new_state", mcpJsonString("hidden"));
					objChanges.push_back(new Common::JSONValue(c));
				}
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

	if (hasPendingQuestion()) {
		Common::JSONObject question;
		buildQuestion(_talkOptions, question);
		changes.setVal("question", new Common::JSONValue(question));
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Tool: debug
// ---------------------------------------------------------------------------

Common::String QueenMcpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics. Sections are selected by "
	       "flag: 'gamestate' (a slice of the GAMESTATE array, with 'from'/'to'), "
	       "'objects' (the current room's object records), 'items' (all item "
	       "records) and 'system' (the engine's own read-out). Defaults to 'system'.";
}

Common::JSONValue *QueenMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("gamestate", mcpProp("boolean", "Include game state variables."));
	props.setVal("from",      mcpProp("integer", "First game state index (default 0)."));
	props.setVal("to",        mcpProp("integer", "Last game state index, inclusive (default 63)."));
	props.setVal("objects",   mcpProp("boolean", "Include the current room's object records."));
	props.setVal("items",     mcpProp("boolean", "Include every item record."));
	props.setVal("system",    mcpProp("boolean", "Include the engine state summary (default true)."));
	return mcpObjectSchema(props);
}

Common::JSONValue *QueenMcpBridge::toolDebug(const Common::JSONValue &args, Common::String &) {
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
	Logic *logic = _vm->logic();

	if (flag("system", true)) {
		Common::JSONObject sys;
		sys.setVal("room",         mcpJsonInt(logic->currentRoom()));
		sys.setVal("new_room",     mcpJsonInt(logic->newRoom()));
		sys.setVal("old_room",     mcpJsonInt(logic->oldRoom()));
		{
			int jx = 0, jy = 0;
			joeBobPos(_vm, jx, jy);
			sys.setVal("joe_x",    mcpJsonInt(jx));
			sys.setVal("joe_y",    mcpJsonInt(jy));
		}
		sys.setVal("joe_walk",     mcpJsonInt((int)logic->joeWalk()));
		sys.setVal("joe_facing",   mcpJsonInt(logic->joeFacing()));
		sys.setVal("entry_obj",    mcpJsonInt(logic->entryObj()));
		sys.setVal("cutaway",      mcpJsonBool(_vm->input()->cutawayRunning()));
		sys.setVal("dialogue",     mcpJsonBool(_vm->input()->isDialogueRunning()));
		sys.setVal("talk_options", mcpJsonInt((int)_talkOptions.size()));
		sys.setVal("can_act",      mcpJsonBool(canAct()));
		sys.setVal("obj_max",      mcpJsonInt(_vm->grid()->objMax(logic->currentRoom())));
		sys.setVal("room_data",    mcpJsonInt(logic->currentRoomData()));
		out.setVal("system", new Common::JSONValue(sys));
	}

	if (flag("gamestate", false)) {
		int from = MAX(0, number("from", 0));
		int to   = MIN((int)Logic::GAME_STATE_COUNT - 1, number("to", 63));
		Common::JSONArray vars;
		for (int i = from; i <= to; i++) {
			Common::JSONObject v;
			v.setVal("index", mcpJsonInt(i));
			v.setVal("value", mcpJsonInt(logic->gameState(i)));
			vars.push_back(new Common::JSONValue(v));
		}
		out.setVal("gamestate", new Common::JSONValue(vars));
	}

	if (flag("objects", false)) {
		Common::JSONArray objs;
		Common::Array<RoomObject> roomObjects;
		collectRoomObjects(roomObjects);
		for (uint i = 0; i < roomObjects.size(); i++) {
			const RoomObject &ro = roomObjects[i];
			ObjectData *od = logic->objectData(ro.absNum);
			Common::JSONObject o;
			o.setVal("rel",      mcpJsonInt(ro.relNum));
			o.setVal("abs",      mcpJsonInt(ro.absNum));
			o.setVal("name",     mcpJsonString(ro.name));
			o.setVal("x",        mcpJsonInt(od->x));
			o.setVal("y",        mcpJsonInt(od->y));
			o.setVal("state",    mcpJsonInt(od->state));
			o.setVal("image",    mcpJsonInt(od->image));
			o.setVal("entry_obj", mcpJsonInt(od->entryObj));
			o.setVal("description", mcpJsonInt(od->description));
			objs.push_back(new Common::JSONValue(o));
		}
		out.setVal("objects", new Common::JSONValue(objs));
	}

	if (flag("items", false)) {
		Common::JSONArray itemsArr;
		for (uint16 i = 1; i < logic->itemDataCount(); i++) {
			ItemData *id = logic->itemData(i);
			Common::JSONObject o;
			o.setVal("item",  mcpJsonInt(i));
			o.setVal("name_id", mcpJsonInt(id->name));
			if (id->name > 0)
				o.setVal("name", mcpJsonString(normalizeActionName(
				    Common::String(logic->objectName(id->name)))));
			o.setVal("state", mcpJsonInt(id->state));
			itemsArr.push_back(new Common::JSONValue(o));
		}
		out.setVal("items", new Common::JSONValue(itemsArr));
	}

	return new Common::JSONValue(out);
}

} // End of namespace Queen
