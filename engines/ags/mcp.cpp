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
#include "ags/shared/gui/gui_button.h"
#include "ags/shared/gui/gui_label.h"
#include "ags/shared/gui/gui_main.h"
#include "ags/shared/gui/gui_object.h"

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
	_pendingVerbClick(false),
	_pendingVerbX(0),
	_pendingVerbY(0),
	_verbButtonNext(0),
	_verbAttempts(0),
	_pressedButton(-1),
	_verbReadyMs(0),
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

// What a verb bar's button says, and the verb it means. Matched against the
// button's own label, folded, so a game gets its verbs from the words it
// shows the player rather than from a list of game names kept here.
static const struct { const char *label; const char *verb; } kButtonVerbs[] = {
	{ "walk_to",  "walk_to" },
	{ "walk",     "walk_to" },
	{ "go_to",    "walk_to" },
	{ "look",     "look_at" },
	{ "look_at",  "look_at" },
	{ "examine",  "look_at" },
	{ "use",      "use" },
	{ "open",     "open" },
	{ "close",    "close" },
	{ "push",     "push" },
	{ "pull",     "pull" },
	{ "talk",     "talk_to" },
	{ "talk_to",  "talk_to" },
	{ "pick_up",  "take" },
	{ "take",     "take" },
	{ "give",     "give" },
	{ "turn_on",  "turn_on" },
	{ "turn_off", "turn_off" },
	{ "fix",      "fix" },
	{ "new_kid",  "switch_character" }
};

// The verb a label's text means, folded through the same vocabulary the
// buttons are matched against. Empty when the words are not a verb at all.
static Common::String verbFromWords(const Common::String &words) {
	const Common::String folded = agsDisplayName(words);
	for (uint k = 0; k < ARRAYSIZE(kButtonVerbs); k++) {
		if (folded == kButtonVerbs[k].label)
			return kButtonVerbs[k].verb;
	}
	return Common::String();
}

Common::String AgsMcpBridge::currentVerbFromLabel() const {
	if (!engineReady() || _G(guis) == nullptr)
		return Common::String();
	for (uint g = 0; g < _GP(guis).size(); g++) {
		const AGS::Shared::GUIMain &gui = _GP(guis)[g];
		if (!gui.IsDisplayed())
			continue;
		for (int32_t ci = 0; ci < gui.GetControlCount(); ci++) {
			const AGS::Shared::GUILabel *label =
				dynamic_cast<const AGS::Shared::GUILabel *>(gui.GetControl(ci));
			if (label == nullptr)
				continue;
			// The status line reads "<verb> <whatever is under the pointer>",
			// so the verb is however much of the front of it is a verb. Two
			// words first, because "pick up" and "look at" are two.
			const Common::String text(label->GetText().GetCStr());
			Common::String words[2];
			uint word = 0;
			for (uint i = 0; i < text.size() && word < 2; i++) {
				if (text[i] == ' ') {
					word++;
					continue;
				}
				words[word] += text[i];
			}
			const Common::String two = words[0] + " " + words[1];
			Common::String verb = verbFromWords(two);
			if (verb.empty())
				verb = verbFromWords(words[0]);
			if (!verb.empty())
				return verb;
		}
	}
	return Common::String();
}

void AgsMcpBridge::collectVerbButtons(Common::Array<Verb> &verbs) const {
	if (!engineReady() || _G(guis) == nullptr)
		return;
	for (uint g = 0; g < _GP(guis).size(); g++) {
		const AGS::Shared::GUIMain &gui = _GP(guis)[g];
		if (!gui.IsDisplayed())
			continue;
		for (int32_t ci = 0; ci < gui.GetControlCount(); ci++) {
			const AGS::Shared::GUIObject *control = gui.GetControl(ci);
			if (control == nullptr || !control->IsVisible() || !control->IsEnabled())
				continue;
			const AGS::Shared::GUIButton *button =
				dynamic_cast<const AGS::Shared::GUIButton *>(control);
			if (button == nullptr)
				continue;
			// Most fan games never change a button's Text from the editor's
			// default and draw their own labels, so matching on the label is
			// only the easy case. When it does not match, the button is still
			// offered as a verb button with no name yet: which verb it is is
			// found by pressing it and reading the status line.
			const Common::String label =
				agsDisplayName(Common::String(button->GetText().GetCStr()));
			Common::String named;
			for (uint k = 0; k < ARRAYSIZE(kButtonVerbs); k++) {
				if (label == kButtonVerbs[k].label)
					named = kButtonVerbs[k].verb;
			}
			{
				Verb verb;
				verb.name = named;
				verb.mode = -1;
				verb.guiId = (int)g;
				verb.controlId = (int)ci;
				// The middle of the button, in screen coordinates: a GUI
				// control's position is relative to the GUI it sits on.
				verb.x = gui.X + control->X + control->GetWidth() / 2;
				verb.y = gui.Y + control->Y + control->GetHeight() / 2;
				verbs.push_back(verb);
			}
		}
	}
}

void AgsMcpBridge::collectVerbs(Common::Array<Verb> &verbs) const {
	if (!engineReady())
		return;

	// A verb bar, when the game has one, is the whole answer: a game that
	// drives its own verb state from buttons ignores the cursor mode
	// entirely, so offering modes as well would offer clicks that do nothing.
	Common::Array<Verb> buttons;
	collectVerbButtons(buttons);
	if (!buttons.empty()) {
		// Which button is which verb is not written anywhere: most games
		// leave the editor's default text on them and draw their own labels.
		// So what is offered is the vocabulary the bridge knows how to reach
		// by pressing, and act() finds the button by pressing and reading the
		// status line back. A button whose text did say what it was keeps
		// that name and is reached in one press.
		for (uint b = 0; b < buttons.size(); b++) {
			if (buttons[b].name.empty())
				continue;
			bool already = false;
			for (uint v = 0; v < verbs.size(); v++)
				already = already || verbs[v].name == buttons[b].name;
			if (!already)
				verbs.push_back(buttons[b]);
		}
		if (!verbs.empty())
			return;
		// Nothing named itself. Offer the standard bar vocabulary; a verb the
		// game does not have simply cannot be reached, and act() says so.
		static const char *const kBarVerbs[] = {
			"walk_to", "look_at", "use", "talk_to", "take", "give", nullptr
		};
		for (int i = 0; kBarVerbs[i] != nullptr; i++) {
			Verb verb;
			verb.name = kBarVerbs[i];
			verb.mode = -1;
			verb.guiId = buttons[0].guiId;
			verb.controlId = -2;  // "on the bar, button not yet known"
			verb.x = verb.y = 0;
			verbs.push_back(verb);
		}
		return;
	}

	for (uint i = 0; i < ARRAYSIZE(kVerbModes); i++) {
		const int mode = kVerbModes[i].mode;
		if (mode >= (int)_GP(game).mcurs.size())
			continue;
		// A game that does not want a mode disables its cursor. Offering it
		// would be offering a click that does nothing.
		if (_GP(game).mcurs[mode].flags & MCF_DISABLED)
			continue;
		Verb verb;
		verb.name = kVerbModes[i].verb;
		verb.mode = mode;
		verb.guiId = verb.controlId = -1;
		verb.x = verb.y = 0;
		verbs.push_back(verb);
	}
}

bool AgsMcpBridge::findVerb(const Common::String &verb, Verb &out) const {
	Common::Array<Verb> verbs;
	collectVerbs(verbs);
	for (uint i = 0; i < verbs.size(); i++) {
		if (verbs[i].name == verb) {
			out = verbs[i];
			return true;
		}
	}
	return false;
}

Common::String AgsMcpBridge::verbList() const {
	Common::Array<Verb> verbs;
	collectVerbs(verbs);
	if (verbs.empty())
		return Common::String();
	Common::String out("The verbs here are: ");
	for (uint i = 0; i < verbs.size(); i++) {
		if (i > 0)
			out += ", ";
		out += verbs[i].name;
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
	Common::Array<Verb> verbList_;
	collectVerbs(verbList_);
	for (uint i = 0; i < verbList_.size(); i++)
		verbs.push_back(mcpJsonString(verbList_[i].name));
	out.setVal("verbs", new Common::JSONValue(verbs));
	const Common::String showing = currentVerbFromLabel();
	if (!showing.empty())
		out.setVal("current_verb", mcpJsonString(showing));

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
	Verb chosen;
	if (!findVerb(verb, chosen)) {
		errorOut = Common::String::format("act: '%s' is not a verb here. %s",
		                                  verb.c_str(), verbList().c_str());
		return false;
	}

	Target target;
	if (!resolveTarget(args.asObject()["target1"]->asString(), target, errorOut)) {
		errorOut = Common::String("act: ") + errorOut;
		return false;
	}

	// Using an inventory item on something is the same click with the item
	// held: the engine reads activeinv when the mode is MODE_USE.
	if (chosen.mode == MODE_USE && args.asObject().contains("target2") &&
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
	pointAndClick(target.x, target.y, chosen);
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
	Verb walk;
	if (!findVerb("walk_to", walk)) {
		// Not every game has a walk verb of its own; a plain click on the
		// floor is what walks in those.
		walk.name = "walk_to";
		walk.mode = MODE_WALK;
		walk.guiId = walk.controlId = -1;
		walk.x = walk.y = 0;
	}
	_skipStream = false;
	pointAndClick((int)args.asObject()["x"]->asIntegerNumber(),
	              (int)args.asObject()["y"]->asIntegerNumber(), walk);
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

	// The game's own interface, which is where a verb bar lives when there is
	// one. Worth reporting because whether a game has one decides how a verb
	// is chosen at all, and that is invisible from anywhere else.
	Common::JSONArray guis;
	if (engineReady() && _G(guis) != nullptr) {
		for (uint g = 0; g < _GP(guis).size(); g++) {
			const AGS::Shared::GUIMain &gui = _GP(guis)[g];
			Common::JSONObject entry;
			entry.setVal("id", mcpJsonInt((int)g));
			entry.setVal("name", mcpJsonString(gui.Name.GetCStr()));
			entry.setVal("displayed", mcpJsonBool(gui.IsDisplayed()));
			Common::JSONArray labels;
			for (int32_t ci = 0; ci < gui.GetControlCount(); ci++) {
				const AGS::Shared::GUIObject *control = gui.GetControl(ci);
				const AGS::Shared::GUIButton *button =
					dynamic_cast<const AGS::Shared::GUIButton *>(control);
				if (button == nullptr)
					continue;
				labels.push_back(mcpJsonString(button->GetText().GetCStr()));
			}
			entry.setVal("buttons", new Common::JSONValue(labels));
			Common::JSONArray texts;
			for (int32_t ci = 0; ci < gui.GetControlCount(); ci++) {
				const AGS::Shared::GUILabel *label =
					dynamic_cast<const AGS::Shared::GUILabel *>(gui.GetControl(ci));
				if (label == nullptr)
					continue;
				texts.push_back(mcpJsonString(label->GetText().GetCStr()));
			}
			entry.setVal("labels", new Common::JSONValue(texts));
			guis.push_back(new Common::JSONValue(entry));
		}
	}
	out.setVal("guis", new Common::JSONValue(guis));

	// Where the bridge thinks the verb bar's buttons are, and what the game
	// says is selected. The two together are the whole of how a verb is
	// chosen on a bar, and both are invisible from anywhere else.
	Common::JSONArray bar;
	Common::Array<Verb> buttons;
	collectVerbButtons(buttons);
	for (uint i = 0; i < buttons.size(); i++) {
		Common::JSONObject b;
		b.setVal("gui", mcpJsonInt(buttons[i].guiId));
		b.setVal("control", mcpJsonInt(buttons[i].controlId));
		b.setVal("x", mcpJsonInt(buttons[i].x));
		b.setVal("y", mcpJsonInt(buttons[i].y));
		if (!buttons[i].name.empty())
			b.setVal("verb", mcpJsonString(buttons[i].name));
		bar.push_back(new Common::JSONValue(b));
	}
	out.setVal("verb_buttons", new Common::JSONValue(bar));
	out.setVal("current_verb", mcpJsonString(currentVerbFromLabel()));

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// select_verb
// ---------------------------------------------------------------------------
//
// A game whose verbs are cursor modes needs nothing here: act() sets the mode
// itself, in the same breath as the click. A game whose verbs are buttons on a
// bar is different, and honestly so: which button is which verb is written
// nowhere - most of these games leave the editor's default text on them and
// draw their own labels - so the only way to find out is to press one and read
// what the game then writes on its status line.
//
// That is a thing an agent can do perfectly well, and doing it in the open is
// better than a bridge guessing: press, read the answer back, and now both
// know. So the tool is one press and one answer, and it is registered only for
// the games that have a bar.

void AgsMcpBridge::registerGameTools() {
	// Registered whatever the game turns out to be, because this runs while
	// the engine is still starting up and no room is loaded yet - there is
	// nothing to ask about a verb bar at this point. A game that has no bar
	// refuses the call instead, which is the honest place for that answer.
	Networking::McpServer::ToolSpec spec;
	spec.name = "select_verb";
	spec.description =
		"Choose the verb the next act() will use, on a game whose verbs are "
		"buttons along the bottom of the screen. Give a verb name to press the "
		"button known to mean it, or a button number to press that one and find "
		"out what it means. The answer is what the game then says is selected, "
		"so pressing each button once is how an agent learns the bar.";
	Common::JSONObject props;
	props.setVal("verb", Networking::mcpProp("string",
		"The verb to select, when it is already known which button means it."));
	props.setVal("button", Networking::mcpProp("integer",
		"The button to press, counted from 0, when the verb is not known yet."));
	spec.inputSchema = Networking::mcpObjectSchema(props);
	Common::JSONObject outProps;
	outProps.setVal("was", Networking::mcpProp("string",
		"What was selected before this press; the game writes what it is now "
		"on a later loop, so read state() for that."));
	outProps.setVal("known", Networking::mcpProp("array",
		"Which button means which verb, as far as has been found out."));
	outProps.setVal("button", Networking::mcpProp("integer", "The button pressed."));
	outProps.setVal("buttons", Networking::mcpProp("integer",
		"How many buttons the bar has."));
	spec.outputSchema = Networking::mcpObjectSchema(outProps);
	spec.streaming = false;
	_server->registerTool(spec);
}

Common::JSONValue *AgsMcpBridge::dispatchGameTool(const Common::String &name,
                                                  const Common::JSONValue &args,
                                                  Common::String &errorOut,
                                                  bool &handled) {
	if (name != "select_verb")
		return MCP::McpBridge::dispatchGameTool(name, args, errorOut, handled);
	handled = true;

	Common::Array<Verb> buttons;
	collectVerbButtons(buttons);
	if (buttons.empty()) {
		errorOut = "select_verb: this game has no verb bar";
		return nullptr;
	}

	// Before anything else, learn from the press before this one. The status
	// line has had a whole call's worth of real time to be written by now,
	// which is what makes reading it here reliable where reading it a few
	// frames after the press was not.
	const Common::String showing = currentVerbFromLabel();
	if (_pressedButton >= 0 && !showing.empty()) {
		bool known = false;
		for (uint i = 0; i < _learnedButtons.size(); i++)
			known = known || _learnedButtons[i] == _pressedButton;
		if (!known) {
			_learnedButtons.push_back(_pressedButton);
			_learnedVerbs.push_back(showing);
		}
		_pressedButton = -1;
	}

	int pick = -1;
	if (args.isObject() && args.asObject().contains("button") &&
	    args.asObject()["button"]->isIntegerNumber()) {
		pick = (int)args.asObject()["button"]->asIntegerNumber();
	} else if (args.isObject() && args.asObject().contains("verb") &&
	           args.asObject()["verb"]->isString()) {
		const Common::String wanted =
			MCP::McpBridge::normalizeActionName(args.asObject()["verb"]->asString());
		for (uint i = 0; i < _learnedButtons.size() && pick < 0; i++) {
			if (_learnedVerbs[i] == wanted)
				pick = _learnedButtons[i];
		}
		for (uint i = 0; i < buttons.size() && pick < 0; i++) {
			if (buttons[i].name == wanted)
				pick = (int)i;
		}
		if (pick < 0) {
			errorOut = Common::String::format(
				"select_verb: nothing is known to mean \'%s\' yet. Press the "
				"buttons by number - there are %u - and each answer says what "
				"that one means.", wanted.c_str(), (uint)buttons.size());
			return nullptr;
		}
	} else {
		errorOut = "select_verb: a 'verb' name or a 'button' number is required";
		return nullptr;
	}
	if (pick < 0 || pick >= (int)buttons.size()) {
		errorOut = Common::String::format(
			"select_verb: there are %u buttons, counted from 0", (uint)buttons.size());
		return nullptr;
	}

	injectMouseClick(buttons[pick].x, buttons[pick].y, "left", false);
	_pressedButton = pick;
	_verbReadyMs = g_system->getMillis() + kVerbSettleMs;

	Common::JSONObject out;
	out.setVal("button", mcpJsonInt(pick));
	out.setVal("buttons", mcpJsonInt((int)buttons.size()));
	// What the press just made of it is not on the status line yet - the game
	// writes that from its own script, on a later loop. So this answers with
	// what was selected *before*, and the verb the press produced is what the
	// next state() reports. Pressing each button once and reading state()
	// after each is how an agent learns the bar; the bridge learns it at the
	// same time, from the same answers.
	out.setVal("was", mcpJsonString(showing));
	Common::JSONArray learned;
	for (uint i = 0; i < _learnedButtons.size(); i++) {
		Common::JSONObject entry;
		entry.setVal("button", mcpJsonInt(_learnedButtons[i]));
		entry.setVal("verb", mcpJsonString(_learnedVerbs[i]));
		learned.push_back(new Common::JSONValue(entry));
	}
	out.setVal("known", new Common::JSONValue(learned));
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

void AgsMcpBridge::pointAndClick(int x, int y, const Verb &verb) {
	// The verb first, because the game resolves a click against whatever the
	// verb is at the moment the button goes down. A cursor mode can be set
	// outright; a verb bar's button has to be pressed, and that press needs
	// its own frames before the one on the target - which is what
	// _pendingVerbClick carries.
	// A cursor-mode game can have its verb set outright, in the same breath as
	// the click. A verb-bar game cannot: its verb is whatever its own script
	// last wrote on its status line, and the only way to change that is to
	// press a button and give the game real time to notice - which is what
	// select_verb is for, and why act() does not try to do it here.
	if (verb.mode >= 0)
		set_cursor_mode(verb.mode);
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
	Common::Array<Verb> buttons;
	collectVerbButtons(buttons);
	if (!buttons.empty()) {
		return "Act on something state() named, using the verb that is "
		       "currently selected - state() reports it as current_verb, and "
		       "select_verb changes it. The verb given here is checked against "
		       "that one rather than setting it, because on this game a verb is "
		       "a button that has to be pressed and noticed. " +
		       streamingToolNote();
	}
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
