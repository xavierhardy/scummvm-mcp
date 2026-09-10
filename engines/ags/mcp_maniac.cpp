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

#include "ags/mcp_maniac.h"
#include "ags/mcp_names.h"

#include "ags/ags.h"
#include "ags/globals.h"
#include "ags/shared/ac/character_info.h"
#include "ags/engine/ac/room_object.h"
#include "ags/shared/ac/game_setup_struct.h"
#include "ags/shared/game/room_struct.h"
#include "ags/shared/gui/gui_defines.h"
#include "ags/shared/gui/gui_button.h"
#include "ags/shared/gui/gui_label.h"
#include "ags/shared/gui/gui_main.h"
#include "ags/shared/gui/gui_object.h"

#include "common/events.h"

namespace AGS3 {

using Networking::mcpJsonString;
using Networking::mcpJsonBool;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// How long each step of the machine is given, in engine frames. AGS runs the
// game at its own configured rate, commonly 40 a second, and the bridge is
// pumped once per game loop.
static const uint32 kLabelFrames  = 60;   // longest wait for a portrait's line
static const uint32 kClickFrames  = 12;   // between a click and reading its effect
static const uint32 kStartFrames  = 240;  // START -> the game leaves the title screen
static const uint32 kIntroFrames  = 7200; // ceiling on sitting through the opening
// The opening is not one cutscene but several - four rooms, and a run of
// scenes inside them - and each takes an escape of its own, so they are sent
// one after another until the game hands control over. A dozen does it. The
// budget is a stop for a game that is not listening, not a pace; what the
// escaping is actually waiting for is the verb bar coming back.
static const uint32 kEscapeGap    = 8;
static const int    kMaxEscapes   = 60;
// Aborting the demo the game plays itself is not the same as skipping through
// a cutscene, and wants a slower hand. An escape sent while a scene of the
// demo is still playing skips that scene rather than the demo, so a rapid
// burst simply races through the whole show; one every couple of seconds ends
// it, which is the pacing this was measured at.
static const uint32 kDemoEscapeGap = 200;
static const int    kMaxDemoEscapes = 30;
static const uint32 kSwitchFrames = 120;  // a portrait button -> control changes hands

AgsMcpBridgeManiacDeluxe::AgsMcpBridgeManiacDeluxe(::AGS::AGSEngine *vm) :
	AgsMcpBridge(vm),
	_phase(kIdle),
	_startX(0),
	_startY(0),
	_pickIndex(0),
	_phaseFrame(0),
	_selectRoom(-1),
	_skipIntro(true),
	_escapes(0),
	_lastEscapeFrame(0),
	_switchIndex(0) {
}

// ---------------------------------------------------------------------------
// Tool registration
// ---------------------------------------------------------------------------

void AgsMcpBridgeManiacDeluxe::registerGameTools() {
	AgsMcpBridge::registerGameTools();

	// --- choose_kids (title screen: pick the team, then START) ---
	{
		Common::JSONObject props;
		Common::JSONObject kids;
		kids.setVal("type", mcpJsonString("array"));
		kids.setVal("items", mcpProp("string"));
		kids.setVal("description", mcpJsonString(
		    "The three heroes, by name: the two kids joining Dave, or all three "
		    "with Dave named as well. The cast is dave, syd, michael, wendy, "
		    "bernard, razor and jeff."));
		props.setVal("kids", new Common::JSONValue(kids));
		props.setVal("skip_intro", mcpProp("boolean",
		    "Escape through the opening scene so the call returns with the "
		    "player already in control (default true)."));
		const char *req[] = {"kids"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "choose_kids";
		spec.description =
		    "Pick the three heroes on the opening screen and start the game. "
		    "Dave always leads the rescue, so name the two who join him (naming "
		    "Dave as the third is accepted too). Only the two portraits that are "
		    "wanted are clicked, and each click is checked against the line the "
		    "game writes for it, so the order they are drawn in does not matter. "
		    "Valid until the team is picked - state says so with "
		    "kid_selection_pending - and that includes the demo the game starts "
		    "playing if the screen is left alone for a minute: the call escapes "
		    "out of it and back to the screen first, so there is no hurry. Blocks "
		    "until play has begun (skipping the opening scene by default) and "
		    "returns what changed, with the team in 'kids'.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- switch_character ---
	{
		Common::JSONObject props;
		props.setVal("name", mcpProp("string",
		    "Name of the kid to control, as listed in state.available_characters (e.g. 'dave')."));
		const char *req[] = {"name"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "switch_character";
		spec.description =
		    "Hand control to another member of the team, the way the two portrait "
		    "buttons at the right-hand end of the verb bar do. state lists who is "
		    "available in 'available_characters' and who has control in "
		    "'controlling'. Only allowed during normal play, with the verb bar up. "
		    "Blocks until the switch settles, then returns what changed: the room "
		    "and position are the new character's.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}
}

Common::JSONValue *AgsMcpBridgeManiacDeluxe::dispatchGameTool(const Common::String &name,
                                                              const Common::JSONValue &args,
                                                              Common::String &errorOut,
                                                              bool &handled) {
	if (name == "choose_kids") {
		handled = true;
		toolChooseKids(args, errorOut);
		return nullptr; // streaming
	}
	if (name == "switch_character") {
		handled = true;
		toolSwitchCharacter(args, errorOut);
		return nullptr; // streaming
	}
	return AgsMcpBridge::dispatchGameTool(name, args, errorOut, handled);
}

// ---------------------------------------------------------------------------
// What state says
// ---------------------------------------------------------------------------

void AgsMcpBridgeManiacDeluxe::augmentStateSchema(Common::JSONObject &outputProps) {
	AgsMcpBridge::augmentStateSchema(outputProps);
	outputProps.setVal("kid_selection_pending", mcpProp("boolean",
	    "True while the opening screen is waiting for the three heroes to be "
	    "picked; use the choose_kids tool to pick them and start the game"));
	outputProps.setVal("controlling", mcpProp("string", "Name of the currently controlled kid"));
	Common::JSONObject arr;
	arr.setVal("type",  mcpJsonString("array"));
	arr.setVal("items", mcpProp("string"));
	outputProps.setVal("available_characters", new Common::JSONValue(arr));
}

void AgsMcpBridgeManiacDeluxe::augmentState(Common::JSONObject &out) {
	// The opening screen has no named thing on it at all, so without this an
	// agent booting the game is looking at an empty room with no way to tell
	// that anything is being asked of it. It goes on being said while the game
	// is playing itself, because that is the same waiting screen with a demo
	// running over it and choose_kids is still the answer to it.
	if (!teamPicked())
		out.setVal("kid_selection_pending", mcpJsonBool(true));

	Common::Array<Common::String> names;
	Common::Array<int> ids;
	collectTeam(names, ids);
	if (names.empty())
		return;
	Common::JSONArray arr;
	for (uint i = 0; i < names.size(); i++)
		arr.push_back(mcpJsonString(names[i]));
	out.setVal("available_characters", new Common::JSONValue(arr));
	const Common::String controlling = controllingKid();
	if (!controlling.empty())
		out.setVal("controlling", mcpJsonString(controlling));
}

void AgsMcpBridgeManiacDeluxe::augmentChangesSchema(Common::JSONObject &props) {
	AgsMcpBridge::augmentChangesSchema(props);
	props.setVal("kids", mcpProp("array",
	    "Names of the heroes choose_kids put in the team, as the game named them"));
	props.setVal("controlling", mcpProp("string",
	    "Name of the kid holding control now"));
}

void AgsMcpBridgeManiacDeluxe::augmentStateChanges(Common::JSONObject &changes) const {
	const Common::String controlling = controllingKid();
	if (!controlling.empty())
		changes.setVal("controlling", mcpJsonString(controlling));
	if (_chosen.empty())
		return;
	Common::JSONArray kids;
	for (uint i = 0; i < _chosen.size(); i++)
		kids.push_back(mcpJsonString(_chosen[i]));
	changes.setVal("kids", new Common::JSONValue(kids));
}

// ---------------------------------------------------------------------------
// Reading the opening screen
// ---------------------------------------------------------------------------

bool AgsMcpBridgeManiacDeluxe::collectKidSelectScreen(Common::Array<KidSlot> &slots,
                                                      int &startX, int &startY) const {
	slots.clear();
	if (!engineReady() || _G(objs) == nullptr)
		return false;
	const AGS::Shared::RoomStruct &room = _GP(thisroom);

	// The portraits and START are hotspots the author never named, so what
	// tells them apart is where they are: the portraits are a row sharing one
	// height, and START is the one hotspot that is not in it.
	Common::Array<ScreenPoint> points;
	Common::Array<int> ids;
	for (uint i = 1; i < room.HotspotCount; i++) {
		ScreenPoint point;
		if (!hotspotPoint((int)i, point.x, point.y))
			continue;
		points.push_back(point);
		ids.push_back((int)i);
	}
	if (points.size() < kMinPortraits + 1)
		return false;

	// The row is the largest set of hotspots at one height.
	int rowY = -1;
	uint rowCount = 0;
	for (uint i = 0; i < points.size(); i++) {
		uint count = 0;
		for (uint j = 0; j < points.size(); j++) {
			if (points[j].y == points[i].y)
				count++;
		}
		if (count > rowCount) {
			rowCount = count;
			rowY = points[i].y;
		}
	}
	if (rowCount < kMinPortraits || rowCount + 1 != points.size())
		return false;

	for (uint i = 0; i < points.size(); i++) {
		if (points[i].y == rowY) {
			KidSlot slot;
			slot.hotspot = ids[i];
			slot.x = points[i].x;
			slot.y = points[i].y;
			slot.frameObj = -1;
			slots.push_back(slot);
		} else {
			startX = points[i].x;
			startY = points[i].y;
		}
	}

	// Left to right, so the sweep runs the way the screen reads.
	for (uint i = 0; i + 1 < slots.size(); i++)
		for (uint j = 0; j + 1 < slots.size() - i; j++)
			if (slots[j].x > slots[j + 1].x) {
				KidSlot t = slots[j];
				slots[j] = slots[j + 1];
				slots[j + 1] = t;
			}

	// The frames drawn around picked kids: one room object per slot, all
	// sharing a sprite, because they are the same picture in seven places. The
	// sprite each portrait itself is drawn from differs, which is exactly what
	// tells the two rows apart.
	int frameSprite = -1;
	uint frameCount = 0;
	for (uint i = 0; i < room.Objects.size(); i++) {
		uint count = 0;
		for (uint j = 0; j < room.Objects.size(); j++) {
			if (_G(objs)[j].num == _G(objs)[i].num)
				count++;
		}
		if (count > frameCount) {
			frameCount = count;
			frameSprite = _G(objs)[i].num;
		}
	}
	if (frameCount != slots.size())
		return false;
	Common::Array<int> frames;
	for (uint i = 0; i < room.Objects.size(); i++) {
		if (_G(objs)[i].num == frameSprite)
			frames.push_back((int)i);
	}
	// Left to right as well, then paired with the portraits by position.
	for (uint i = 0; i + 1 < frames.size(); i++)
		for (uint j = 0; j + 1 < frames.size() - i; j++)
			if (_G(objs)[frames[j]].x > _G(objs)[frames[j + 1]].x) {
				int t = frames[j];
				frames[j] = frames[j + 1];
				frames[j + 1] = t;
			}
	for (uint i = 0; i < slots.size(); i++)
		slots[i].frameObj = frames[i];
	return true;
}

bool AgsMcpBridgeManiacDeluxe::slotPicked(const KidSlot &slot) const {
	if (!engineReady() || _G(objs) == nullptr || slot.frameObj < 0)
		return false;
	return _G(objs)[slot.frameObj].on != 0;
}

Common::String AgsMcpBridgeManiacDeluxe::shownKidName() const {
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
			const Common::String text =
				MCP::mcpCleanGameText(safeUtf8(Common::String(label->GetText().GetCStr())));
			// A portrait's line reads "<Name> - <blurb>". The line the screen
			// shows while it waits ("Please select two other kids.") has no
			// dash, which is what tells the two apart.
			uint dash = 0;
			for (uint c = 1; c + 1 < text.size(); c++) {
				if (text[c] == '-' && text[c - 1] == ' ') {
					dash = c;
					break;
				}
			}
			if (!dash)
				continue;
			Common::String name(text.c_str(), dash);
			name.trim();
			// A kid's name is a single word; anything else is not a portrait line.
			if (name.empty() || name.contains(' '))
				continue;
			return normalizeActionName(name);
		}
	}
	return Common::String();
}

// ---------------------------------------------------------------------------
// The team, once the game has started
// ---------------------------------------------------------------------------

void AgsMcpBridgeManiacDeluxe::collectCast(Common::Array<Common::String> &names) const {
	names.clear();
	if (!engineReady() || _GP(game).numcharacters < kKidCharacters)
		return;
	for (int i = 0; i < kKidCharacters; i++) {
		const CharacterInfo &who = _GP(game).chars[i];
		names.push_back(normalizeActionName(
			agsThingName(Common::String(who.name), Common::String(who.scrname))));
	}
}

void AgsMcpBridgeManiacDeluxe::collectTeam(Common::Array<Common::String> &names,
                                           Common::Array<int> &ids) const {
	names.clear();
	ids.clear();
	if (!engineReady() || _GP(game).numcharacters < kKidCharacters)
		return;
	for (int i = 0; i < kKidCharacters; i++) {
		const CharacterInfo &who = _GP(game).chars[i];
		// A kid who was not picked is parked outside the game entirely: the
		// game keeps the other four in room -1 for the whole playthrough.
		if (who.room < 0)
			continue;
		names.push_back(normalizeActionName(
			agsThingName(Common::String(who.name), Common::String(who.scrname))));
		ids.push_back(i);
	}
}

bool AgsMcpBridgeManiacDeluxe::teamPicked() const {
	Common::Array<Common::String> names;
	Common::Array<int> ids;
	collectTeam(names, ids);
	return names.size() >= kKidSideKicks + 1;
}

Common::String AgsMcpBridgeManiacDeluxe::controllingKid() const {
	Common::Array<Common::String> names;
	Common::Array<int> ids;
	collectTeam(names, ids);
	for (uint i = 0; i < ids.size(); i++) {
		if (&_GP(game).chars[ids[i]] == _G(playerchar))
			return names[i];
	}
	return Common::String();
}

// ---------------------------------------------------------------------------
// The verb bar
// ---------------------------------------------------------------------------
//
// Nine verbs in three rows of three, and the game paints the words into the
// button sprites rather than putting them on the buttons as text - so every
// one of them is nameless as far as the engine is concerned, and the base
// class can only offer the vocabulary it guesses at.
//
// They are named here by where they are drawn. The grid is the 1987 game's,
// unchanged - this is a remake of it, and the row of verbs is the thing it
// reproduces most exactly - so left to right and top down it reads Give,
// Pick up, Use / Open, Look at, Push / Close, Talk to, Pull. Reading them off
// the layout rather than off the control numbers means a release that
// reordered its GUI is still read correctly, which is the same bargain the
// kid selection makes.
//
// Nothing says which verb is chosen: the bar lights a verb while it is
// carrying it out and goes dark again after. So `act` presses the button it
// wants before every action rather than asking whether it has to, which is a
// press a player would make anyway.
static const char *const kManiacVerbGrid[] = {
	"give",  "pick_up", "use",
	"open",  "look_at", "push",
	"close", "talk_to", "pull"
};

void AgsMcpBridgeManiacDeluxe::collectVerbGrid(Common::Array<uint> &order,
                                               const Common::Array<Verb> &buttons) const {
	order.clear();
	const uint wanted = ARRAYSIZE(kManiacVerbGrid);
	if (buttons.size() < wanted)
		return;
	// collectVerbButtons hands over every button on the bar, and the grid is
	// only part of it: the inventory's arrows and the two portraits that hand
	// control to the other kids are on there too. The grid is the left-hand
	// end of the bar, so the nine left-most buttons are the nine verbs - and
	// they are only the verbs if they really are a grid, three columns by
	// three rows.
	Common::Array<uint> byX;
	for (uint i = 0; i < buttons.size(); i++)
		byX.push_back(i);
	for (uint i = 1; i < byX.size(); i++) {
		for (uint j = i; j > 0 && buttons[byX[j - 1]].x > buttons[byX[j]].x; j--) {
			const uint swap = byX[j - 1];
			byX[j - 1] = byX[j];
			byX[j] = swap;
		}
	}
	Common::Array<uint> grid;
	for (uint i = 0; i < wanted; i++)
		grid.push_back(byX[i]);
	// Three columns and three rows, and nothing else sharing them.
	Common::Array<int> columns, rows;
	for (uint i = 0; i < grid.size(); i++) {
		bool haveX = false, haveY = false;
		for (uint c = 0; c < columns.size(); c++)
			haveX = haveX || columns[c] == buttons[grid[i]].x;
		for (uint r = 0; r < rows.size(); r++)
			haveY = haveY || rows[r] == buttons[grid[i]].y;
		if (!haveX)
			columns.push_back(buttons[grid[i]].x);
		if (!haveY)
			rows.push_back(buttons[grid[i]].y);
	}
	if (columns.size() != 3 || rows.size() != 3)
		return;
	// Left to right, top row first, which is the order the verbs read in.
	for (uint i = 1; i < grid.size(); i++) {
		for (uint j = i; j > 0; j--) {
			const Verb &a = buttons[grid[j - 1]], &b = buttons[grid[j]];
			if (!(a.y > b.y || (a.y == b.y && a.x > b.x)))
				break;
			const uint swap = grid[j - 1];
			grid[j - 1] = grid[j];
			grid[j] = swap;
		}
	}
	order = grid;
}

void AgsMcpBridgeManiacDeluxe::nameVerbButtons(Common::Array<Verb> &buttons) const {
	Common::Array<uint> order;
	collectVerbGrid(order, buttons);
	for (uint i = 0; i < order.size(); i++)
		buttons[order[i]].name = kManiacVerbGrid[i];
}

void AgsMcpBridgeManiacDeluxe::addGameVerbs(Common::Array<Verb> &verbs) const {
	// There is no Walk to on the bar: walking is what a click on the floor
	// does whatever verb is chosen, exactly as it is in the game this remakes.
	for (uint i = 0; i < verbs.size(); i++) {
		if (verbs[i].name == "walk_to")
			return;
	}
	Verb walk;
	walk.name = "walk_to";
	walk.mode = -1;
	walk.guiId = walk.controlId = -1;
	walk.x = walk.y = 0;
	verbs.push_back(walk);
}

Common::String AgsMcpBridgeManiacDeluxe::selectedVerb() const {
	// Nothing to read. The bar lights the verb it is carrying out while it is
	// carrying it out and goes dark again after - measured by pressing every
	// one of the nine and looking, with the pointer moved away each time - so
	// there is no moment at which the interface states which verb the next
	// click would use. `act` presses the button it wants every time rather
	// than asking, which is a press a player would make anyway, and `state`
	// says nothing about a current verb rather than something untrue.
	return Common::String();
}

bool AgsMcpBridgeManiacDeluxe::interfaceUp() const {
	Common::Array<ScreenPoint> buttons;
	collectSwitchButtons(buttons);
	return !buttons.empty();
}

bool AgsMcpBridgeManiacDeluxe::playerHasControl() const {
	return AgsMcpBridge::playerHasControl() && interfaceUp();
}

void AgsMcpBridgeManiacDeluxe::collectSwitchButtons(Common::Array<ScreenPoint> &out) const {
	out.clear();
	if (!engineReady() || _G(guis) == nullptr)
		return;
	for (uint g = 0; g < _GP(guis).size(); g++) {
		const AGS::Shared::GUIMain &gui = _GP(guis)[g];
		if (!gui.IsDisplayed())
			continue;
		// The verb bar is the panel that holds the inventory window; the two
		// portrait buttons are the ones drawn past its right-hand edge. Found
		// that way rather than by index, because which control is which is the
		// game's business and only their side of the inventory is stable.
		int inventoryRight = -1;
		for (int32_t ci = 0; ci < gui.GetControlCount(); ci++) {
			const AGS::Shared::GUIObject *control = gui.GetControl(ci);
			if (control != nullptr && gui.GetControlType(ci) == AGS::Shared::kGUIInvWindow)
				inventoryRight = control->X + control->GetWidth();
		}
		if (inventoryRight < 0)
			continue;
		for (int32_t ci = 0; ci < gui.GetControlCount(); ci++) {
			const AGS::Shared::GUIObject *control = gui.GetControl(ci);
			if (control == nullptr || !control->IsVisible() || !control->IsEnabled())
				continue;
			if (dynamic_cast<const AGS::Shared::GUIButton *>(control) == nullptr)
				continue;
			if (control->X < inventoryRight)
				continue;
			ScreenPoint button;
			button.x = gui.X + control->X + control->GetWidth() / 2;
			button.y = gui.Y + control->Y + control->GetHeight() / 2;
			out.push_back(button);
		}
		if (!out.empty())
			return;
	}
}

// ---------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------

void AgsMcpBridgeManiacDeluxe::snapshotPreAction() {
	AgsMcpBridge::snapshotPreAction();
	// beginStream() calls this, so a tool that drives the machine sets its
	// phase up afterwards; anything else starting an action ends it.
	_phase = kIdle;
	_slots.clear();
	_slotNames.clear();
	_slotHeard.clear();
	_wanted.clear();
	_chosen.clear();
	_pickIndex = 0;
	_escapes = 0;
	_lastEscapeFrame = 0;
	_switchWanted.clear();
	_switchFrom.clear();
	_switchButtons.clear();
	_switchTriedFrom.clear();
	_switchTriedButton.clear();
	_switchIndex = 0;
	_cast.clear();
}

void AgsMcpBridgeManiacDeluxe::failCall(const Common::String &what,
                                        const Common::String &reason) {
	_phase = kIdle;
	_chosen.clear();
	closeStreamFailure(what + ": " + reason);
}

bool AgsMcpBridgeManiacDeluxe::pumpStreamGameEarly() {
	if (_phase == kIdle)
		return false;

	// The machine is driving the game, so the stream is neither stale nor
	// stuck: hold open the per-event deadline the stream is anchored on.
	_sseLastEventFrame = _frameCounter;

	switch (_phase) {
	case kReturnToTitle: {
		Common::Array<KidSlot> slots;
		int startX = 0, startY = 0;
		if (collectKidSelectScreen(slots, startX, startY)) {
			_slots = slots;
			_startX = startX;
			_startY = startY;
			_selectRoom = roomNumber();
			_slotNames.clear();
			_slotHeard.clear();
			for (uint i = 0; i < _slots.size(); i++) {
				_slotNames.push_back(_cast[i]);
				_slotHeard.push_back(false);
			}
			_phase = kPickChoose;
			break;
		}
		if (_escapes >= kMaxDemoEscapes) {
			failCall("choose_kids", "the opening screen never came back, so there "
			         "is nothing to pick on");
			break;
		}
		if (_escapes == 0 || _frameCounter - _lastEscapeFrame >= kDemoEscapeGap) {
			injectKey(Common::KeyState(Common::KEYCODE_ESCAPE, 27));
			_lastEscapeFrame = _frameCounter;
			_escapes++;
			debug(1, "mcp: choose_kids escape %d at room %d", _escapes, roomNumber());
		}
		break;
	}

	case kPickChoose: {
		// A kid the table puts on a portrait that is already picked is in the
		// team without anything being pressed: that is the leader.
		for (uint i = 0; i < _slots.size(); i++) {
			if (!slotPicked(_slots[i]))
				continue;
			for (uint w = 0; w < _wanted.size(); w++) {
				if (_wanted[w] != _slotNames[i])
					continue;
				_wanted.remove_at(w);
				break;
			}
		}
		if (_wanted.empty()) {
			uint picked = 0;
			_chosen.clear();
			for (uint i = 0; i < _slots.size(); i++) {
				if (!slotPicked(_slots[i]))
					continue;
				picked++;
				_chosen.push_back(_slotNames[i]);
			}
			if (picked != kKidSideKicks + 1) {
				_chosen.clear();
				failCall("choose_kids", Common::String::format(
				    "the team came out %u strong, not %u - name the %u kids "
				    "joining the leader, who is always in it himself",
				    picked, kKidSideKicks + 1, kKidSideKicks));
				break;
			}
			_phase = kStartClick;
			break;
		}
		// The next portrait to press: one the table says is wanted, or - once
		// the table has been shown wrong about a slot - the next portrait the
		// game has not named yet, because the kid has to be on one of those.
		int next = -1;
		for (uint i = 0; i < _slots.size() && next < 0; i++) {
			if (slotPicked(_slots[i]) || _slotHeard[i])
				continue;
			for (uint w = 0; w < _wanted.size(); w++) {
				if (_wanted[w] == _slotNames[i])
					next = (int)i;
			}
		}
		for (uint i = 0; i < _slots.size() && next < 0; i++) {
			if (!slotPicked(_slots[i]) && !_slotHeard[i])
				next = (int)i;
		}
		if (next < 0) {
			Common::String missing, seen;
			for (uint i = 0; i < _wanted.size(); i++) {
				if (!missing.empty())
					missing += ", ";
				missing += _wanted[i];
			}
			for (uint i = 0; i < _slotNames.size(); i++) {
				if (!seen.empty())
					seen += ", ";
				seen += _slotNames[i];
			}
			failCall("choose_kids", "no portrait for " + missing +
			         " on the opening screen. Kids offered: " + seen);
			break;
		}
		_pickIndex = (uint)next;
		_phase = kPickClick;
		break;
	}

	case kPickClick:
		injectMouseClick(_slots[_pickIndex].x, _slots[_pickIndex].y, "left", false);
		_phaseFrame = _frameCounter;
		_phase = kPickRead;
		break;

	case kPickRead: {
		// The click writes the kid's line and turns the frame on in the same
		// breath, so one wait covers both. A portrait the screen turns down
		// (the team is already full) still gets its line, which is what makes
		// this readable even when nothing changed.
		const Common::String heard = shownKidName();
		if (heard.empty() && _frameCounter - _phaseFrame < kLabelFrames)
			break;
		if (_frameCounter - _phaseFrame < kClickFrames)
			break;
		if (heard.empty()) {
			failCall("choose_kids", "the opening screen said nothing about the "
			         "portrait that was clicked, so there is no telling who it is");
			break;
		}
		_slotHeard[_pickIndex] = true;
		_slotNames[_pickIndex] = heard;
		bool wanted = false;
		for (uint w = 0; w < _wanted.size(); w++) {
			if (_wanted[w] != heard)
				continue;
			wanted = true;
			_wanted.remove_at(w);
			break;
		}
		if (wanted && !slotPicked(_slots[_pickIndex])) {
			failCall("choose_kids", "'" + heard + "' would not join the team - the "
			         "screen turned the pick down");
			break;
		}
		if (!wanted && slotPicked(_slots[_pickIndex])) {
			// Only reachable once the character table has been shown wrong
			// about a slot: put the screen back the way it was and go on.
			injectMouseClick(_slots[_pickIndex].x, _slots[_pickIndex].y, "left", false);
			_phaseFrame = _frameCounter;
			_phase = kPickUndo;
			break;
		}
		_phase = kPickChoose;
		break;
	}

	case kPickUndo:
		if (_frameCounter - _phaseFrame >= kClickFrames)
			_phase = kPickChoose;
		break;

	case kStartClick:
		injectMouseClick(_startX, _startY, "left", false);
		_phaseFrame = _frameCounter;
		_phase = kStartAwait;
		break;

	case kStartAwait:
		if (roomNumber() != _selectRoom) {
			_phaseFrame = _frameCounter;
			_escapes = 0;
			_phase = _skipIntro ? kSkipIntro : kIdle;
			break;
		}
		if (_frameCounter - _phaseFrame >= kStartFrames)
			failCall("choose_kids", "the team was picked but START did not begin the game");
		break;

	case kSkipIntro:
		// Escape out of the opening, one press after another as fast as the
		// game reads them, until it puts its verb bar back - which is the game
		// itself saying the story is over and the player is in charge. Giving
		// up on the ceiling is not a failure: the game is running, it is just
		// still telling its story.
		if (interfaceUp())
			_phase = kIdle;
		else if (_frameCounter - _phaseFrame >= kIntroFrames)
			_phase = kIdle;
		else if (_escapes < kMaxEscapes &&
		         (_escapes == 0 || _frameCounter - _lastEscapeFrame >= kEscapeGap)) {
			injectKey(Common::KeyState(Common::KEYCODE_ESCAPE, 27));
			_lastEscapeFrame = _frameCounter;
			_escapes++;
		}
		break;

	case kSwitchClick: {
		const Common::String from = controllingKid();
		int button = -1;
		for (uint b = 0; b < _switchButtons.size() && button < 0; b++) {
			bool tried = false;
			for (uint t = 0; t < _switchTriedFrom.size(); t++)
				tried = tried || (_switchTriedFrom[t] == from && _switchTriedButton[t] == b);
			if (!tried)
				button = (int)b;
		}
		if (button < 0) {
			failCall("switch_character",
			         "'" + _switchWanted + "' did not take control - neither of the "
			         "verb bar's portrait buttons hands it over");
			break;
		}
		_switchFrom = from;
		_switchIndex = (uint)button;
		_switchTriedFrom.push_back(from);
		_switchTriedButton.push_back((uint)button);
		injectMouseClick(_switchButtons[_switchIndex].x, _switchButtons[_switchIndex].y,
		                 "left", false);
		_phaseFrame = _frameCounter;
		_phase = kSwitchAwait;
		break;
	}

	case kSwitchAwait: {
		const Common::String now = controllingKid();
		if (now == _switchWanted) {
			_phase = kIdle;
			break;
		}
		// A press that landed on the wrong kid is an answer, not a failure:
		// the next press is chosen from where control is *now*, which is why
		// this waits for it to have landed rather than for a clock.
		if (!now.empty() && now != _switchFrom) {
			_phase = kSwitchClick;
			break;
		}
		if (_frameCounter - _phaseFrame >= kSwitchFrames)
			_phase = kSwitchClick;
		break;
	}

	default:
		break;
	}
	return true; // the frame belongs to the machine while it runs
}

// ---------------------------------------------------------------------------
// choose_kids
// ---------------------------------------------------------------------------

bool AgsMcpBridgeManiacDeluxe::toolChooseKids(const Common::JSONValue &args,
                                              Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "choose_kids: another action is already in progress";
		return false;
	}
	if (teamPicked()) {
		errorOut = "choose_kids: the team is already in the mansion - the kids are "
		           "picked once, on the opening screen, before the game starts";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("kids") ||
	    !args.asObject()["kids"]->isArray()) {
		errorOut = "choose_kids: 'kids' (array of kid names) is required";
		return false;
	}
	const Common::JSONArray &kidsArg = args.asObject()["kids"]->asArray();
	if (kidsArg.size() < kKidSideKicks || kidsArg.size() > kKidSideKicks + 1) {
		errorOut = "choose_kids: 'kids' must name the two kids joining the leader "
		           "(naming the leader as the third is accepted)";
		return false;
	}
	Common::Array<Common::String> wanted;
	for (uint i = 0; i < kidsArg.size(); i++) {
		if (!kidsArg[i]->isString()) {
			errorOut = "choose_kids: 'kids' must be an array of names";
			return false;
		}
		const Common::String name = normalizeActionName(kidsArg[i]->asString());
		if (name.empty()) {
			errorOut = "choose_kids: kid names must not be empty";
			return false;
		}
		for (uint j = 0; j < wanted.size(); j++) {
			if (wanted[j] == name) {
				errorOut = "choose_kids: '" + name + "' is named twice";
				return false;
			}
		}
		wanted.push_back(name);
	}
	bool skipIntro = true;
	if (args.asObject().contains("skip_intro") && args.asObject()["skip_intro"]->isBool())
		skipIntro = args.asObject()["skip_intro"]->asBool();

	// Who is on which portrait, before anything is pressed: the game's own
	// character table, in the order the portraits are drawn. Every click checks
	// its own slot against this, so a release that disagreed would be caught
	// rather than believed.
	Common::Array<Common::String> cast;
	collectCast(cast);
	if (cast.size() < kKidSideKicks + 1) {
		errorOut = "choose_kids: this release does not name its kids, so there is "
		           "no telling which portrait is whose";
		return false;
	}

	beginStream();
	// After beginStream(), which clears the machine through snapshotPreAction().
	// The screen itself is read in kReturnToTitle rather than here: it may not
	// be up, and getting it back is the machine's first job.
	_cast = cast;
	_wanted = wanted;
	_skipIntro = skipIntro;
	_phaseFrame = _frameCounter;
	_phase = kReturnToTitle;
	return true;
}

// ---------------------------------------------------------------------------
// switch_character
// ---------------------------------------------------------------------------

bool AgsMcpBridgeManiacDeluxe::toolSwitchCharacter(const Common::JSONValue &args,
                                                   Common::String &errorOut) {
	if (isStreaming()) {
		errorOut = "switch_character: another action is already in progress";
		return false;
	}
	if (!playerHasControl()) {
		errorOut = "switch_character: the game is not accepting input right now";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("name") ||
	    !args.asObject()["name"]->isString()) {
		errorOut = "switch_character: 'name' (string) is required";
		return false;
	}
	const Common::String wanted = normalizeActionName(args.asObject()["name"]->asString());
	Common::Array<Common::String> names;
	Common::Array<int> ids;
	collectTeam(names, ids);
	Common::String known;
	bool found = false;
	for (uint i = 0; i < names.size(); i++) {
		found = found || names[i] == wanted;
		if (!known.empty())
			known += ", ";
		known += names[i];
	}
	if (!found) {
		errorOut = "switch_character: nobody on the team is called '" + wanted +
		           "'. The team is: " + (known.empty() ? Common::String("(nobody yet)") : known);
		return false;
	}
	if (controllingKid() == wanted) {
		errorOut = "switch_character: '" + wanted + "' already has control";
		return false;
	}
	Common::Array<ScreenPoint> buttons;
	collectSwitchButtons(buttons);
	if (buttons.empty()) {
		errorOut = "switch_character: the verb bar is not up, so there is nothing "
		           "to switch with";
		return false;
	}

	beginStream();
	_switchWanted = wanted;
	_switchButtons = buttons;
	_switchIndex = 0;
	_phaseFrame = _frameCounter;
	_phase = kSwitchClick;
	return true;
}

} // End of namespace AGS3
