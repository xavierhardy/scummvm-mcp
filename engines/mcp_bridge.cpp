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

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/system.h"

#include "engines/engine.h"

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
	_server = new Networking::McpServer(port, serverName, serverVersion, host);
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

Common::String McpBridge::debugToolDescription() const {
	return "Return raw engine state for diagnostics.";
}

Common::JSONValue *McpBridge::buildDebugSchema() const {
	Common::JSONObject props;
	props.setVal("from", mcpProp("integer", "First script variable index to return."));
	props.setVal("to",   mcpProp("integer", "Last script variable index to return (inclusive)."));
	return mcpObjectSchema(props);
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
		spec.description =
		    "Returns the current game state: room, position, inventory, scene objects "
		    "(including NPCs with their compatible_verbs — always includes talk_to), "
		    "active verbs, latest messages (cleared after reading), "
		    "and pending dialog question if any. The player character is never listed. "
		    "Objects with a meaningful state expose a human-readable 'state_name' "
		    "(e.g. a door reads 'opened'/'closed'); doors advertise the 'open'/'close' verbs. "
		    "Use act(verb='talk_to', target1=<npc_name>) to speak to an NPC.";
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
		const char *req[] = {"verb"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "act";
		spec.description =
		    "Perform a verb action on one or two named targets. Blocks until the "
		    "action/cutscene sequence completes, streaming dialog and events via SSE, "
		    "then returns state changes. For walking to specific coordinates, use 'walk'. "
		    "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		    "Wait for the previous act/answer/walk call to complete before sending the next one. "
		    "Fails if a question is pending (use 'answer' first) or another action is running.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- answer ---
	{
		Common::JSONObject props;
		props.setVal("id", mcpProp("integer", "1-indexed dialog choice (1 = first option shown in state.question.choices)."));
		const char *req[] = {"id"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "answer";
		spec.description =
		    "Select a dialog choice by 1-based index. Blocks until the conversation "
		    "sequence completes, streaming events via SSE, then returns state changes. "
		    "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		    "Wait for the previous act/answer/walk call to complete before sending the next one. "
		    "Fails if no question is currently pending or another action is running.";
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
		spec.description =
		    "Walk ego to explicit (x, y) pixel coordinates in the current room. "
		    "Out-of-bounds values are automatically clamped to the room bounds. "
		    "Use 'act' with verb='walk_to' and target1=<name> to walk to a named object. "
		    "Blocks until the walk completes and returns state changes.";
		spec.inputSchema  = mcpObjectSchema(props, req, 2);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- skip ---
	if (_skipToolEnabled) {
		Networking::McpServer::ToolSpec spec;
		spec.name = "skip";
		spec.description =
		    "Skip/cancel current action or cutscene by simulating an Escape key press. "
		    "Useful for skipping long intros or animations. Returns state changes.";
		spec.inputSchema  = nullptr;  // No input required
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
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
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// keystroke — inject a key event
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "keystroke";
			spec.description =
			    "Inject a keyboard keystroke into the engine, so the next engine "
			    "frame processes it. Useful for skipping cutscenes (Escape), "
			    "opening menus, or sending game-specific shortcuts.";
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
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// mouse_move — set the virtual mouse position
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "mouse_move";
			spec.description =
			    "Move the virtual mouse cursor to (x, y) in room/screen coordinates, "
			    "so the engine and its scripts read the new position. Does not click.";
			Common::JSONObject props;
			props.setVal("x", mcpProp("integer", "X coordinate."));
			props.setVal("y", mcpProp("integer", "Y coordinate."));
			const char *req[] = {"x", "y"};
			spec.inputSchema  = mcpObjectSchema(props, req, 2);
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// mouse_click — simulate a mouse click at (x, y)
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "mouse_click";
			spec.description =
			    "Simulate a mouse click at (x, y). The engine processes the click "
			    "the same way as a real player click (walks ego, runs verb script, "
			    "etc.). Set 'double' for a double click. Button defaults to left.";
			Common::JSONObject props;
			props.setVal("x", mcpProp("integer", "X coordinate."));
			props.setVal("y", mcpProp("integer", "Y coordinate."));
			props.setVal("button", mcpProp("string",
			    "Mouse button: 'left' (default), 'right', or 'middle'."));
			props.setVal("double", mcpProp("boolean",
			    "True for a double click (two clicks within ~250ms). Default false."));
			const char *req[] = {"x", "y"};
			spec.inputSchema  = mcpObjectSchema(props, req, 2);
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// screenshot — capture the current frame to the screenshot path
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "screenshot";
			spec.description =
			    "Save a PNG screenshot of the current frame to the configured "
			    "screenshot path (auto-numbered, like the in-app screenshot key). "
			    "Useful for visually inspecting the game state. Engine-agnostic.";
			spec.inputSchema  = nullptr;  // No input required
			spec.outputSchema = nullptr;
			spec.streaming    = false;
			_server->registerTool(spec);
		}
		// save_state — write the current game to a save slot
		{
			Networking::McpServer::ToolSpec spec;
			spec.name = "save_state";
			spec.description =
			    "Save the current game to a save slot, the same way the in-game "
			    "save menu does. Writes the engine's save file for that slot in the "
			    "active save path, so it can be used to capture reusable save states "
			    "for tests. Returns the slot and whether the save was accepted.";
			Common::JSONObject props;
			props.setVal("slot", mcpProp("integer",
			    "Save slot index to write."));
			props.setVal("description", mcpProp("string",
			    "Optional human-readable label stored in the save header "
			    "(default \"mcp save\")."));
			const char *req[] = {"slot"};
			spec.inputSchema  = mcpObjectSchema(props, req, 1);
			spec.outputSchema = nullptr;
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
	if (name == "keystroke")    {
		if (!toolKeystroke(args, errorOut)) return nullptr;
		return new Common::JSONValue(Common::JSONObject());
	}
	if (name == "mouse_move")   {
		if (!toolMouseMove(args, errorOut)) return nullptr;
		return new Common::JSONValue(Common::JSONObject());
	}
	if (name == "mouse_click")  {
		if (!toolMouseClick(args, errorOut)) return nullptr;
		return new Common::JSONValue(Common::JSONObject());
	}
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
	(void)args;
	if (!g_system) {
		errorOut = "screenshot: no system available";
		return nullptr;
	}
	// Save to the configured screenshot path, auto-numbered, exactly like the
	// in-app "save screenshot" action. Engine-agnostic.
	g_system->saveScreenshot();
	Common::JSONObject out;
	out.setVal("saved", mcpJsonBool(true));
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

void McpBridge::beginStream() {
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

	if (_frameCounter - _sseStartFrame < minStreamFrames())
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
	if (_sseLastEventFrame > _sseDoneAtFrame) {
		debug(1, "mcp: new event at frame %d after done at %d, extending window",
		      _sseLastEventFrame, _sseDoneAtFrame);
		_sseDoneAtFrame = _sseLastEventFrame;
	}

	if (shouldCloseStream()) {
		debug(1, "mcp: closing stream at frame %d", _frameCounter);
		closeStreamSuccess();
	}
}

} // End of namespace MCP
