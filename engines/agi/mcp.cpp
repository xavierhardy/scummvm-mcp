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

#include "agi/mcp.h"
#include "agi/mcp_names.h"

#include "agi/agi.h"
#include "agi/text.h"
#include "agi/words.h"

#include "common/config-manager.h"
#include "common/events.h"
#include "common/system.h"

namespace Agi {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AgiMcpBridge *AgiMcpBridge::create(AgiEngine *vm) {
	AgiMcpBridge *bridge = new AgiMcpBridge(vm);
	bridge->init();
	return bridge;
}

AgiMcpBridge::AgiMcpBridge(AgiEngine *vm) :
	MCP::McpBridge(vm, "scummvm", "1.0"),
	_vm(vm),
	_inStallPump(false),
	_skipStream(false),
	_walkDirection(0),
	_walkUntilFrame(0),
	_ssePreRoom(-1),
	_ssePreX(0), _ssePreY(0),
	_ssePreScore(0),
	_sseTrackRoom(-1), _sseTrackX(0), _sseTrackY(0), _sseTrackScore(0) {
}

AgiMcpBridge::~AgiMcpBridge() {
}

bool AgiMcpBridge::engineReady() const {
	// The bridge is built in the engine's constructor so the port binds before
	// anything can block on startup, which means every one of these can still
	// be null when the first call arrives.
	return _vm != nullptr && _vm->_text != nullptr && _vm->_words != nullptr &&
	       _vm->_game._vm != nullptr;
}

// ---------------------------------------------------------------------------
// Pumping
// ---------------------------------------------------------------------------

void AgiMcpBridge::pump() {
	if (!isEnabled())
		return;

	// A walk is a direction held down for a while: AGI has no "go here", only
	// "he is walking north". Release it when its frames are spent.
	if (_walkDirection != 0 && _frameCounter >= _walkUntilFrame) {
		if (engineReady())
			_vm->setVar(VM_VAR_EGO_DIRECTION, 0);
		_walkDirection = 0;
	}

	MCP::McpBridge::pump();
}

void AgiMcpBridge::pumpFromStall() {
	if (!isEnabled() || _inStallPump)
		return;
	_inStallPump = true;
	pumpTransportOnly();
	_inStallPump = false;
}

// ---------------------------------------------------------------------------
// Reading the game
// ---------------------------------------------------------------------------

int AgiMcpBridge::roomNumber() const {
	if (!engineReady())
		return -1;
	return _vm->getVar(VM_VAR_CURRENT_ROOM);
}

void AgiMcpBridge::egoPosition(int &x, int &y) const {
	x = y = 0;
	if (!engineReady())
		return;
	const ScreenObjEntry &ego = _vm->_game.screenObjTable[SCREENOBJECTS_EGO_ENTRY];
	x = ego.xPos;
	y = ego.yPos;
}

bool AgiMcpBridge::playerHasControl() const {
	if (!engineReady())
		return false;
	// playerControl is the interpreter's own answer to "are the arrow keys
	// live", which is off through every cutscene. cycleInnerLoopActive is the
	// other half: it is set while the interpreter is inside one of its modal
	// waits - a message box wanting a key, the inventory screen, a menu - and
	// is running no cycles at all.
	//
	// The message *window* is deliberately not consulted. It is up through a
	// great deal of ordinary play (the status line and the game's own
	// non-blocking text live in it), so reading it as "blocked" reports a game
	// that is waiting for a command as a game that is not.
	return _vm->_game.playerControl && !_vm->_game.cycleInnerLoopActive;
}

void AgiMcpBridge::collectItems(Common::Array<Item> &out) const {
	if (!engineReady())
		return;
	Common::Array<Common::String> seen;
	for (uint16 i = 0; i < _vm->_game.numObjects; i++) {
		const char *raw = _vm->objectName(i);
		if (raw == nullptr)
			continue;
		const Common::String label(raw);
		if (agiIsPlaceholderItem(label))
			continue;
		Common::String name = agiItemName(label);
		if (name.empty())
			continue;
		uint occurrence = 0;
		for (uint j = 0; j < seen.size(); j++) {
			if (seen[j] == name)
				occurrence++;
		}
		seen.push_back(name);

		Item item;
		item.name = agiDisambiguate(name, occurrence);
		item.label = label;
		item.number = i;
		const int where = _vm->objectGetLocation(i);
		item.carried = (where == EGO_OWNED || where == EGO_OWNED_V1);
		out.push_back(item);
	}
}

void AgiMcpBridge::collectWords(Common::Array<Common::String> &out) const {
	if (!engineReady())
		return;
	_vm->_words->getAllWords(out);
}

// ---------------------------------------------------------------------------
// Typing, which is the whole of this engine's input
// ---------------------------------------------------------------------------

void AgiMcpBridge::typeSentence(const Common::String &sentence) {
	for (uint i = 0; i < sentence.size(); i++) {
		const char c = sentence[i];
		Common::KeyState key;
		key.ascii = (uint16)(byte)c;
		key.keycode = Common::KEYCODE_INVALID;
		injectKey(key);
	}
	Common::KeyState enter(Common::KEYCODE_RETURN, 13);
	injectKey(enter);
}

void AgiMcpBridge::injectKey(const Common::KeyState &ks) {
	// AGI reads its keyboard through the ordinary event queue
	// (processScummVMEvents), so a pushed event is indistinguishable from a
	// real one.
	Common::Event down;
	down.type = Common::EVENT_KEYDOWN;
	down.kbd = ks;
	g_system->getEventManager()->pushEvent(down);
	Common::Event up;
	up.type = Common::EVENT_KEYUP;
	up.kbd = ks;
	g_system->getEventManager()->pushEvent(up);
}

void AgiMcpBridge::injectMouseMove(int, int) {
	// Nothing to do: see the declaration.
}

void AgiMcpBridge::injectMouseClick(int, int, const Common::String &, bool) {
	// Nothing to do: see the declaration.
}

int AgiMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

void AgiMcpBridge::onGameText(const Common::String &text) {
	if (!isEnabled())
		return;
	const Common::String line = agiJoinMessage(text);
	if (line.empty() || agiIsInterfaceLine(line))
		return;
	onSystemLine(line);
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *AgiMcpBridge::toolState(const Common::JSONValue &, Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "state: the game is still starting up";
		return nullptr;
	}
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	out.setVal("room", new Common::JSONValue(room));

	int x = 0, y = 0;
	egoPosition(x, y);
	Common::JSONObject position;
	position.setVal("x", mcpJsonInt(x));
	position.setVal("y", mcpJsonInt(y));
	out.setVal("position", new Common::JSONValue(position));

	out.setVal("score", mcpJsonInt(_vm->getVar(VM_VAR_SCORE)));
	out.setVal("can_act", mcpJsonBool(playerHasControl()));

	// Everything in the OBJECT file, split the way it matters to an agent:
	// what is being carried is what can be used, and what exists but is
	// elsewhere is what can be looked for.
	Common::Array<Item> items;
	collectItems(items);
	Common::JSONArray inventory, objects;
	for (uint i = 0; i < items.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("name", mcpJsonString(items[i].name));
		entry.setVal("label", mcpJsonString(items[i].label));
		if (items[i].carried)
			inventory.push_back(new Common::JSONValue(entry));
		else
			objects.push_back(new Common::JSONValue(entry));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));
	// Named "objects" for the same reason every other bridge does: it is the
	// list of things act() takes names from. Here they are the game's items
	// rather than what is painted on screen, because AGI does not label what
	// is painted on screen at all.
	out.setVal("objects", new Common::JSONValue(objects));

	// The lines said since the last read, cleared by reading.
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

bool AgiMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
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

	Common::String verb = "look at";
	if (args.asObject().contains("verb") && args.asObject()["verb"]->isString()) {
		// The tool's verbs are written with underscores; the parser wants
		// words, and it wants them in its own dictionary.
		verb = args.asObject()["verb"]->asString();
		for (uint i = 0; i < verb.size(); i++) {
			if (verb[i] == '_')
				verb.setChar(' ', i);
		}
	}

	// An item's published name is underscored the same way, and the parser
	// has never heard of an underscore.
	Common::String target = args.asObject()["target1"]->asString();
	for (uint i = 0; i < target.size(); i++) {
		if (target[i] == '_')
			target.setChar(' ', i);
	}

	Common::String sentence = verb + " " + target;
	if (args.asObject().contains("target2") && args.asObject()["target2"]->isString()) {
		Common::String second = args.asObject()["target2"]->asString();
		for (uint i = 0; i < second.size(); i++) {
			if (second[i] == '_')
				second.setChar(' ', i);
		}
		// "give the sandwich to the man" - the preposition is the parser's,
		// and "with" is the other one it takes.
		sentence += " with " + second;
	}

	_skipStream = false;
	typeSentence(sentence);
	beginStream();
	return true;
}

bool AgiMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	// Registered by the base only when usesDialogQuestions() is true, which it
	// is not here; this exists so the class is concrete.
	errorOut = "answer: this game asks its questions in prose - type the answer";
	return false;
}

bool AgiMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
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

	const int wantX = (int)args.asObject()["x"]->asIntegerNumber();
	const int wantY = (int)args.asObject()["y"]->asIntegerNumber();
	int x = 0, y = 0;
	egoPosition(x, y);

	// AGI has no "walk to here": the ego moves while a direction is set and
	// stops when it is cleared. So a walk is the direction of the target, held
	// for a while - which is what holding an arrow key does, and is as close
	// to "walk there" as this engine has. Asking for where he already stands
	// sets no direction at all, and comes back saying so rather than failing:
	// that is how a caller can ask whether the game is taking commands.
	const int dx = wantX - x;
	const int dy = wantY - y;
	// AGI numbers the eight directions clockwise from north, with 0 for
	// "standing still".
	int direction = 0;
	const int slack = 4;
	if (ABS(dx) <= slack && ABS(dy) <= slack) {
		direction = 0;
	} else if (ABS(dx) <= slack) {
		direction = (dy < 0) ? 1 : 5;
	} else if (ABS(dy) <= slack) {
		direction = (dx > 0) ? 3 : 7;
	} else if (dx > 0) {
		direction = (dy < 0) ? 2 : 4;
	} else {
		direction = (dy < 0) ? 8 : 6;
	}

	_vm->setVar(VM_VAR_EGO_DIRECTION, (byte)direction);
	_walkDirection = direction;
	_walkUntilFrame = _frameCounter + kWalkFrames;

	_skipStream = false;
	beginStream();
	return true;
}

bool AgiMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	if (!engineReady()) {
		errorOut = "skip: the game is still starting up";
		return false;
	}
	// Return, and only Return. Every other bridge here sends Escape as well,
	// because in those engines Escape is what cuts a cutscene short - but in
	// AGI it is the menu key, and pressing it pauses the game behind a box
	// reading "Game paused. Press Enter to continue." A skip that sends both
	// therefore ends every call paused, which is the opposite of skipping.
	// Return is what dismisses this engine's message boxes, and a message box
	// is what a skip is for here.
	Common::KeyState enter(Common::KEYCODE_RETURN, 13);
	injectKey(enter);

	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

Common::JSONValue *AgiMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;
	if (!engineReady()) {
		out.setVal("ready", mcpJsonBool(false));
		return new Common::JSONValue(out);
	}
	out.setVal("ready", mcpJsonBool(true));
	out.setVal("room", mcpJsonInt(roomNumber()));
	out.setVal("logic", mcpJsonInt(_vm->_game.curLogicNr));
	out.setVal("player_control", mcpJsonBool(_vm->_game.playerControl));
	out.setVal("message_window", mcpJsonBool(_vm->_text->_messageState.window_Active));
	out.setVal("inner_loop", mcpJsonBool(_vm->_game.cycleInnerLoopActive));
	out.setVal("ego_direction", mcpJsonInt(_vm->getVar(VM_VAR_EGO_DIRECTION)));
	out.setVal("object_count", mcpJsonInt((int)_vm->_game.numObjects));

	Common::Array<Common::String> words;
	collectWords(words);
	out.setVal("word_count", mcpJsonInt((int)words.size()));
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// The vocabulary tool
// ---------------------------------------------------------------------------
//
// The one tool this engine needs that no pointer game does. A parser game is
// only playable if you know what words it was built with: the dictionary is a
// few hundred entries, it is different in every game, and the interpreter's
// answer to a word it has never heard ("I don't know how to do that") is the
// same as its answer to a sensible idea it cannot carry out. An agent that
// cannot tell those apart is guessing blind.

void AgiMcpBridge::registerGameTools() {
	Networking::McpServer::ToolSpec spec;
	spec.name = "vocabulary";
	spec.description =
	    "Every word this game's parser understands. Typing a word that is not "
	    "in this list gets \"I don't know how to do that\", which is also what "
	    "a word it does know gets when the idea is wrong - so read this before "
	    "concluding that an idea failed. Optionally filtered with 'starts_with'.";
	Common::JSONObject props;
	props.setVal("starts_with", Networking::mcpProp("string",
	    "Only words beginning with this, for narrowing a long list."));
	spec.inputSchema = Networking::mcpObjectSchema(props);
	Common::JSONObject outProps;
	outProps.setVal("words", Networking::mcpProp("array", "The words, sorted."));
	outProps.setVal("total", Networking::mcpProp("integer",
	    "How many words the parser knows in all."));
	spec.outputSchema = Networking::mcpObjectSchema(outProps);
	spec.streaming = false;
	_server->registerTool(spec);
}

Common::JSONValue *AgiMcpBridge::callTool(const Common::String &name,
                                          const Common::JSONValue &args,
                                          Common::String &errorOut) {
	if (name == "vocabulary") {
		if (!engineReady()) {
			errorOut = "vocabulary: the game is still starting up";
			return nullptr;
		}
		Common::String prefix;
		if (args.isObject() && args.asObject().contains("starts_with") &&
		    args.asObject()["starts_with"]->isString())
			prefix = args.asObject()["starts_with"]->asString();

		Common::Array<Common::String> words;
		collectWords(words);
		Common::JSONArray listed;
		for (uint i = 0; i < words.size(); i++) {
			if (!prefix.empty() && !words[i].hasPrefix(prefix))
				continue;
			listed.push_back(mcpJsonString(words[i]));
		}
		Common::JSONObject out;
		out.setVal("words", new Common::JSONValue(listed));
		out.setVal("total", mcpJsonInt((int)words.size()));
		return new Common::JSONValue(out);
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String AgiMcpBridge::stateToolDescription() const {
	return "The room as it is now: its number, where the player character "
	       "stands, the score, whether the game is taking commands, what is "
	       "being carried, what other items exist in the game, and every line "
	       "said since the last call (reading them clears them). This game is "
	       "played by typing, so the names here are words to put in a sentence "
	       "rather than things to click.";
}

Common::String AgiMcpBridge::actToolDescription() const {
	return "Say something to the parser, built from a verb and a target: verb "
	       "'look_at' on target 'chest' types \"look at chest\". That is "
	       "exactly what a player does, and it reaches the same parser - so "
	       "the words have to be words the game knows. Read vocabulary() to "
	       "find out which they are, and use type_text where a sentence will "
	       "not fit this shape.";
}

Common::String AgiMcpBridge::walkToolDescription() const {
	return "Walk the player character towards a point. This engine has no "
	       "\"go here\": the character moves while a direction is set, so this "
	       "points him that way and lets go after a moment, and several calls "
	       "may be needed to cross a room. Asking for where he already stands "
	       "sets no direction and comes back saying nothing moved, which is a "
	       "cheap way to ask whether the game is taking commands at all.";
}

Common::String AgiMcpBridge::typeTextToolDescription() const {
	return "Type a line at the parser and press Return. This is how the game "
	       "is played: every action is a sentence, and act() is a convenience "
	       "over this rather than a replacement for it. It is also how a "
	       "question the game asks in prose gets answered.";
}

Common::String AgiMcpBridge::debugToolDescription() const {
	return "Raw interpreter state: the room, the logic script running, "
	       "whether the player has control, whether a message window is up, "
	       "and how many words the parser knows.";
}

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

// An array-of-objects schema, for the parts of the snapshot that are lists of
// things with the same shape.
static Common::JSONValue *itemArraySchema(const char *what) {
	Common::JSONObject props;
	props.setVal("name", Networking::mcpProp("string", "Name to use in act()."));
	props.setVal("label", Networking::mcpProp("string",
	    "What the game's own object file calls it."));
	Common::JSONObject item;
	item.setVal("type", Networking::mcpJsonString("object"));
	item.setVal("properties", new Common::JSONValue(props));
	Common::JSONObject array;
	array.setVal("type", Networking::mcpJsonString("array"));
	array.setVal("description", Networking::mcpJsonString(what));
	array.setVal("items", new Common::JSONValue(item));
	return new Common::JSONValue(array);
}

void AgiMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	outputProps.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking commands right now."));
	outputProps.setVal("score", Networking::mcpProp("integer",
	    "The score the game keeps."));
	outputProps.setVal("inventory", itemArraySchema("What is being carried."));
	outputProps.setVal("objects", itemArraySchema(
	    "Items that exist in the game but are not being carried. This engine "
	    "does not label what is painted on screen, so these are the game's own "
	    "items rather than what is visible here."));
}

void AgiMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	props.setVal("can_act", Networking::mcpProp("boolean",
	    "Whether the game is taking commands now the action is over."));
	props.setVal("score_changed", Networking::mcpProp("integer",
	    "How much the score moved, when it moved."));
	props.setVal("items_gained", Networking::mcpProp("array",
	    "Items picked up while the action ran."));
	props.setVal("items_lost", Networking::mcpProp("array",
	    "Items given away or used up while the action ran."));
}

void AgiMcpBridge::augmentActSchema(Common::JSONObject &props) {
	props.setVal("verb", Networking::mcpProp("string",
	    "The verb, in the parser's own words: look_at, open, take, give, "
	    "talk_to. Underscores become spaces."));
	props.setVal("target2", Networking::mcpProp("string",
	    "A second thing, for a sentence that needs one: \"unlock door with "
	    "key\"."));
}

Common::JSONValue *AgiMcpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	return Networking::mcpObjectSchema(props);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void AgiMcpBridge::snapshotPreAction() {
	_ssePreRoom = roomNumber();
	egoPosition(_ssePreX, _ssePreY);
	_ssePreScore = engineReady() ? _vm->getVar(VM_VAR_SCORE) : 0;
	_ssePreItems.clear();
	Common::Array<Item> items;
	collectItems(items);
	for (uint i = 0; i < items.size(); i++) {
		if (items[i].carried)
			_ssePreItems.push_back(items[i].name);
	}
	_sseTrackRoom = _ssePreRoom;
	_sseTrackX = _ssePreX;
	_sseTrackY = _ssePreY;
	_sseTrackScore = _ssePreScore;
}

Common::JSONObject AgiMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	const int room = roomNumber();
	Common::JSONObject roomObj;
	roomObj.setVal("id", mcpJsonInt(room));
	roomObj.setVal("changed", mcpJsonBool(room != _ssePreRoom));
	out.setVal("room", new Common::JSONValue(roomObj));

	int x = 0, y = 0;
	egoPosition(x, y);
	Common::JSONObject pos;
	pos.setVal("x", mcpJsonInt(x));
	pos.setVal("y", mcpJsonInt(y));
	pos.setVal("changed", mcpJsonBool(x != _ssePreX || y != _ssePreY));
	out.setVal("position", new Common::JSONValue(pos));

	const int score = engineReady() ? _vm->getVar(VM_VAR_SCORE) : 0;
	out.setVal("score_changed", mcpJsonInt(score - _ssePreScore));

	Common::Array<Item> items;
	collectItems(items);
	Common::Array<Common::String> carried;
	for (uint i = 0; i < items.size(); i++) {
		if (items[i].carried)
			carried.push_back(items[i].name);
	}
	Common::JSONArray gained, lost;
	for (uint i = 0; i < carried.size(); i++) {
		bool had = false;
		for (uint j = 0; j < _ssePreItems.size(); j++)
			had = had || _ssePreItems[j] == carried[i];
		if (!had)
			gained.push_back(mcpJsonString(carried[i]));
	}
	for (uint j = 0; j < _ssePreItems.size(); j++) {
		bool still = false;
		for (uint i = 0; i < carried.size(); i++)
			still = still || carried[i] == _ssePreItems[j];
		if (!still)
			lost.push_back(mcpJsonString(_ssePreItems[j]));
	}
	out.setVal("items_gained", new Common::JSONValue(gained));
	out.setVal("items_lost", new Common::JSONValue(lost));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	return out;
}

bool AgiMcpBridge::isActionDone() const {
	if (_skipStream) {
		if ((_frameCounter - _sseStartFrame) >= 12)
			return true;
		return g_system != nullptr && (g_system->getMillis() - _sseStartMs) >= kSkipMs;
	}
	// A walk is over when its held direction has been let go; anything else is
	// over when the interpreter is taking commands again.
	if (_walkDirection != 0)
		return false;
	return playerHasControl();
}

bool AgiMcpBridge::hasPendingQuestion() const {
	return false;
}

bool AgiMcpBridge::streamRoomChanged() const {
	return roomNumber() != _ssePreRoom;
}

void AgiMcpBridge::pumpStreamTrack() {
	// Only a change counts as progress: a condition that simply stays true -
	// control off for the whole of a cutscene - must not keep the deadline
	// alive by itself.
	const int room = roomNumber();
	int x = 0, y = 0;
	egoPosition(x, y);
	const int score = engineReady() ? _vm->getVar(VM_VAR_SCORE) : 0;
	if (room != _sseTrackRoom || x != _sseTrackX || y != _sseTrackY ||
	    score != _sseTrackScore) {
		_sseTrackRoom = room;
		_sseTrackX = x;
		_sseTrackY = y;
		_sseTrackScore = score;
		_sseLastEventFrame = _frameCounter;
	}
}

} // End of namespace Agi
