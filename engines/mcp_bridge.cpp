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

#include "engines/mcp_bridge.h"

#include "common/base64.h"
#include "common/config-manager.h"
#include "common/debug.h"
#include "common/memstream.h"
#include "common/system.h"

#include "engines/engine.h"

#include "graphics/palette.h"
#include "graphics/paletteman.h"
#include "graphics/surface.h"

#include "image/png.h"

namespace MCP {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpPropOneOf;
using Networking::mcpObjectSchema;
using Networking::mcpLowerTrimmed;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

McpBridge::McpBridge(Engine *engine, const Common::String &serverName,
                     const Common::String &serverVersion)
	: _engine(engine),
	  _enabled(false),
	  _skipToolEnabled(false),
	  _debugToolsEnabled(false),
	  _server(nullptr),
	  _nextMessageSeq(1),
	  _frameCounter(0),
	  _streaming(false),
	  _sseStartFrame(0),
	  _sseStartMs(0),
	  _sseDoneAtFrame(0),
	  _sseStuckAtFrame(0),
	  _sseLastEventFrame(0),
	  _sseWorkDoneFrame(0),
	  _ssePreRoom(0),
	  _ssePrePosX(0),
	  _ssePrePosY(0),
	  _debugButtonReleaseFrame(0) {
	if (!_engine) return;

	_enabled = ConfMan.getBool("mcp");
	if (!_enabled) return;

	_skipToolEnabled = ConfMan.hasKey("mcp_skip_tool") && ConfMan.getBool("mcp_skip_tool");
	_debugToolsEnabled = ConfMan.hasKey("mcp_debug") && ConfMan.getBool("mcp_debug");

	int port = ConfMan.hasKey("mcp_port") ? ConfMan.getInt("mcp_port") : 23456;
	Common::String host = ConfMan.hasKey("mcp_host") ? ConfMan.get("mcp_host") : "127.0.0.1";
	int maxSessions = ConfMan.hasKey("mcp_max_sessions") ? ConfMan.getInt("mcp_max_sessions") : 4;
	if (maxSessions < 1) maxSessions = 1;
	_server = new Networking::McpServer(port, serverName, serverVersion, host, maxSessions);
	if (!_server->isListening()) {
		delete _server;
		_server = nullptr;
		_enabled = false;
		return;
	}
	_server->setToolHandler(this);
}

McpBridge::~McpBridge() {
	delete _server;
}

void McpBridge::init() {
	// registerTools() dispatches through virtual hooks, so it must run after the
	// object is fully constructed — never from a base constructor.
	if (_server)
		registerTools();
}

// ---------------------------------------------------------------------------
// Game-loop hooks
// ---------------------------------------------------------------------------

void McpBridge::pump() {
	if (!_enabled || !_server) return;
	++_frameCounter;
	// If the SSE client vanished mid-action the server abandons the stream
	// without telling us; drop our half too or every later action is rejected
	// with "another action is already in progress".
	if (_streaming && !_server->isStreaming()) {
		debug(1, "mcp: stream abandoned by client, resetting");
		_streaming = false;
	}
	pumpGame();
	_server->pump();
}

void McpBridge::pumpTransportOnly() {
	if (!_enabled || !_server) return;
	_server->pump();
}

// ---------------------------------------------------------------------------
// Message capture
// ---------------------------------------------------------------------------

void McpBridge::pushMessage(const char *type, int actorId, const Common::String &text) {
	if (!_enabled || text.empty()) return;
	MessageEntry m;
	m.seq = _nextMessageSeq++;
	m.frame = _frameCounter;
	m.room = currentRoomForMessages();
	m.actorId = actorId;
	m.type = type;
	m.text = text;
	_messages.push_back(m);
	const uint kMaxMessages = 512;
	if (_messages.size() > kMaxMessages)
		_messages.remove_at(0);
}

void McpBridge::onActorLine(int actorId, const Common::String &text) {
	if (stripTalkieMetadata()) {
		// Spoken lines arrive straight from the engine's charset buffer, prefixed
		// by one or more 0xFF-coded 4-byte talkie/sound blocks
		// (0xFF <code> <id-lo> <id-hi>). Strip them, then drop the fragments that
		// hold only embedded sound codes with no readable text — voice-only
		// reaction cues, which would otherwise surface as garbage "messages".
		const byte *p = (const byte *)text.c_str();
		const byte *end = p + text.size();
		while (p + 3 < end && *p == 0xFF)
			p += 4;
		Common::String line((const char *)p);
		// Require at least two ASCII letters: a real spoken line is a word, while
		// a leftover sound-code fragment (e.g. 0xFF 0x0A 'L') carries at most a
		// stray byte that happens to fall in the letter range.
		int letters = 0;
		for (uint i = 0; i < line.size(); ++i) {
			byte c = (byte)line[i];
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
				++letters;
		}
		if (letters < 2)
			return;
		pushMessage("actor", actorId, line);
		return;
	}
	pushMessage("actor", actorId, text);
}

void McpBridge::onSystemLine(const Common::String &text) {
	pushMessage("system", -1, text);
}

void McpBridge::onDialogPrompt(const Common::String &text) {
	pushMessage("dialog", -1, text);
}

void McpBridge::emitPendingMessages() {
	while (!_messages.empty()) {
		const MessageEntry &m = _messages[0];
		_sseMessages.push_back(m);
		_sseLastEventFrame = _frameCounter;
		Common::JSONObject params;
		if (m.actorId >= 0) {
			Common::String actorName = messageActorName(m.actorId);
			if (!actorName.empty()) {
				Common::String safe = safeUtf8(mcpLowerTrimmed(actorName));
				params.setVal("actor", mcpJsonString(safe));
			}
		}
		Common::String cleanText = mcpCleanGameText(safeUtf8(m.text));
		if (!cleanText.empty()) {
			params.setVal("text", mcpJsonString(cleanText));
			params.setVal("type", mcpJsonString(safeUtf8(m.type)));
			_server->emitNotification(params);
		}
		_messages.remove_at(0);
	}
}

Common::String McpBridge::safeUtf8(const Common::String &raw) const {
	if (raw.empty()) return raw;
	return mcpStripNamePadding(Networking::mcpNormalizeSpaces(Networking::mcpSanitizeString(raw)));
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

Common::JSONValue *McpBridge::buildChangesSchema() const {
	Common::JSONObject props;
	props.setVal("inventory_added",   mcpProp("array",   "Names of items added to inventory"));
	props.setVal("inventory_removed", mcpProp("array",   "Names of items removed from inventory"));
	props.setVal("room_changed",      mcpProp("integer", "New room number (only present if room changed)"));
	Common::JSONObject posProps;
	posProps.setVal("x", mcpProp("integer", "X coordinate"));
	posProps.setVal("y", mcpProp("integer", "Y coordinate"));
	Common::JSONObject posSchema;
	posSchema.setVal("type",       mcpJsonString("object"));
	posSchema.setVal("properties", new Common::JSONValue(posProps));
	props.setVal("position",        new Common::JSONValue(posSchema));
	props.setVal("objects_changed", mcpProp("array",  "Objects whose state changed: [{name, old_state, new_state}]"));
	props.setVal("messages",        mcpProp("array",  "Dialog/narration lines spoken during the action: [{text, actor?}]"));
	props.setVal("question",        mcpProp("object", "Pending dialog question if action ended with one: {choices:[{id,label}]}"));
	// Engine/game-specific additions (e.g. CMI's boats_remaining).
	const_cast<McpBridge *>(this)->augmentChangesSchema(props);
	return mcpObjectSchema(props);
}

Common::String McpBridge::stateToolDescription() const {
	return "Returns the current game state: the room, where the player character "
	       "stands, what is in the room, what is carried, the verbs that can be "
	       "used, the lines said since the last read (cleared by reading them) "
	       "and any question waiting to be answered. The player character itself "
	       "is not listed among the room's objects. Everything act() accepts as a "
	       "target appears here first, under the name given here.";
}

Common::String McpBridge::streamingToolNote() const {
	return "Blocks until the action and anything it sets off have played out, "
	       "streaming the lines said meanwhile, then returns what changed. Only "
	       "one action runs at a time: wait for the previous one to return before "
	       "sending the next.";
}

Common::String McpBridge::actToolDescription() const {
	Common::String desc =
	    "Perform a verb on one or two targets, named as state shows them. Use "
	    "'walk' for a plain point on the floor. ";
	desc += streamingToolNote();
	if (usesDialogQuestions())
		desc += " Refused while a question is waiting — answer it first.";
	return desc;
}

Common::String McpBridge::answerToolDescription() const {
	return "Answer the question the game is asking, by the 1-based index of the "
	       "choice in state.question.choices. " + streamingToolNote() +
	       " Refused when no question is waiting.";
}

Common::String McpBridge::walkToolDescription() const {
	return "Send the player character to explicit (x, y) coordinates in the "
	       "current room, in the coordinate space state reports positions in. "
	       "Out-of-bounds values are clamped to the room. To go to something "
	       "named, use act with a walk verb and that name instead. " +
	       streamingToolNote();
}

Common::String McpBridge::skipToolDescription() const {
	return "Cut short whatever is playing — an intro, a cutscene, a long "
	       "animation — the way pressing Escape does. Returns what changed. "
	       "Rejected when there is nothing to skip, which is how to tell that "
	       "the game is waiting for input again.";
}

Common::String McpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics.";
}

Common::JSONValue *McpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("from", mcpProp("integer", "First script variable index to return."));
	props.setVal("to",   mcpProp("integer", "Last script variable index to return (inclusive)."));
	return mcpObjectSchema(props);
}

Common::JSONValue *McpBridge::buildDebugOutputSchema() const {
	// Deliberately open: this is a read-out of whatever the game keeps, and
	// which fields it keeps differs per game. What it does promise is that the
	// answer is an object of named sections.
	Common::JSONObject schema;
	schema.setVal("type", mcpJsonString("object"));
	schema.setVal("description", mcpJsonString(
	    "Named sections of raw game state; which ones are present depends on "
	    "the flags passed in."));
	return new Common::JSONValue(schema);
}

void McpBridge::registerTools() {
	// --- state ---
	{
		Common::JSONObject inputProps;
		Common::JSONObject outputProps;
		Common::JSONObject roomProps;
		roomProps.setVal("id", mcpProp("integer", "Current room ID"));
		roomProps.setVal("name", mcpProp("string", "Human-readable room name (optional)"));
		Common::JSONObject roomSchema;
		roomSchema.setVal("type", mcpJsonString("object"));
		roomSchema.setVal("properties", new Common::JSONValue(roomProps));
		outputProps.setVal("room", new Common::JSONValue(roomSchema));

		Common::JSONObject positionProps;
		positionProps.setVal("x", mcpProp("integer", "X coordinate"));
		positionProps.setVal("y", mcpProp("integer", "Y coordinate"));
		Common::JSONObject positionSchema;
		positionSchema.setVal("type", mcpJsonString("object"));
		positionSchema.setVal("properties", new Common::JSONValue(positionProps));
		outputProps.setVal("position", new Common::JSONValue(positionSchema));

		auto makeStringArray = []() -> Common::JSONValue * {
			Common::JSONObject arr;
			arr.setVal("type",  mcpJsonString("array"));
			arr.setVal("items", mcpProp("string"));
			return new Common::JSONValue(arr);
		};
		outputProps.setVal("verbs",     makeStringArray());
		outputProps.setVal("inventory", makeStringArray());

		// Game-specific extra state fields (e.g. Maniac Mansion's controlled kid).
		augmentStateSchema(outputProps);

		Common::JSONObject objectItemProps;
		objectItemProps.setVal("id",              mcpProp("integer", "Object ID"));
		objectItemProps.setVal("name",            mcpProp("string",  "Object name"));
		objectItemProps.setVal("state",           mcpProp("integer", "Object state"));
		objectItemProps.setVal("state_name",      mcpProp("string",  "Human-readable state (e.g. a door's 'opened'/'closed'); omitted when the object has no named state"));
		objectItemProps.setVal("x",               mcpProp("integer", "X coordinate"));
		objectItemProps.setVal("y",               mcpProp("integer", "Y coordinate"));
		objectItemProps.setVal("pathway",         mcpProp("boolean", "Is pathway/exit"));
		objectItemProps.setVal("compatible_verbs",mcpProp("array",   "Verbs that have script handlers for this object"));
		Common::JSONObject objectItem;
		objectItem.setVal("type",       mcpJsonString("object"));
		objectItem.setVal("properties", new Common::JSONValue(objectItemProps));
		Common::JSONObject objectsArray;
		objectsArray.setVal("type",  mcpJsonString("array"));
		objectsArray.setVal("items", new Common::JSONValue(objectItem));
		outputProps.setVal("objects", new Common::JSONValue(objectsArray));

		Common::JSONObject msgItemProps;
		msgItemProps.setVal("text",  mcpProp("string", "Message text"));
		msgItemProps.setVal("actor", mcpProp("string", "Actor name (optional)"));
		Common::JSONObject msgItem;
		msgItem.setVal("type",       mcpJsonString("object"));
		msgItem.setVal("properties", new Common::JSONValue(msgItemProps));
		Common::JSONObject messagesArray;
		messagesArray.setVal("type",  mcpJsonString("array"));
		messagesArray.setVal("items", new Common::JSONValue(msgItem));
		outputProps.setVal("messages", new Common::JSONValue(messagesArray));

		Common::JSONObject choiceItemProps;
		choiceItemProps.setVal("id",    mcpProp("integer", "1-based choice index"));
		choiceItemProps.setVal("label", mcpProp("string",  "Choice text"));
		Common::JSONObject choiceItem;
		choiceItem.setVal("type",       mcpJsonString("object"));
		choiceItem.setVal("properties", new Common::JSONValue(choiceItemProps));
		Common::JSONObject choicesArray;
		choicesArray.setVal("type",  mcpJsonString("array"));
		choicesArray.setVal("items", new Common::JSONValue(choiceItem));
		Common::JSONObject questionProps;
		questionProps.setVal("choices", new Common::JSONValue(choicesArray));
		Common::JSONObject questionSchema;
		questionSchema.setVal("type",       mcpJsonString("object"));
		questionSchema.setVal("properties", new Common::JSONValue(questionProps));
		outputProps.setVal("question", new Common::JSONValue(questionSchema));

		Networking::McpServer::ToolSpec spec;
		spec.name = "state";
		spec.description = stateToolDescription();
		spec.inputSchema  = mcpObjectSchema(inputProps);
		spec.outputSchema = mcpObjectSchema(outputProps);
		spec.streaming    = false;
		_server->registerTool(spec);
	}

	// --- act ---
	{
		Common::JSONObject props;
		props.setVal("verb",    mcpProp("string", "Verb name (e.g. 'open', 'use'). Required."));
		props.setVal("target1", mcpPropOneOf("string", "integer",
		    "Primary target: name or numeric id of an object/inventory item "
		    "currently present in state (objects[] or inventory[]). "
		    "NPCs appear in objects[] and can be targeted by name. "
		    "For 'use X on Y', this is X."));
		props.setVal("target2", mcpPropOneOf("string", "integer",
		    "Secondary target for 'use X on Y' (Y): name or numeric id, "
		    "must currently exist in state."));
		// Game-specific arguments (e.g. coordinates, for a game whose targets
		// are points on screen rather than named things).
		augmentActSchema(props);
		const char *req[] = {"verb"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "act";
		spec.description = actToolDescription();
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- answer (only for games that ask questions) ---
	if (usesDialogQuestions()) {
		Common::JSONObject props;
		props.setVal("id", mcpProp("integer", "1-indexed dialog choice (1 = first option shown in state.question.choices)."));
		const char *req[] = {"id"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "answer";
		spec.description = answerToolDescription();
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- walk ---
	{
		Common::JSONObject props;
		props.setVal("x", mcpProp("integer", "Target X pixel coordinate (auto-clamped to room bounds)"));
		props.setVal("y", mcpProp("integer", "Target Y pixel coordinate (auto-clamped to room bounds)"));
		const char *req[] = {"x", "y"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "walk";
		spec.description = walkToolDescription();
		spec.inputSchema  = mcpObjectSchema(props, req, 2);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- skip ---
	if (_skipToolEnabled) {
		Networking::McpServer::ToolSpec spec;
		spec.name = "skip";
		spec.description = skipToolDescription();
		spec.inputSchema  = nullptr;  // No input required
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- type_text ---
	// Only for a game that ever asks for something to be typed. It is not a
	// debug tool: a screen that stops and waits for a line is part of playing
	// the game, and an agent that can only point at things cannot get past
	// one.
	if (usesTypedInput()) {
		Networking::McpServer::ToolSpec spec;
		spec.name = "type_text";
		spec.description = typeTextToolDescription();
		Common::JSONObject props;
		props.setVal("text", mcpProp("string",
		    "The line to type. Typed one character at a time, as at a keyboard."));
		props.setVal("enter", mcpProp("boolean",
		    "Press Return afterwards (default true), which is what submits a line."));
		const char *req[] = {"text"};
		spec.inputSchema = mcpObjectSchema(props, req, 1);
		Common::JSONObject outProps;
		outProps.setVal("text", mcpProp("string", "What was typed."));
		outProps.setVal("enter", mcpProp("boolean", "Whether Return followed it."));
		spec.outputSchema = mcpObjectSchema(outProps);
		spec.streaming = false;
		_server->registerTool(spec);
	}

	// Game-specific tools (registered only for the games that provide them).
	registerGameTools();

	// --- debug tools (gated by mcp_debug ini option) ---
	if (_debugToolsEnabled) {
		// debug — return raw engine state for diagnostics
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "debug";
			spec.description = debugToolDescription();
			spec.inputSchema  = buildDebugSchema();
			spec.outputSchema = buildDebugOutputSchema();
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// keystroke — inject a key event
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "keystroke";
			spec.description =
			    "Press a key, as if on the keyboard: the next frame reads it. For "
			    "anything the tools above do not cover — menus, shortcuts, a "
			    "prompt waiting for a keypress.";
			Common::JSONObject props;
			props.setVal("key", mcpProp("string",
			    "Key to press: a single ASCII character ('a', 'C', '1'), or a name "
			    "('Escape', 'Return', 'Space', 'Tab', 'Backspace', 'F1'..'F12', "
			    "'Up', 'Down', 'Left', 'Right')."));
			props.setVal("ctrl",  mcpProp("boolean", "Hold Ctrl modifier (default false)."));
			props.setVal("shift", mcpProp("boolean", "Hold Shift modifier (default false)."));
			props.setVal("alt",   mcpProp("boolean", "Hold Alt modifier (default false)."));
			const char *req[] = {"key"};
			spec.inputSchema  = mcpObjectSchema(props, req, 1);
			Common::JSONObject outProps;
			outProps.setVal("key", mcpProp("string", "The key that was pressed."));
			spec.outputSchema = mcpObjectSchema(outProps);
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// mouse_move — set the virtual mouse position
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "mouse_move";
			spec.description =
			    "Move the cursor to (x, y), in the coordinate space state reports "
			    "positions in, without clicking. Some games react to what the "
			    "cursor is merely pointing at.";
			Common::JSONObject props;
			props.setVal("x", mcpProp("integer", "X coordinate."));
			props.setVal("y", mcpProp("integer", "Y coordinate."));
			const char *req[] = {"x", "y"};
			spec.inputSchema  = mcpObjectSchema(props, req, 2);
			Common::JSONObject outProps;
			outProps.setVal("x", mcpProp("integer", "Where the cursor was put."));
			outProps.setVal("y", mcpProp("integer", "Where the cursor was put."));
			spec.outputSchema = mcpObjectSchema(outProps);
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// mouse_click — simulate a mouse click at (x, y)
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "mouse_click";
			spec.description =
			    "Click at (x, y) exactly as a player would, which reaches anything "
			    "the named tools cannot: a control the game draws itself, a spot "
			    "no object covers. Returns as soon as the click is sent — read "
			    "state (or take a screenshot) to see what came of it.";
			Common::JSONObject props;
			props.setVal("x", mcpProp("integer", "X coordinate."));
			props.setVal("y", mcpProp("integer", "Y coordinate."));
			props.setVal("button", mcpProp("string",
			    "Mouse button: 'left' (default), 'right', or 'middle'."));
			props.setVal("double", mcpProp("boolean",
			    "True for a double click (two clicks within ~250ms). Default false."));
			const char *req[] = {"x", "y"};
			spec.inputSchema  = mcpObjectSchema(props, req, 2);
			Common::JSONObject outProps;
			outProps.setVal("x",      mcpProp("integer", "Where the click was sent."));
			outProps.setVal("y",      mcpProp("integer", "Where the click was sent."));
			outProps.setVal("button", mcpProp("string",  "Which button was used."));
			outProps.setVal("double", mcpProp("boolean", "Whether it was a double click."));
			spec.outputSchema = mcpObjectSchema(outProps);
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// screenshot — return the current frame (and file it on disk)
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "screenshot";
			spec.description =
			    "Return the current frame as a PNG image to look at, and by "
			    "default also write it to the configured screenshot folder. Shows "
			    "exactly what a player would see, which is the way to check "
			    "anything the state snapshot does not describe.";
			Common::JSONObject props;
			props.setVal("save_to_disk", mcpProp("boolean",
			    "Also write the frame to the screenshot folder (default true)."));
			spec.inputSchema  = mcpObjectSchema(props);
			Common::JSONObject outProps;
			outProps.setVal("width",  mcpProp("integer", "Frame width in pixels."));
			outProps.setVal("height", mcpProp("integer", "Frame height in pixels."));
			outProps.setVal("saved",  mcpProp("boolean",
			    "Whether the frame was written to the screenshot folder."));
			spec.outputSchema = mcpObjectSchema(outProps);
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// save_state — write the current game to a save slot
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "save_state";
			spec.description =
			    "Save the game to a slot, the same way its own save menu does, so "
			    "the state can be returned to later. Says whether the save was "
			    "accepted: a game refuses to save in the middle of some scenes.";
			Common::JSONObject props;
			props.setVal("slot", mcpProp("integer",
			    "Save slot index to write."));
			props.setVal("description", mcpProp("string",
			    "Optional human-readable label stored in the save header "
			    "(default \"mcp save\")."));
			const char *req[] = {"slot"};
			spec.inputSchema  = mcpObjectSchema(props, req, 1);
			Common::JSONObject outProps;
			outProps.setVal("slot",   mcpProp("integer", "The slot that was written."));
			outProps.setVal("saved",  mcpProp("boolean", "Whether the game was saved."));
			outProps.setVal("reason", mcpProp("string",
			    "Why the save was refused, when it was."));
			outProps.setVal("description", mcpProp("string",
			    "The label stored in the save header."));
			spec.outputSchema = mcpObjectSchema(outProps);
			spec.streaming    = false;
			_server->registerTool(spec);
		}

		// Engine-specific debug tools (e.g. SCUMM's set_talk_speed).
		registerDebugTools();
	}
}

// ---------------------------------------------------------------------------
// Tool dispatch
// ---------------------------------------------------------------------------

Common::JSONValue *McpBridge::callTool(const Common::String &name,
                                       const Common::JSONValue &args,
                                       Common::String &errorOut) {
	if (!_engine) {
		errorOut = "No game loaded";
		return nullptr;
	}
	if (name == "state")
		return toolState(args, errorOut);
	if (name == "act") {
		if (!toolAct(args, errorOut)) return nullptr;
		return nullptr; // streaming started
	}
	if (name == "answer") {
		if (!toolAnswer(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "walk") {
		if (!toolWalk(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "skip") {
		if (!toolSkip(args, errorOut)) return nullptr;
		return nullptr;
	}
	if (name == "debug")        return toolDebug(args, errorOut);
	// The three input-injection tools answer with what they injected: there is
	// nothing else to report, and echoing it is what makes their result match
	// the schema they advertise.
	if (name == "keystroke")    {
		if (!toolKeystroke(args, errorOut)) return nullptr;
		Common::JSONObject out;
		out.setVal("key", mcpJsonString(args.asObject()["key"]->asString()));
		return new Common::JSONValue(out);
	}
	if (name == "mouse_move")   {
		if (!toolMouseMove(args, errorOut)) return nullptr;
		const Common::JSONObject &a = args.asObject();
		Common::JSONObject out;
		out.setVal("x", mcpJsonInt((int)a["x"]->asIntegerNumber()));
		out.setVal("y", mcpJsonInt((int)a["y"]->asIntegerNumber()));
		return new Common::JSONValue(out);
	}
	if (name == "mouse_click")  {
		if (!toolMouseClick(args, errorOut)) return nullptr;
		const Common::JSONObject &a = args.asObject();
		Common::JSONObject out;
		out.setVal("x", mcpJsonInt((int)a["x"]->asIntegerNumber()));
		out.setVal("y", mcpJsonInt((int)a["y"]->asIntegerNumber()));
		out.setVal("button", mcpJsonString(
		    a.contains("button") && a["button"]->isString() ? a["button"]->asString() : "left"));
		out.setVal("double", mcpJsonBool(
		    a.contains("double") && a["double"]->isBool() && a["double"]->asBool()));
		return new Common::JSONValue(out);
	}
	if (name == "type_text")    return toolTypeText(args, errorOut);
	if (name == "screenshot")   return toolScreenshot(args, errorOut);
	if (name == "save_state")   return toolSaveState(args, errorOut);
	// Game-specific tools (shoot_cannon, …) handled by the leaf class.
	bool handled = false;
	Common::JSONValue *gameResult = dispatchGameTool(name, args, errorOut, handled);
	if (handled)
		return gameResult;
	errorOut = "Unknown tool: " + name;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Shared debug tools
// ---------------------------------------------------------------------------

Common::String McpBridge::typeTextToolDescription() const {
	return "Type a line, for the places this game stops and waits for one "
	       "rather than for something to be pointed at. Each character is sent "
	       "as its own keypress, and Return follows unless you say otherwise.";
}

Common::JSONValue *McpBridge::toolTypeText(const Common::JSONValue &args,
                                           Common::String &errorOut) {
	if (!args.isObject() || !args.asObject().contains("text") ||
	    !args.asObject()["text"]->isString()) {
		errorOut = "type_text: a string 'text' is required";
		return nullptr;
	}
	const Common::String text = args.asObject()["text"]->asString();
	// A cap rather than no bound at all: this drives a 1990s parser, and a
	// megabyte of keypresses is a mistake being made, not a sentence.
	if (text.size() > 256) {
		errorOut = "type_text: 'text' is longer than anything these games read (256)";
		return nullptr;
	}
	const bool enter = !args.asObject().contains("enter") ||
	                   !args.asObject()["enter"]->isBool() ||
	                   args.asObject()["enter"]->asBool();

	for (uint i = 0; i < text.size(); i++) {
		const byte c = (byte)text[i];
		// Printable ASCII only. Anything else has no keypress that means it,
		// and inventing one would type something the agent did not ask for.
		if (c < 0x20 || c > 0x7e) {
			errorOut = Common::String::format(
				"type_text: '%c' is not a character these games can be sent", text[i]);
			return nullptr;
		}
		injectKey(Common::KeyState(Common::KEYCODE_INVALID, c));
	}
	if (enter)
		injectKey(Common::KeyState(Common::KEYCODE_RETURN, 13));

	Common::JSONObject out;
	out.setVal("text", mcpJsonString(text));
	out.setVal("enter", mcpJsonBool(enter));
	return new Common::JSONValue(out);
}

bool McpBridge::toolKeystroke(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject()) {
		errorOut = "keystroke: arguments must be an object with a 'key' field";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("key") || !a["key"]->isString()) {
		errorOut = "keystroke: 'key' string is required";
		return false;
	}
	bool ctrl  = a.contains("ctrl")  && a["ctrl"]->isBool()  && a["ctrl"]->asBool();
	bool shift = a.contains("shift") && a["shift"]->isBool() && a["shift"]->asBool();
	bool alt   = a.contains("alt")   && a["alt"]->isBool()   && a["alt"]->asBool();

	Common::KeyState ks;
	if (!mcpJsonKeyToKeyState(a["key"]->asString(), ctrl, shift, alt, ks)) {
		errorOut = "keystroke: unknown key '" + a["key"]->asString() + "'";
		return false;
	}
	injectKey(ks);
	return true;
}

bool McpBridge::toolMouseMove(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject()) { errorOut = "mouse_move: arguments must be an object"; return false; }
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "mouse_move: integer 'x' and 'y' are required";
		return false;
	}
	injectMouseMove((int)a["x"]->asIntegerNumber(), (int)a["y"]->asIntegerNumber());
	return true;
}

bool McpBridge::toolMouseClick(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject()) { errorOut = "mouse_click: arguments must be an object"; return false; }
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("x") || !a["x"]->isIntegerNumber() ||
	    !a.contains("y") || !a["y"]->isIntegerNumber()) {
		errorOut = "mouse_click: integer 'x' and 'y' are required";
		return false;
	}
	Common::String button = "left";
	if (a.contains("button") && a["button"]->isString()) button = a["button"]->asString();
	bool isDouble = a.contains("double") && a["double"]->isBool() && a["double"]->asBool();
	injectMouseClick((int)a["x"]->asIntegerNumber(), (int)a["y"]->asIntegerNumber(),
	                 button, isDouble);
	return true;
}

Common::JSONValue *McpBridge::toolScreenshot(const Common::JSONValue &args, Common::String &errorOut) {
	if (!g_system) {
		errorOut = "screenshot: no system available";
		return nullptr;
	}
	const bool wantFile = !args.isObject() || !args.asObject().contains("save_to_disk") ||
	                      !args.asObject()["save_to_disk"]->isBool() ||
	                      args.asObject()["save_to_disk"]->asBool();

	Common::JSONObject out;

	// Save to the configured screenshot path, auto-numbered, exactly like the
	// in-app "save screenshot" action. Engine-agnostic.
	if (wantFile) {
		g_system->saveScreenshot();
		out.setVal("saved", mcpJsonBool(true));
	}

	// The picture itself, so the caller can simply look at the game.
	Graphics::Surface *screen = g_system->lockScreen();
	if (!screen) {
		errorOut = "screenshot: the screen is not readable right now";
		return nullptr;
	}
	out.setVal("width",  mcpJsonInt(screen->w));
	out.setVal("height", mcpJsonInt(screen->h));

	byte palette[256 * 3] = {};
	if (screen->format.isCLUT8() && g_system->getPaletteManager())
		g_system->getPaletteManager()->grabPalette(palette, 0, 256);
	// Flatten to plain RGB first: a paletted frame carries its colours out of
	// band, and every client can read RGB.
	Graphics::Surface *rgb =
	    screen->convertTo(Graphics::PixelFormat::createFormatRGB24(), palette, 256);
	g_system->unlockScreen();
	if (!rgb) {
		errorOut = "screenshot: could not read the frame";
		return nullptr;
	}

	Common::MemoryWriteStreamDynamic png(DisposeAfterUse::YES);
	const bool encoded = Image::writePNG(png, *rgb);
	rgb->free();
	delete rgb;
	if (!encoded) {
		errorOut = "screenshot: could not encode the frame";
		return nullptr;
	}
	out.setVal(Networking::kMcpImageKey,
	           mcpJsonString(Common::b64EncodeData(png.getData(), png.size())));

	return new Common::JSONValue(out);
}

Common::JSONValue *McpBridge::toolSaveState(const Common::JSONValue &args, Common::String &errorOut) {
	if (!args.isObject() || !args.asObject().contains("slot") ||
	    !args.asObject()["slot"]->isIntegerNumber()) {
		errorOut = "save_state: missing integer 'slot'";
		return nullptr;
	}
	int slot = (int)args.asObject()["slot"]->asIntegerNumber();
	if (slot < 0) {
		errorOut = "save_state: 'slot' must be >= 0";
		return nullptr;
	}
	Common::String desc = "mcp save";
	if (args.asObject().contains("description") && args.asObject()["description"]->isString())
		desc = args.asObject()["description"]->asString();

	Common::JSONObject out;
	out.setVal("slot", mcpJsonInt(slot));

	// Refuse when the engine itself would refuse (e.g. mid-cutscene or with a
	// modal panel open).
	if (!_engine->canSaveGameStateCurrently(nullptr)) {
		out.setVal("saved", mcpJsonBool(false));
		out.setVal("reason", mcpJsonString("game cannot be saved in the current state"));
		return new Common::JSONValue(out);
	}

	Common::Error err = _engine->saveGameState(slot, desc, false);
	if (err.getCode() != Common::kNoError) {
		out.setVal("saved", mcpJsonBool(false));
		out.setVal("reason", mcpJsonString(err.getDesc()));
		return new Common::JSONValue(out);
	}
	debug(1, "mcp: save_state wrote slot %d ('%s')", slot, desc.c_str());
	out.setVal("saved", mcpJsonBool(true));
	out.setVal("description", mcpJsonString(desc));
	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------

void McpBridge::noteStreamStart() {
	_sseWorkDoneFrame = 0;
}

void McpBridge::beginStream() {
	noteStreamStart();
	snapshotPreAction();
	_streaming = true;
	_sseStartFrame = _frameCounter;
	_sseStartMs = g_system ? g_system->getMillis() : 0;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseMessages.clear();
	_server->startStreaming();
}

void McpBridge::closeStreamSuccess() {
	Common::JSONObject changes = buildStateChanges();
	_streaming = false;
	_server->endStream(new Common::JSONValue(changes), true);
}

void McpBridge::closeStreamFailure(const Common::String &reason) {
	_streaming = false;
	_server->endStream(nullptr, false, reason);
}

bool McpBridge::shouldCloseStream() const {
	if (hasPendingQuestion())
		return true;
	return _frameCounter - _sseDoneAtFrame >= settleFrames();
}

void McpBridge::pumpStream() {
	if (!_streaming) return;

	emitPendingMessages();

	// Engine-specific early step: may end the frame before the generic
	// settle/close logic runs (e.g. an action running inside its own inner loop).
	if (pumpStreamGameEarly())
		return;

	// Per-frame engine step, run before the close/timeout checks so its effects
	// (e.g. resetting the settle window) are seen this frame.
	pumpStreamGame();

	// Progress tracking: whatever the engine watches to decide that something is
	// still happening. Implementations bump _sseLastEventFrame.
	pumpStreamTrack();

	// Early-close: if the room has already changed, there is nothing left to
	// settle — no dialogue will appear in the old room and accessing old-room
	// state is unsafe.
	if (streamRoomChanged()) {
		debug(1, "mcp: room changed during stream, closing immediately");
		closeStreamSuccess();
		return;
	}

	// Early-exit: stuck (engine frozen with no visible progress). Use a short
	// budget when no events have occurred yet (the action had no visible effect
	// and completed quickly), and a longer one once activity has been seen.
	if (isStreamStuck()) {
		if (_sseStuckAtFrame == 0) _sseStuckAtFrame = _frameCounter;
		uint32 stuckLimit = stuckFrames(streamHadActivity());
		if (_frameCounter - _sseStuckAtFrame > stuckLimit) {
			debug(1, "mcp: action stuck for %d frames — closing stream", stuckLimit);
			closeStreamSuccess();
			return;
		}
	} else {
		_sseStuckAtFrame = 0;
	}

	// Hard timeout, measured from streamTimeoutAnchor(), plus an optional
	// absolute ceiling and an optional wall-clock ceiling (the latter for
	// engines whose frame counter can stop advancing entirely).
	{
		uint32 absLimit = absoluteTimeoutFrames();
		bool absoluteTimeout = absLimit != 0 && (_frameCounter - _sseStartFrame > absLimit);
		uint32 msLimit = wallClockTimeoutMs();
		bool wallClockTimeout = msLimit != 0 && g_system &&
		                        (g_system->getMillis() - _sseStartMs > msLimit);
		if (absoluteTimeout || wallClockTimeout ||
		    _frameCounter - streamTimeoutAnchor() > timeoutFrames()) {
			debug(1, "mcp: stream timeout (anchor=%u, start=%u, last=%u, now=%u)",
			      streamTimeoutAnchor(), _sseStartFrame, _sseLastEventFrame, _frameCounter);
			closeStreamFailure("action timed out");
			return;
		}
	}

	// Mid step: bookkeeping that must run after the timeout checks (e.g.
	// releasing a simulated mouse button).
	pumpStreamMid();

	// Late engine step: deferred synthetic clicks that must run after the
	// button-clear pass above so their freshly-set button state survives.
	pumpStreamGameLate();

	// Last engine step before the settle decision (multi-frame click machines).
	pumpStreamPreSettle();

	// See wallClockCloseMs(): when cycles have stopped, the frame gates below
	// can never be met and real time is what is left to judge by.
	const uint32 closeMs = wallClockCloseMs();
	const bool framesStopped = closeMs != 0 && g_system != nullptr &&
	                           (g_system->getMillis() - _sseStartMs) >= closeMs;

	if (!framesStopped && _frameCounter - _sseStartFrame < minStreamFrames())
		return;

	if (!isActionDone()) {
		_sseDoneAtFrame = 0;
		return;
	}

	if (_sseDoneAtFrame == 0) {
		_sseDoneAtFrame = _frameCounter;
		debug(1, "mcp: action looks done at frame %d, settling (lastEvent=%d)",
		      _frameCounter, _sseLastEventFrame);
	}

	// If a new message arrived after we first thought we were done, the action
	// script was still running — reset the window to wait for it to finish.
	// Not once the post-action speech allowance is spent, though: a room that
	// talks to itself (Zak's living-room TV) would otherwise extend the window
	// until the stream fails as timed out.
	if (_sseLastEventFrame > _sseDoneAtFrame) {
		if (postActionSpeechExpired()) {
			debug(1, "mcp: ignoring ambient chatter at frame %d", _sseLastEventFrame);
		} else {
			debug(1, "mcp: new event at frame %d after done at %d, extending window",
			      _sseLastEventFrame, _sseDoneAtFrame);
			_sseDoneAtFrame = _sseLastEventFrame;
		}
	}

	if (shouldCloseStream() || framesStopped) {
		debug(1, "mcp: closing stream at frame %d", _frameCounter);
		closeStreamSuccess();
	}
}

} // End of namespace MCP
