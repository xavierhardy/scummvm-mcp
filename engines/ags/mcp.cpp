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


#include "ags/mcp.h"
#include "ags/mcp_names.h"

#include "ags/ags.h"
#include "ags/globals.h"
#include "ags/shared/ac/character_info.h"
#include "ags/engine/ac/mouse.h"
#include "ags/engine/ac/room_object.h"
#include "ags/engine/ac/runtime_defines.h"
#include "ags/shared/ac/game_setup_struct.h"
#include "ags/shared/game/room_struct.h"
#include "ags/shared/gfx/bitmap.h"

#include "common/events.h"
#include "common/system.h"

namespace AGS3 {

using Networking::mcpJsonString;
using Networking::mcpJsonInt;
using Networking::mcpJsonBool;

// The engine's standard cursor modes, and the verb each one means. A game may
// have removed some of them; collectVerbs() offers only the ones it kept,
// because clicking with a mode the author took out does nothing at all.
struct VerbMode {
	const char *verb;
	int mode;
};

static const VerbMode kVerbModes[] = {
	{ "walk_to", MODE_WALK },
	{ "look_at", MODE_LOOK },
	{ "use",     MODE_HAND },
	{ "talk_to", MODE_TALK },
	{ "use_inv", MODE_USE },
	{ "take",    MODE_PICKUP }
};

AgsMcpBridge *AgsMcpBridge::create(::AGS::AGSEngine *vm) {
	AgsMcpBridge *bridge = new AgsMcpBridge(vm);
	bridge->init();
	return bridge;
}

AgsMcpBridge::AgsMcpBridge(::AGS::AGSEngine *vm) :
	MCP::McpBridge(vm),
	_vm(vm),
	_pendingClick(false),
	_pendingX(0),
	_pendingY(0),
	_pendingFrame(0),
	_skipStream(false),
	_sseTrackRoom(-1),
	_sseTrackPosX(-1),
	_sseTrackPosY(-1) {
}

AgsMcpBridge::~AgsMcpBridge() {
}

// ---------------------------------------------------------------------------
// Engine state
// ---------------------------------------------------------------------------

bool AgsMcpBridge::engineReady() const {
	return g_globals != nullptr && _G(game) != nullptr && _G(thisroom) != nullptr &&
	       _G(playerchar) != nullptr && _G(displayed_room) >= 0;
}

int AgsMcpBridge::roomNumber() const {
	return engineReady() ? _G(displayed_room) : -1;
}

bool AgsMcpBridge::playerPosition(int &x, int &y) const {
	if (!engineReady())
		return false;
	x = _G(playerchar)->x;
	y = _G(playerchar)->y;
	return true;
}

bool AgsMcpBridge::playerHasControl() const {
	if (!engineReady() || _pendingClick)
		return false;
	// `walking` is non-zero for as long as the character is on its way
	// somewhere; the engine blocks input for the whole of that.
	return _G(playerchar)->walking == 0;
}

// ---------------------------------------------------------------------------
// What is in the room
// ---------------------------------------------------------------------------

void AgsMcpBridge::collectTargets(Common::Array<Target> &out) const {
	if (!engineReady())
		return;
	Common::Array<Common::String> seen;

	auto publish = [&](Common::String name, const char *kind, int id, int x, int y) {
		if (name.empty())
			name = agsFallbackName(kind, id);
		uint occurrence = 0;
		for (uint i = 0; i < seen.size(); i++) {
			if (seen[i] == name)
				occurrence++;
		}
		seen.push_back(name);
		Target target;
		target.name = agsDisambiguate(name, occurrence);
		target.kind = kind;
		target.id = id;
		target.x = x;
		target.y = y;
		out.push_back(target);
	};

	// Room objects. `on` is the author's own switch for whether the thing is
	// in the room at all right now, so an object that has been turned off is
	// not something to offer.
	const AGS::Shared::RoomStruct &room = _GP(thisroom);
	for (uint i = 0; i < room.Objects.size(); i++) {
		if (_G(objs) == nullptr || !_G(objs)[i].on)
			continue;
		const Common::String name = agsThingName(
			Common::String(room.Objects[i].Name.GetCStr()),
			Common::String(room.Objects[i].ScriptName.GetCStr()));
		if (agsIsPlaceholderName(name, (int)i))
			continue;
		publish(name, "object", (int)i, _G(objs)[i].x, _G(objs)[i].y);
	}

	// Hotspots. Index 0 is the "nothing here" hotspot every AGS room has, and
	// it is not a thing.
	for (uint i = 1; i < room.HotspotCount; i++) {
		const Common::String name = agsThingName(
			Common::String(room.Hotspots[i].Name.GetCStr()),
			Common::String(room.Hotspots[i].ScriptName.GetCStr()));
		// A hotspot the author never named is a piece of the room's geometry,
		// not a thing: it has no description and nothing to say for itself.
		if (agsIsPlaceholderName(name, (int)i))
			continue;
		// The author's own WalkTo point when there is one - it is inside the
		// hotspot and is where a player is meant to stand. Most hotspots
		// never get one, and then the mask is asked where the shape actually
		// is.
		int hx = room.Hotspots[i].WalkTo.X, hy = room.Hotspots[i].WalkTo.Y;
		if ((hx <= 0 && hy <= 0) && !hotspotPoint((int)i, hx, hy))
			continue;
		publish(name, "hotspot", (int)i, hx, hy);
	}

	// Characters standing in this room, the player aside.
	for (int i = 0; i < _GP(game).numcharacters; i++) {
		const CharacterInfo &who = _GP(game).chars[i];
		if (who.room != _G(displayed_room) || &who == _G(playerchar))
			continue;
		publish(agsThingName(Common::String(who.name), Common::String(who.scrname)),
		        "character", i, who.x, who.y);
	}
}

// A hotspot is an area painted into a mask, not a rectangle: the data says
// which pixels belong to it and nothing else. The author's WalkTo point is
// the polite answer when there is one, but most hotspots never get one, so
// this finds a pixel that belongs to the hotspot and clicks the middle of the
// run it sits in. The mask is usually drawn at a coarser resolution than the
// room, which is what MaskResolution is for.
bool AgsMcpBridge::hotspotPoint(int id, int &x, int &y) const {
	if (!engineReady())
		return false;
	const AGS::Shared::Bitmap *mask = _GP(thisroom).HotspotMask.get();
	if (mask == nullptr)
		return false;
	const int scale = MAX(1, (int)_GP(thisroom).MaskResolution);

	// Sample rather than walk every pixel: a mask is up to a few hundred
	// pixels each way and this runs on every snapshot.
	const int stepX = MAX(1, mask->GetWidth() / 160);
	const int stepY = MAX(1, mask->GetHeight() / 100);
	long sumX = 0, sumY = 0;
	int found = 0;
	for (int my = 0; my < mask->GetHeight(); my += stepY) {
		for (int mx = 0; mx < mask->GetWidth(); mx += stepX) {
			if (mask->GetPixel(mx, my) != id)
				continue;
			sumX += mx;
			sumY += my;
			found++;
		}
	}
	if (found == 0)
		return false;
	// The average of the pixels found. For the shapes these games use - a
	// door, a window, a patch of wall - that lands inside the shape.
	x = (int)(sumX / found) * scale;
	y = (int)(sumY / found) * scale;
	return true;
}

bool AgsMcpBridge::resolveTarget(const Common::String &name, Target &out,
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

void AgsMcpBridge::collectInventory(Common::Array<Common::String> &names,
                                    Common::Array<int> &ids) const {
	if (!engineReady())
		return;
	for (int i = 1; i < _GP(game).numinvitems; i++) {
		if (_G(playerchar)->inv[i] <= 0)
			continue;
		Common::String name = agsDisplayName(
			Common::String(_GP(game).invinfo[i].name.GetCStr()));
		if (name.empty())
			name = agsFallbackName("item", i);
		names.push_back(name);
		ids.push_back(i);
	}
}

void AgsMcpBridge::collectVerbs(Common::Array<Common::String> &verbs) const {
	if (!engineReady())
		return;
	for (uint i = 0; i < ARRAYSIZE(kVerbModes); i++) {
		const int mode = kVerbModes[i].mode;
		if (mode >= (int)_GP(game).mcurs.size())
			continue;
		// A game that does not want a mode disables its cursor. Offering it
		// would be offering a click that does nothing.
		if (_GP(game).mcurs[mode].flags & MCF_DISABLED)
			continue;
		verbs.push_back(kVerbModes[i].verb);
	}
}

int AgsMcpBridge::modeForVerb(const Common::String &verb) const {
	for (uint i = 0; i < ARRAYSIZE(kVerbModes); i++) {
		if (verb == kVerbModes[i].verb)
			return kVerbModes[i].mode;
	}
	return -1;
}

Common::String AgsMcpBridge::verbList() const {
	Common::Array<Common::String> verbs;
	collectVerbs(verbs);
	if (verbs.empty())
		return Common::String();
	Common::String out("The verbs here are: ");
	for (uint i = 0; i < verbs.size(); i++) {
		if (i > 0)
			out += ", ";
		out += verbs[i];
	}
	return out + ".";
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

Common::JSONValue *AgsMcpBridge::callTool(const Common::String &name,
                                          const Common::JSONValue &args,
                                          Common::String &errorOut) {
	if (!engineReady()) {
		errorOut = "the game is still starting up";
		return nullptr;
	}
	return MCP::McpBridge::callTool(name, args, errorOut);
}

Common::JSONValue *AgsMcpBridge::toolState(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	out.setVal("room", new Common::JSONValue(room));

	int px = 0, py = 0;
	if (playerPosition(px, py)) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(px));
		pos.setVal("y", mcpJsonInt(py));
		out.setVal("position", new Common::JSONValue(pos));
	}
	out.setVal("can_act", mcpJsonBool(playerHasControl()));

	Common::JSONArray objects;
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++) {
		Common::JSONObject o;
		o.setVal("id", mcpJsonInt(targets[i].id));
		o.setVal("name", mcpJsonString(targets[i].name));
		o.setVal("kind", mcpJsonString(targets[i].kind));
		o.setVal("x", mcpJsonInt(targets[i].x));
		o.setVal("y", mcpJsonInt(targets[i].y));
		objects.push_back(new Common::JSONValue(o));
	}
	out.setVal("objects", new Common::JSONValue(objects));

	Common::JSONArray inventory;
	Common::Array<Common::String> itemNames;
	Common::Array<int> itemIds;
	collectInventory(itemNames, itemIds);
	for (uint i = 0; i < itemNames.size(); i++) {
		Common::JSONObject item;
		item.setVal("id", mcpJsonInt(itemIds[i]));
		item.setVal("name", mcpJsonString(itemNames[i]));
		if (_G(playerchar)->activeinv == itemIds[i])
			item.setVal("held", mcpJsonBool(true));
		inventory.push_back(new Common::JSONValue(item));
	}
	out.setVal("inventory", new Common::JSONValue(inventory));

	Common::JSONArray verbs;
	Common::Array<Common::String> verbNames;
	collectVerbs(verbNames);
	for (uint i = 0; i < verbNames.size(); i++)
		verbs.push_back(mcpJsonString(verbNames[i]));
	out.setVal("verbs", new Common::JSONValue(verbs));

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

bool AgsMcpBridge::toolAct(const Common::JSONValue &args, Common::String &errorOut) {
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
	const int mode = modeForVerb(verb);
	if (mode < 0) {
		errorOut = Common::String::format("act: '%s' is not a verb here. %s",
		                                  verb.c_str(), verbList().c_str());
		return false;
	}
	Common::Array<Common::String> available;
	collectVerbs(available);
	bool offered = false;
	for (uint i = 0; i < available.size(); i++)
		offered = offered || available[i] == verb;
	if (!offered) {
		errorOut = Common::String::format(
			"act: this game has no '%s'. %s", verb.c_str(), verbList().c_str());
		return false;
	}

	Target target;
	if (!resolveTarget(args.asObject()["target1"]->asString(), target, errorOut)) {
		errorOut = Common::String("act: ") + errorOut;
		return false;
	}

	// Using an inventory item on something is the same click with the item
	// held: the engine reads activeinv when the mode is MODE_USE.
	if (mode == MODE_USE && args.asObject().contains("target2") &&
	    args.asObject()["target2"]->isString()) {
		Common::Array<Common::String> itemNames;
		Common::Array<int> itemIds;
		collectInventory(itemNames, itemIds);
		const Common::String wanted =
			MCP::McpBridge::normalizeActionName(args.asObject()["target2"]->asString());
		int found = -1;
		for (uint i = 0; i < itemNames.size(); i++) {
			if (MCP::McpBridge::normalizeActionName(itemNames[i]) == wanted)
				found = itemIds[i];
		}
		if (found < 0) {
			errorOut = Common::String::format(
				"act: '%s' is not something being carried",
				args.asObject()["target2"]->asString().c_str());
			return false;
		}
		_G(playerchar)->activeinv = found;
	}

	_skipStream = false;
	pointAndClick(target.x, target.y, mode);
	beginStream();
	return true;
}

bool AgsMcpBridge::toolWalk(const Common::JSONValue &args, Common::String &errorOut) {
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
	_skipStream = false;
	pointAndClick((int)args.asObject()["x"]->asIntegerNumber(),
	              (int)args.asObject()["y"]->asIntegerNumber(), MODE_WALK);
	beginStream();
	return true;
}

bool AgsMcpBridge::toolAnswer(const Common::JSONValue &, Common::String &errorOut) {
	errorOut = "answer: this game's conversations are not offered as a list";
	return false;
}

bool AgsMcpBridge::toolSkip(const Common::JSONValue &, Common::String &errorOut) {
	if (!_skipToolEnabled) {
		errorOut = "skip: tool is disabled (set mcp_skip_tool=true)";
		return false;
	}
	// Escape is what a player presses to cut a cutscene short, and a click is
	// what dismisses a line of speech waiting to be read. Both are sent,
	// because which one is wanted depends on what is on screen.
	injectKey(Common::KeyState(Common::KEYCODE_ESCAPE, 27));
	if (!isStreaming()) {
		_skipStream = true;
		beginStream();
	}
	return true;
}

Common::JSONValue *AgsMcpBridge::toolDebug(const Common::JSONValue &, Common::String &) {
	Common::JSONObject out;

	Common::JSONObject engine;
	engine.setVal("room", mcpJsonInt(roomNumber()));
	engine.setVal("cursor_mode", mcpJsonInt(engineReady() ? _G(cur_mode) : -1));
	engine.setVal("characters", mcpJsonInt(engineReady() ? _GP(game).numcharacters : 0));
	engine.setVal("inventory_items", mcpJsonInt(engineReady() ? _GP(game).numinvitems : 0));
	engine.setVal("hotspots", mcpJsonInt(engineReady() ? (int)_GP(thisroom).HotspotCount : 0));
	engine.setVal("room_objects", mcpJsonInt(engineReady() ? (int)_GP(thisroom).Objects.size() : 0));
	out.setVal("engine", new Common::JSONValue(engine));

	Common::JSONObject player;
	int px = 0, py = 0;
	if (playerPosition(px, py)) {
		player.setVal("x", mcpJsonInt(px));
		player.setVal("y", mcpJsonInt(py));
		player.setVal("walking", mcpJsonBool(_G(playerchar)->walking != 0));
		player.setVal("active_item", mcpJsonInt(_G(playerchar)->activeinv));
	}
	out.setVal("player", new Common::JSONValue(player));

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Input injection
// ---------------------------------------------------------------------------

void AgsMcpBridge::injectKey(const Common::KeyState &ks) {
	Common::Event event;
	event.type = Common::EVENT_KEYDOWN;
	event.kbd = ks;
	g_system->getEventManager()->pushEvent(event);
	event.type = Common::EVENT_KEYUP;
	g_system->getEventManager()->pushEvent(event);
}

void AgsMcpBridge::moveCursorTo(int x, int y) {
	g_system->warpMouse(x, y);
	Common::Event event;
	event.type = Common::EVENT_MOUSEMOVE;
	event.mouse = Common::Point(x, y);
	g_system->getEventManager()->pushEvent(event);
}

void AgsMcpBridge::injectMouseMove(int x, int y) {
	moveCursorTo(x, y);
}

void AgsMcpBridge::injectMouseClick(int x, int y, const Common::String &button, bool isDouble) {
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

void AgsMcpBridge::pointAndClick(int x, int y, int mode) {
	// The mode first, because the engine resolves a click against whatever the
	// cursor means at the moment the button goes down; then the pointer, and
	// the press a couple of frames later so the hit-testing has seen it move.
	if (mode >= 0)
		set_cursor_mode(mode);
	moveCursorTo(x, y);
	_pendingClick = true;
	_pendingX = x;
	_pendingY = y;
	_pendingFrame = _frameCounter;
}

void AgsMcpBridge::pumpPendingClick() {
	if (!_pendingClick || (_frameCounter - _pendingFrame) < kPointFrames)
		return;
	_pendingClick = false;
	injectMouseClick(_pendingX, _pendingY, "left", false);
}

// ---------------------------------------------------------------------------
// Text capture
// ---------------------------------------------------------------------------

void AgsMcpBridge::onGameText(const Common::String &text, int charId) {
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

Common::String AgsMcpBridge::messageActorName(int actorId) const {
	if (actorId < 0 || (uint)actorId >= _messageActors.size())
		return Common::String();
	const int who = _messageActors[actorId];
	if (who < 0 || !engineReady() || who >= _GP(game).numcharacters)
		return Common::String();
	return agsThingName(Common::String(_GP(game).chars[who].name),
	                    Common::String(_GP(game).chars[who].scrname));
}

int AgsMcpBridge::currentRoomForMessages() const {
	return roomNumber();
}

// ---------------------------------------------------------------------------
// Per-frame and streaming
// ---------------------------------------------------------------------------

void AgsMcpBridge::pumpGame() {
	pumpPendingClick();
}

void AgsMcpBridge::snapshotPreAction() {
	noteStreamStart();
	_ssePreTargets.clear();
	Common::Array<Target> targets;
	collectTargets(targets);
	for (uint i = 0; i < targets.size(); i++)
		_ssePreTargets.push_back(targets[i].name);

	_ssePreInventory.clear();
	Common::Array<int> ids;
	collectInventory(_ssePreInventory, ids);

	_sseTrackRoom = roomNumber();
	playerPosition(_sseTrackPosX, _sseTrackPosY);
	_ssePreRoom = _sseTrackRoom;
	_ssePrePosX = _sseTrackPosX;
	_ssePrePosY = _sseTrackPosY;
}

Common::JSONObject AgsMcpBridge::buildStateChanges() const {
	Common::JSONObject out;

	Common::JSONObject room;
	room.setVal("id", mcpJsonInt(roomNumber()));
	room.setVal("changed", mcpJsonBool(roomNumber() != _ssePreRoom));
	out.setVal("room", new Common::JSONValue(room));

	int px = 0, py = 0;
	if (playerPosition(px, py)) {
		Common::JSONObject pos;
		pos.setVal("x", mcpJsonInt(px));
		pos.setVal("y", mcpJsonInt(py));
		pos.setVal("changed", mcpJsonBool(px != _ssePrePosX || py != _ssePrePosY));
		out.setVal("position", new Common::JSONValue(pos));
	}

	Common::Array<Common::String> nowItems;
	Common::Array<int> ids;
	collectInventory(nowItems, ids);
	Common::JSONArray gained, lost;
	for (uint i = 0; i < nowItems.size(); i++) {
		bool had = false;
		for (uint j = 0; j < _ssePreInventory.size(); j++)
			had = had || _ssePreInventory[j] == nowItems[i];
		if (!had)
			gained.push_back(mcpJsonString(nowItems[i]));
	}
	for (uint j = 0; j < _ssePreInventory.size(); j++) {
		bool still = false;
		for (uint i = 0; i < nowItems.size(); i++)
			still = still || nowItems[i] == _ssePreInventory[j];
		if (!still)
			lost.push_back(mcpJsonString(_ssePreInventory[j]));
	}
	out.setVal("inventory_gained", new Common::JSONValue(gained));
	out.setVal("inventory_lost", new Common::JSONValue(lost));

	out.setVal("can_act", mcpJsonBool(playerHasControl()));
	return out;
}

bool AgsMcpBridge::isActionDone() const {
	if (_skipStream)
		return (_frameCounter - _sseStartFrame) >= kSkipFrames;
	return !_pendingClick && playerHasControl();
}

bool AgsMcpBridge::hasPendingQuestion() const {
	return false;
}

bool AgsMcpBridge::streamRoomChanged() const {
	return roomNumber() != _ssePreRoom;
}

void AgsMcpBridge::pumpStreamTrack() {
	const int room = roomNumber();
	int px = 0, py = 0;
	playerPosition(px, py);
	if (room != _sseTrackRoom || px != _sseTrackPosX || py != _sseTrackPosY) {
		_sseTrackRoom = room;
		_sseTrackPosX = px;
		_sseTrackPosY = py;
		_sseLastEventFrame = _frameCounter;
	}
	if (_sseWorkDoneFrame == 0 && isActionDone())
		_sseWorkDoneFrame = _frameCounter;
}

// ---------------------------------------------------------------------------
// What an agent reads about the tools
// ---------------------------------------------------------------------------

Common::String AgsMcpBridge::stateToolDescription() const {
	return "The room as it is now: everything in it that can be named - the "
	       "things, the ways through and the people - with where each one is, "
	       "what is being carried, which verbs this game has, and every line "
	       "said since the last call (reading them clears them). Read this "
	       "before every action: the names it lists are the names act() takes.";
}

Common::String AgsMcpBridge::actToolDescription() const {
	return "Act on something state() named. The verb is one of the ones "
	       "state() lists; target1 is the name from state(). To use a carried "
	       "thing on something, pass verb='use_inv' with the carried thing as "
	       "target2. " + streamingToolNote();
}

Common::String AgsMcpBridge::walkToolDescription() const {
	return "Go to a point, in the coordinates state() reports positions in. "
	       "Somewhere the character cannot reach leaves it where it is. " +
	       streamingToolNote();
}

Common::String AgsMcpBridge::skipToolDescription() const {
	return "Cut short whatever is playing itself out - an opening sequence, a "
	       "line being spoken - by pressing escape once.";
}

Common::String AgsMcpBridge::debugToolDescription() const {
	return "Diagnostics: which room is loaded, how much is in it, what the "
	       "cursor currently means, and where the player character is.";
}

Common::JSONValue *AgsMcpBridge::buildDebugSchema() const {
	Common::JSONObject schema;
	schema.setVal("type", mcpJsonString("object"));
	Common::JSONObject props;
	schema.setVal("properties", new Common::JSONValue(props));
	return new Common::JSONValue(schema);
}

void AgsMcpBridge::augmentStateSchema(Common::JSONObject &outputProps) {
	Common::JSONObject kind;
	kind.setVal("type", mcpJsonString("string"));
	kind.setVal("description", mcpJsonString(
		"What sort of thing each object is: object, hotspot or character."));
	outputProps.setVal("object_kind", new Common::JSONValue(kind));
}

void AgsMcpBridge::augmentChangesSchema(Common::JSONObject &props) {
	Common::JSONObject gained;
	gained.setVal("type", mcpJsonString("array"));
	Common::JSONObject name;
	name.setVal("type", mcpJsonString("string"));
	gained.setVal("items", new Common::JSONValue(name));
	gained.setVal("description", mcpJsonString("Things picked up by this action."));
	props.setVal("inventory_gained", new Common::JSONValue(gained));

	Common::JSONObject lost;
	lost.setVal("type", mcpJsonString("array"));
	Common::JSONObject lname;
	lname.setVal("type", mcpJsonString("string"));
	lost.setVal("items", new Common::JSONValue(lname));
	lost.setVal("description", mcpJsonString("Things given up by this action."));
	props.setVal("inventory_lost", new Common::JSONValue(lost));
}

} // End of namespace AGS3
