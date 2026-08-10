/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "scumm/actor.h"
#include "scumm/mcp_subclasses.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/scumm_v0.h"
#include "scumm/verbs.h"

namespace Scumm {

using Networking::mcpJsonString;
using Networking::mcpProp;
using Networking::mcpObjectSchema;

// Game/version-specific MCP bridge logic for the V0 SCUMM family (Maniac
// Mansion). Migrated out of mcp.cpp via the ScummMcpBridge virtual hooks.

// ---------------------------------------------------------------------------
// McpBridgeManiac — Maniac Mansion (kid selection, kid switching, dial pad)
// ---------------------------------------------------------------------------

void McpBridgeManiac::registerGameTools() {
	// --- switch_character ---
	// V0 (C64/Apple II) maps F1-F3 to switchActor(slot)/VAR(97+slot); the V1/V2
	// ports use the in-game "New Kid" verb but share the same ego/kid vars, so
	// the tool drives the switch directly for them.
	{
		Common::JSONObject props;
		props.setVal("name", mcpProp("string",
		    "Name of the kid to control, as listed in state.available_characters (e.g. 'dave')."));
		const char *req[] = {"name"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "switch_character";
		spec.description =
		    "Switch the player-controlled kid (the F1-F3 keys in Maniac Mansion). "
		    "state lists the available names in 'available_characters' and the "
		    "current one in 'controlling'. Only allowed during normal gameplay (not "
		    "in a cutscene and not while kid switching is disabled). Blocks until "
		    "the switch settles, then returns state changes — room_changed/position "
		    "reflect the newly controlled kid.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- dial (phone keypad) ---
	{
		Common::JSONObject props;
		props.setVal("number", mcpProp("string",
		    "The number to dial, as a string of keypad keys: digits 0-9 plus "
		    "'*' and '#' (e.g. '1234')."));
		const char *req[] = {"number"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "dial";
		spec.description =
		    "Dial a number on the phone dial pad in Maniac Mansion. Only valid "
		    "while the dial pad is on screen (use the phone first via "
		    "act(verb='use', target1='phone')). Presses the keypad buttons one "
		    "at a time, blocks until the sequence (and any resulting call) "
		    "settles, then returns state changes.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}

	// --- choose_kids (title screen: pick the team, then START) ---
	{
		Common::JSONObject props;
		Common::JSONObject kids;
		kids.setVal("type",  mcpJsonString("array"));
		kids.setVal("items", mcpProp("string"));
		kids.setVal("description", mcpJsonString(
		    "The three heroes, by name: the two kids joining Dave, or all three "
		    "with Dave named as well. The cast is dave, syd, michael, wendy, "
		    "bernard, razor and jeff."));
		props.setVal("kids", new Common::JSONValue(kids));
		props.setVal("skip_intro", mcpProp("boolean",
		    "Escape through the opening cutscene so the call returns with the "
		    "player already in control (default true)."));
		const char *req[] = {"kids"};
		Networking::McpServer::ToolSpec spec;
		spec.name = "choose_kids";
		spec.description =
		    "Pick the three heroes on the Maniac Mansion title screen and start "
		    "the game. Dave always leads the rescue, so name the two kids who "
		    "join him (naming Dave as the third is accepted). Each portrait is "
		    "clicked in turn and identified by the line the game prints for it, "
		    "so the kids end up selected whatever order the portraits are in; "
		    "then START is pressed. Only valid while the title screen is up, "
		    "before the game has started. Blocks until the game has begun (by "
		    "default escaping through the intro until the player has control) "
		    "and returns state changes, with the team in 'kids'.";
		spec.inputSchema  = mcpObjectSchema(props, req, 1);
		spec.outputSchema = buildChangesSchema();
		spec.streaming    = true;
		_server->registerTool(spec);
	}
}

Common::JSONValue *McpBridgeManiac::dispatchGameTool(const Common::String &name,
                                                     const Common::JSONValue &args,
                                                     Common::String &errorOut, bool &handled) {
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
	if (name == "dial") {
		handled = true;
		toolDial(args, errorOut);
		return nullptr; // streaming
	}
	return McpBridgeV0::dispatchGameTool(name, args, errorOut, handled);
}

void McpBridgeManiac::augmentStateSchema(Common::JSONObject &outputProps) {
	// Keep the SCUMM-wide fields (can_act) declared: state emits them for every
	// game, and the schema is closed (additionalProperties: false), so dropping
	// the base call makes every state result fail validation on a strict client.
	McpBridgeV0::augmentStateSchema(outputProps);
	outputProps.setVal("controlling", mcpProp("string", "Name of the currently controlled kid"));
	Common::JSONObject arr;
	arr.setVal("type",  mcpJsonString("array"));
	arr.setVal("items", mcpProp("string"));
	outputProps.setVal("available_characters", new Common::JSONValue(arr));
	outputProps.setVal("kid_selection_pending", mcpProp("boolean",
	    "True while the title screen is waiting for the three heroes to be "
	    "picked; use the choose_kids tool to pick them and start the game"));
}

void McpBridgeManiac::augmentChangesSchema(Common::JSONObject &props) {
	McpBridgeV0::augmentChangesSchema(props);
	props.setVal("kids", mcpProp("array",
	    "Names of the heroes choose_kids put in the team, as the game named them"));
}

void McpBridgeManiac::augmentState(Common::JSONObject &out) {
	// The title screen has no verbs and no named objects, so flag it: without
	// this an agent booting the game has no way to tell that the row of unnamed
	// objects it is looking at is the kid selection.
	{
		Common::Array<KidPortrait> portraits;
		KidPortrait startButton = {0, 0, 0};
		if (collectKidSelectScreen(portraits, startButton))
			out.setVal("kid_selection_pending", Networking::mcpJsonBool(true));
	}

	// Expose the switchable kids and the current one so clients can drive the
	// switch_character tool by name.
	Common::Array<ManiacKid> kids;
	collectManiacKids(kids);
	if (kids.empty())
		return;
	int egoNum = (_vm->VAR_EGO != 0xFF) ? (int)_vm->VAR(_vm->VAR_EGO) : -1;
	Common::JSONArray charArr;
	for (uint i = 0; i < kids.size(); ++i) {
		charArr.push_back(mcpJsonString(kids[i].name));
		if (kids[i].actorId == egoNum)
			out.setVal("controlling", mcpJsonString(kids[i].name));
	}
	out.setVal("available_characters", new Common::JSONValue(charArr));
}

void McpBridgeManiac::pumpStreamGame() {
	// Phone dial pad: press the queued keypad buttons one at a time. Wait for the
	// previous press's sentence to dispatch (the keypad scripts run without
	// walking) and leave a few frames between presses so each button script
	// finishes before the next begins.
	const uint32 kDialSpacingFrames = 12;
	if (!_ssePendingDialObjs.empty() && _vm->_sentenceNum == 0 &&
	    (_sseLastDialFedFrame == 0
	     || _frameCounter - _sseLastDialFedFrame >= kDialSpacingFrames)) {
		int obj = _ssePendingDialObjs[0];
		_ssePendingDialObjs.remove_at(0);
		_sseLastDialFedFrame = _frameCounter;
		_sseLastEventFrame = _frameCounter;
		vmDoSentence(_sseDialVerbId, obj, 0);
	}
}

void McpBridgeManiac::augmentStateChanges(Common::JSONObject &changes) const {
	if (_kidChosen.empty())
		return;
	Common::JSONArray kids;
	for (uint i = 0; i < _kidChosen.size(); ++i)
		kids.push_back(mcpJsonString(_kidChosen[i]));
	changes.setVal("kids", new Common::JSONValue(kids));
}

void McpBridgeManiac::resetGameStream() {
	_ssePendingDialObjs.clear();
	_sseLastDialFedFrame = 0;
	_kidPhase = kKidIdle;
	_kidPortraits.clear();
	_kidWanted.clear();
	_kidNamesSeen.clear();
	_kidChosen.clear();
	_kidProbeIndex = 0;
	_kidClearIndex = 0;
	_kidSelected = 0;
	_kidMsgMark = 0;
	_kidEscapes = 0;
	_kidLastEscapeFrame = 0;
}

bool McpBridgeManiac::gameStreamBusy() const {
	return !_ssePendingDialObjs.empty() || _kidPhase != kKidIdle;
}

// ---------------------------------------------------------------------------
// choose_kids — the title screen: pick the three heroes, then press START
// ---------------------------------------------------------------------------

bool McpBridgeManiac::collectKidSelectScreen(Common::Array<KidPortrait> &portraits,
                                             KidPortrait &startButton) const {
	portraits.clear();
	if (!_vm->_objs)
		return false;
	// The title screen is the one screen the ego is not in: until the game
	// starts the kids exist only as portrait objects.
	Actor *ego = getEgoActor();
	if (!ego || (int)ego->_room == (int)_vm->_currentRoom)
		return false;

	Common::Array<int> objIdx;
	for (int i = 1; i < vmNumLocalObjects(); ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr || od.width <= 0 || od.height <= 0)
			continue;
		objIdx.push_back(i);
	}

	// The portraits are the largest set of identically sized objects sharing a
	// row; anything else on the screen (START) is the odd one out.
	int best = -1;
	uint bestCount = 0;
	for (uint i = 0; i < objIdx.size(); ++i) {
		const ObjectData &od = _vm->_objs[objIdx[i]];
		uint count = 0;
		for (uint j = 0; j < objIdx.size(); ++j) {
			const ObjectData &o2 = _vm->_objs[objIdx[j]];
			if (o2.width == od.width && o2.height == od.height && o2.y_pos == od.y_pos)
				++count;
		}
		if (count > bestCount) {
			bestCount = count;
			best = objIdx[i];
		}
	}
	// Maniac Mansion offers seven; keep some slack rather than pin the number.
	const uint kMinPortraits = 4;
	if (best < 0 || bestCount < kMinPortraits)
		return false;

	const ObjectData &ref = _vm->_objs[best];
	int startIdx = -1;
	for (uint i = 0; i < objIdx.size(); ++i) {
		const ObjectData &od = _vm->_objs[objIdx[i]];
		if (od.width == ref.width && od.height == ref.height && od.y_pos == ref.y_pos) {
			KidPortrait p;
			p.obj = od.obj_nr;
			p.x   = od.x_pos + od.width  / 2;
			p.y   = od.y_pos + od.height / 2;
			portraits.push_back(p);
			continue;
		}
		// START sits above the row: if the screen ever holds more than one
		// leftover object, the topmost is the one to press.
		if (startIdx < 0 || od.y_pos < _vm->_objs[startIdx].y_pos)
			startIdx = objIdx[i];
	}
	if (startIdx < 0)
		return false;
	{
		const ObjectData &od = _vm->_objs[startIdx];
		startButton.obj = od.obj_nr;
		startButton.x   = od.x_pos + od.width  / 2;
		startButton.y   = od.y_pos + od.height / 2;
	}

	// Left to right, so the sweep runs the way the screen reads.
	for (uint i = 0; i + 1 < portraits.size(); ++i)
		for (uint j = 0; j + 1 < portraits.size() - i; ++j)
			if (portraits[j].x > portraits[j + 1].x) {
				KidPortrait t = portraits[j];
				portraits[j] = portraits[j + 1];
				portraits[j + 1] = t;
			}
	return true;
}

void McpBridgeManiac::clickKidScreenObject(const KidPortrait &target) {
	VirtScreen *vs = &_vm->_virtscr[kMainVirtScreen];
	int mouseX = target.x - vs->xstart;
	int mouseY = target.y + vs->topline;
	if (mouseX < 0) mouseX = 0;
	if (mouseX > _vm->_screenWidth - 1)  mouseX = _vm->_screenWidth - 1;
	if (mouseY < 0) mouseY = 0;
	if (mouseY > _vm->_screenHeight - 1) mouseY = _vm->_screenHeight - 1;
	// checkExecVerbs() routes the click by _mouse (which virtual screen was hit),
	// while the input script looks the object up through VAR_VIRT_MOUSE_X/Y —
	// which V0-V2 rebuild from _virtualMouse every frame, shifted into their
	// coarser coordinate grid. So the room-pixel point goes into _virtualMouse
	// and the vars are left to the engine.
	vmMouse().x        = mouseX;
	vmMouse().y        = mouseY;
	vmVirtualMouse().x = target.x;
	vmVirtualMouse().y = target.y;
	vmLeftBtnPressed() |= 0x03; // msClicked | msDown
	_sseButtonClearFrame = _frameCounter + 2;
}

bool McpBridgeManiac::kidNameFromMessages(uint fromIndex, Common::String &nameOut) const {
	for (uint i = fromIndex; i < _sseMessages.size(); ++i) {
		Common::String text = MCP::mcpCleanGameText(safeUtf8(_sseMessages[i].text));
		// A portrait's line reads "<Name> - <blurb>"; the prompt the screen
		// repeats while it waits ("Please select two other kids.") has no dash,
		// which is exactly what tells the two apart.
		uint dash = 0;
		for (uint c = 1; c + 1 < text.size(); ++c) {
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
		nameOut = normalizeActionName(name);
		return true;
	}
	return false;
}

void McpBridgeManiac::failKidSelection(const Common::String &reason) {
	_kidPhase = kKidIdle;
	_kidChosen.clear();
	closeStreamFailure("choose_kids: " + reason);
}

bool McpBridgeManiac::pumpStreamGameEarly() {
	if (_kidPhase == kKidIdle)
		return false;

	// Release a simulated click a couple of frames after it went out. The generic
	// pumpStreamMid() does this, but the frame ends here while the machine runs.
	if (_sseButtonClearFrame != 0 && _frameCounter >= _sseButtonClearFrame) {
		vmLeftBtnPressed() &= ~0x01; // clear msDown
		_sseButtonClearFrame = 0;
	}
	// The machine is driving the game, so the stream is neither stale nor stuck:
	// hold open the per-event deadline toolChooseKids anchored here.
	_sseLastEventFrame = _frameCounter;

	const uint32 kBlurbFrames    = 90;   // longest wait for a portrait's line
	const uint32 kClickSpacing   = 12;   // frames between two clicks
	const uint32 kStartFrames    = 240;  // START -> the game leaves the title screen
	const uint32 kIntroFrames    = 7200; // ceiling on sitting through the intro
	const uint32 kEscapeSpacing  = 60;
	const int    kMaxEscapes     = 12;

	switch (_kidPhase) {
	case kKidClearSelected:
		// A retry can find kids an earlier attempt selected still switched on,
		// and the sweep would then click a wanted one *off*. Start clean.
		while (_kidClearIndex < _kidPortraits.size() &&
		       !(vmGetState(_kidPortraits[_kidClearIndex].obj) & kObjectStateIntrinsic))
			++_kidClearIndex;
		if (_kidClearIndex >= _kidPortraits.size()) {
			_kidPhase = kKidClickPortrait;
			break;
		}
		clickKidScreenObject(_kidPortraits[_kidClearIndex]);
		++_kidClearIndex;
		_kidPhaseFrame = _frameCounter;
		_kidPhase = kKidAwaitClear;
		break;

	case kKidAwaitClear:
		if (_frameCounter - _kidPhaseFrame >= kClickSpacing)
			_kidPhase = kKidClearSelected;
		break;

	case kKidClickPortrait:
		if (_kidWanted.empty()) {
			_kidPhase = kKidClickStart;
			break;
		}
		if (_kidProbeIndex >= _kidPortraits.size()) {
			Common::String missing, seen;
			for (uint i = 0; i < _kidWanted.size(); ++i) {
				if (!missing.empty()) missing += ", ";
				missing += _kidWanted[i];
			}
			for (uint i = 0; i < _kidNamesSeen.size(); ++i) {
				if (!seen.empty()) seen += ", ";
				seen += _kidNamesSeen[i];
			}
			failKidSelection("no portrait for " + missing + " on the title screen. Kids offered: " +
			                 (seen.empty() ? Common::String("(none identified)") : seen));
			break;
		}
		clickKidScreenObject(_kidPortraits[_kidProbeIndex]);
		_kidMsgMark = _sseMessages.size();
		_kidPhaseFrame = _frameCounter;
		_kidPhase = kKidAwaitPortrait;
		break;

	case kKidAwaitPortrait: {
		Common::String name;
		bool identified = kidNameFromMessages(_kidMsgMark, name);
		if (!identified && _frameCounter - _kidPhaseFrame < kBlurbFrames)
			break; // the line has not been printed yet
		const KidPortrait &portrait = _kidPortraits[_kidProbeIndex];
		bool selected = (vmGetState(portrait.obj) & kObjectStateIntrinsic) != 0;
		bool wanted = false;
		if (identified) {
			_kidNamesSeen.push_back(name);
			for (uint i = 0; i < _kidWanted.size(); ++i) {
				if (_kidWanted[i] != name)
					continue;
				wanted = true;
				_kidWanted.remove_at(i);
				break;
			}
		}
		if (wanted) {
			if (selected) {
				++_kidSelected;
			} else if (_kidSelected >= kKidSideKicks) {
				// The click was turned down: the team is already full, so this kid
				// cannot be in it as well.
				failKidSelection("only " + Common::String::format("%d", kKidSideKicks) +
				                 " kids can join the leader, so '" + name +
				                 "' cannot be picked as well");
				break;
			}
			// Not selected and there is still room: this is the leader (Dave),
			// who is in the team from the start and cannot be switched off.
			_kidChosen.push_back(name);
			++_kidProbeIndex;
			_kidPhase = kKidClickPortrait;
			break;
		}
		if (selected) {
			// Not one of ours — click it away again before moving on.
			clickKidScreenObject(portrait);
			_kidPhaseFrame = _frameCounter;
			_kidPhase = kKidAwaitDeselect;
			break;
		}
		++_kidProbeIndex;
		_kidPhase = kKidClickPortrait;
		break;
	}

	case kKidAwaitDeselect:
		if (_frameCounter - _kidPhaseFrame < kClickSpacing)
			break;
		++_kidProbeIndex;
		_kidPhase = kKidClickPortrait;
		break;

	case kKidClickStart:
		if (_kidSelected < kKidSideKicks) {
			// Every name was found, but they do not add up to a team — the caller
			// named the leader (who is in it anyway) instead of a second kid.
			failKidSelection("the team is one kid short — name the "
			                 + Common::String::format("%d", kKidSideKicks) +
			                 " kids joining the leader, who is always in it himself");
			break;
		}
		clickKidScreenObject(_kidStartButton);
		_kidPhaseFrame = _frameCounter;
		_kidPhase = kKidAwaitStart;
		break;

	case kKidAwaitStart:
		if ((int)_vm->_currentRoom != _kidSelectRoom) {
			// The game has begun: either hand back here (the generic pump closes
			// the stream on the room change) or sit through the intro.
			_kidPhaseFrame = _frameCounter;
			_kidPhase = _kidSkipIntro ? kKidSkipIntro : kKidIdle;
			break;
		}
		if (_frameCounter - _kidPhaseFrame >= kStartFrames)
			failKidSelection("the team was picked but START did not begin the game");
		break;

	case kKidSkipIntro:
		// Escape out of the opening cutscene, exactly like the skip tool does,
		// until the game hands control over. Giving up on the ceiling is not a
		// failure — the game is running, it is just still telling its story.
		if (vmUserPut() > 0 || _frameCounter - _kidPhaseFrame >= kIntroFrames) {
			_kidPhase = kKidIdle;
			break;
		}
		if (_kidEscapes < kMaxEscapes &&
		    (_kidEscapes == 0 || _frameCounter - _kidLastEscapeFrame >= kEscapeSpacing)) {
			injectKey(Common::KeyState(Common::KEYCODE_ESCAPE));
			_kidLastEscapeFrame = _frameCounter;
			++_kidEscapes;
		}
		break;

	default:
		break;
	}
	return true; // the frame belongs to the machine while it runs
}

bool McpBridgeManiac::toolChooseKids(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "choose_kids: another action is already in progress";
		return false;
	}
	if (vmUserPut() <= 0) {
		errorOut = "choose_kids: game is not accepting input right now";
		return false;
	}
	Common::Array<KidPortrait> portraits;
	KidPortrait startButton = {0, 0, 0};
	if (!collectKidSelectScreen(portraits, startButton)) {
		errorOut = "choose_kids: the kid selection is not on screen — it is only "
		           "offered on the title screen, before the game starts";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("kids") ||
	    !args.asObject()["kids"]->isArray()) {
		errorOut = "choose_kids: 'kids' (array of kid names) is required";
		return false;
	}
	const Common::JSONArray &kidsArg = args.asObject()["kids"]->asArray();
	if (kidsArg.size() < (uint)kKidSideKicks || kidsArg.size() > (uint)kKidSideKicks + 1) {
		errorOut = "choose_kids: 'kids' must name the two kids joining the leader "
		           "(naming the leader as the third is accepted)";
		return false;
	}
	Common::Array<Common::String> wanted;
	for (uint i = 0; i < kidsArg.size(); ++i) {
		if (!kidsArg[i]->isString()) {
			errorOut = "choose_kids: 'kids' must be an array of names";
			return false;
		}
		Common::String name = normalizeActionName(kidsArg[i]->asString());
		if (name.empty()) {
			errorOut = "choose_kids: kid names must not be empty";
			return false;
		}
		for (uint j = 0; j < wanted.size(); ++j) {
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

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;
	_sseButtonClearFrame = 0;
	// Set up after snapshotPreAction (which clears the machine); pumpStream drives
	// it from the next frame on. The sweep and the intro both outlast the normal
	// start-anchored deadline, so switch to the per-event one — the machine bumps
	// _sseLastEventFrame every frame it is working.
	_kidPortraits = portraits;
	_kidStartButton = startButton;
	_kidWanted = wanted;
	_kidSkipIntro = skipIntro;
	_kidSelectRoom = (int)_vm->_currentRoom;
	_kidPhaseFrame = _frameCounter;
	_kidPhase = kKidClearSelected;
	_sseAllowLongCutscene = true;
	_server->startStreaming();
	return true;
}

void McpBridgeManiac::collectManiacKids(Common::Array<ManiacKid> &out) const {
	out.clear();
	// V0's F1-F3 handler maps slot N to the actor stored in VAR(97+N) (see
	// ScummEngine_v0::switchActor). The V1/V2 ports (the full DOS game) keep the
	// team in VAR(47..49) instead — slot 0 is the leader picked on the title
	// screen (Dave), slots 1 and 2 the two kids chosen to join him. Slots
	// holding no valid actor are skipped, so on a variant where these vars are
	// unused the list simply comes out empty.
	const int kidVarBase = (_vm->_game.version == 0) ? 97 : 47;
	for (int slot = 0; slot < 3; ++slot) {
		int actorId = (int)_vm->VAR(kidVarBase + slot);
		if (actorId <= 0 || !_vm->isValidActor(actorId)) continue;
		ManiacKid kid;
		kid.slot = slot;
		kid.actorId = actorId;
		Common::String name = objName(vmActorToObj(actorId));
		kid.name = name.empty() ? Common::String::format("actor-%d", actorId)
		                        : normalizeActionName(safeUtf8(name));
		out.push_back(kid);
	}
}

bool McpBridgeManiac::toolSwitchCharacter(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "switch_character: another action is already in progress";
		return false;
	}
	if (vmUserPut() <= 0) {
		errorOut = "switch_character: game is not accepting input right now";
		return false;
	}
	// V0: mirror switchActor()'s own gate so the client gets an error instead
	// of a silent no-op when switching is disallowed (cutscene, keypad, lab
	// door). V1/V2 have no equivalent mode byte; _userPut covers them above.
	if (_vm->_game.version == 0 && !v0InNormalMode()) {
		errorOut = "switch_character: switching is not allowed right now (cutscene or kid switching disabled)";
		return false;
	}
	if (!args.isObject()) {
		errorOut = "switch_character: arguments must be an object with a 'name' field";
		return false;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("name") || !a["name"]->isString()) {
		errorOut = "switch_character: 'name' (string) is required";
		return false;
	}

	Common::Array<ManiacKid> kids;
	collectManiacKids(kids);
	Common::String wanted = normalizeActionName(a["name"]->asString());
	const ManiacKid *match = nullptr;
	Common::String available;
	for (uint i = 0; i < kids.size(); ++i) {
		if (!available.empty()) available += ", ";
		available += kids[i].name;
		if (kids[i].name == wanted) match = &kids[i];
	}
	if (!match) {
		errorOut = "switch_character: unknown character '" + a["name"]->asString() +
		           "'. Available: " + (available.empty() ? "(none)" : available);
		return false;
	}

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;
	_sseButtonClearFrame = 0;
	if (_vm->_game.version == 0) {
		v0SwitchActor(match->slot);
	} else {
		// V1/V2: no engine-side helper exists (the original ports switch via
		// the "New Kid" verb script), so replicate V0's switchActor() body.
		vmResetSentence();
		_vm->VAR(_vm->VAR_EGO) = match->actorId;
		vmActorFollowCamera(match->actorId);
	}
	_server->startStreaming();
	return true;
}

bool McpBridgeManiac::toolDial(const Common::JSONValue &args, Common::String &errorOut) {
	if (_streaming) {
		errorOut = "dial: another action is already in progress";
		return false;
	}
	if (vmUserPut() <= 0) {
		errorOut = "dial: game is not accepting input right now";
		return false;
	}
	if (!args.isObject() || !args.asObject().contains("number") ||
	    !args.asObject()["number"]->isString()) {
		errorOut = "dial: 'number' (string of keypad keys, e.g. '1234') is required";
		return false;
	}
	Common::String number = args.asObject()["number"]->asString();
	number.trim();
	if (number.empty() || number.size() > 16) {
		errorOut = "dial: 'number' must contain 1-16 keypad keys";
		return false;
	}
	for (uint i = 0; i < number.size(); ++i) {
		char c = number[i];
		if (!(c >= '0' && c <= '9') && c != '*' && c != '#') {
			errorOut = Common::String::format("dial: invalid keypad key '%c' (use 0-9, * or #)", c);
			return false;
		}
	}
	// V0 tracks the dial pad (and other selection screens) via _currentMode.
	// V1/V2 have no mode byte; for them the button-map scan below is the gate.
	if (_vm->_game.version == 0 && !v0InKeypadMode()) {
		errorOut = "dial: no dial pad on screen — use the phone first (act verb='use' target1='phone')";
		return false;
	}

	// Build the key -> button-object map for the current room.
	// Strategy 1: buttons named after their key ("1".."9", "0", "*", "#").
	// Strategy 2: the C64 demo's buttons are unnamed — but the pad is exactly
	// 12 equal-sized button objects in the standard 3x4 phone grid, so sort
	// them row-major and assign the layout by position. (Object 427 carries
	// the name "6" in the demo and lands on '6' this way, confirming the
	// mapping.)
	static const char kDialPadLayout[] = "123456789*0#";
	struct DialButton { int obj; int x; int y; };
	Common::Array<DialButton> grid;
	int gridObjForKey[12] = {};
	int nameObjForKey[12] = {};
	auto layoutIndex = [](char c) -> int {
		for (int k = 0; k < 12; ++k)
			if (kDialPadLayout[k] == c) return k;
		return -1;
	};
	{
		// Dominant button size among the room objects.
		int bestW = 0, bestH = 0, bestCount = 0;
		for (int i = 1; _vm->_objs && i < vmNumLocalObjects(); ++i) {
			const ObjectData &od = _vm->_objs[i];
			if (!od.obj_nr || od.width <= 0 || od.height <= 0) continue;
			int cnt = 0;
			for (int j = 1; j < vmNumLocalObjects(); ++j) {
				const ObjectData &o2 = _vm->_objs[j];
				if (o2.obj_nr && o2.width == od.width && o2.height == od.height) ++cnt;
			}
			if (cnt > bestCount) { bestCount = cnt; bestW = od.width; bestH = od.height; }
		}
		for (int i = 1; _vm->_objs && i < vmNumLocalObjects(); ++i) {
			const ObjectData &od = _vm->_objs[i];
			if (!od.obj_nr) continue;
			// Named buttons map directly regardless of geometry.
			Common::String nm = objName(od.obj_nr);
			nm.trim();
			if (nm.size() == 1) {
				int k = layoutIndex(nm[0]);
				if (k >= 0 && !nameObjForKey[k]) nameObjForKey[k] = od.obj_nr;
			}
			if (od.width == bestW && od.height == bestH) {
				DialButton b;
				b.obj = od.obj_nr;
				b.x = od.x_pos;
				b.y = od.y_pos;
				grid.push_back(b);
			}
		}
		if (grid.size() == 12) {
			// Row-major sort (top-to-bottom, left-to-right).
			for (uint i = 0; i + 1 < grid.size(); ++i)
				for (uint j = 0; j + 1 < grid.size() - i; ++j)
					if (grid[j].y > grid[j + 1].y ||
					    (grid[j].y == grid[j + 1].y && grid[j].x > grid[j + 1].x)) {
						DialButton t = grid[j]; grid[j] = grid[j + 1]; grid[j + 1] = t;
					}
			for (int k = 0; k < 12; ++k)
				gridObjForKey[k] = grid[(uint)k].obj;
		}
	}

	Common::Array<int> presses;
	for (uint i = 0; i < number.size(); ++i) {
		int k = layoutIndex(number[i]);
		int obj = nameObjForKey[k] ? nameObjForKey[k] : gridObjForKey[k];
		if (!obj) {
			errorOut = Common::String::format(
			    "dial: could not locate the '%c' button — is the dial pad on screen?", number[i]);
			return false;
		}
		presses.push_back(obj);
	}

	// The keypad buttons respond to the push verb: V0's input handler forces
	// _activeVerb = kVerbPush while in keypad mode; for V1/V2 resolve the verb
	// from the verb bar like act() does.
	int pushVerb = kVerbPush;
	if (_vm->_game.version != 0 && !resolveVerb("push", pushVerb)) {
		errorOut = "dial: could not resolve the 'push' verb";
		return false;
	}

	snapshotPreAction();
	_streaming = true;
	_sseAnswerStream = false;
	_sseStartFrame = _frameCounter;
	_sseDoneAtFrame = 0;
	_sseStuckAtFrame = 0;
	_sseLastEventFrame = 0;
	_sseEgoMoved = false;
	_sseMessages.clear();
	_ssePendingSecondClick = false;
	_ssePendingNotes.clear();
	_sseTargetObject = 0;
	_sseButtonClearFrame = 0;
	// Queue after snapshotPreAction (which clears the dial queue); pumpStream
	// feeds one press per spacing window starting next frame.
	_ssePendingDialObjs = presses;
	_sseDialVerbId = pushVerb;
	_sseLastDialFedFrame = 0;
	_server->startStreaming();
	return true;
}

} // End of namespace Scumm
