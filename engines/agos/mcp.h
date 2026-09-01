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

#ifndef AGOS_MCP_H
#define AGOS_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace AGOS {

class AGOSEngine;
struct HitArea;
struct Item;

// MCP bridge for the AGOS engine (Simon the Sorcerer).
//
// The friendliest of these engines to read. Everything a player can click is a
// hit area with an item behind it, and the item carries the name the game
// writes along the bottom of the screen while the pointer rests on it - so
// unlike SCI or Kyrandia there is no guessing at what a thing is called: the
// bridge publishes the same words the player sees.
//
// The verbs are a bar of twelve, the same twelve in every game this engine
// runs, and they are chosen the way a player chooses them: by clicking the
// word on the bar and then clicking the thing. So `act` is two clicks, played
// out over several frames so the scripts' own polling sees each of them
// arrive, and `state` lists all twelve verbs - an agent never has to guess
// which words this game takes.
//
// Pumping: the engine's whole loop is waitForInput() / handleVerbClicked() /
// delay(), and every blocking wait inside it goes through delay() as well. So
// delay() is the one choke point, and the frame counter is advanced there at
// most once per kFrameMs so the streaming budgets keep their meaning however
// hot the calling loop is.
class AgosMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static AgosMcpBridge *create(AGOSEngine *vm);

	explicit AgosMcpBridge(AGOSEngine *vm);
	~AgosMcpBridge() override;

	// From AGOSEngine::delay(), which every loop in this engine goes through.
	// Advances the frame counter at most once per kFrameMs; other calls only
	// service the transport.
	void pumpFromDelay();

	// Every line the game shows: the sentence bar, spoken lines, subtitles.
	void onGameText(const Common::String &text);

protected:
	// --- Tools --------------------------------------------------------------
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut) override;

	Common::String stateToolDescription() const override;
	Common::String actToolDescription() const override;
	Common::String walkToolDescription() const override;
	Common::String debugToolDescription() const override;

	bool usesTypedInput() const override { return false; }
	bool usesDialogQuestions() const override { return false; }

	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;
	void augmentActSchema(Common::JSONObject &props) override;

	// --- Input injection ----------------------------------------------------
	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	// --- Text ---------------------------------------------------------------
	int currentRoomForMessages() const override;

	// --- Streaming ----------------------------------------------------------
	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	bool streamRoomChanged() const override;
	void pumpStreamTrack() override;
	void pumpGame() override;

	// The frame counter advances at most once per kFrameMs (~25 a second).
	uint32 minStreamFrames() const override { return 4; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 45 : 10; }
	uint32 timeoutFrames() const override { return 250; }
	uint32 absoluteTimeoutFrames() const override { return 2500; }
	uint32 settleFrames() const override { return 14; }
	uint32 wallClockTimeoutMs() const override { return 180000; }
	uint32 wallClockCloseMs() const override { return _skipStream ? kSkipMs : 0; }
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// Something an agent can name: a hit area with an item behind it.
	struct Target {
		Common::String name;
		Common::String label;   // the words the game itself shows for it
		int x, y;               // where to click, in game coordinates
		uint16 hitAreaId;
	};

	// A queued step of synthetic input, played out one frame at a time so the
	// scripts' own polling sees each press and release. A verb is chosen by
	// clicking the word on the bar, so an action is two of these.
	enum StepKind {
		kStepHover,   // move to (x, y) and let the game notice
		kStepClick,   // press and release where the pointer is
		kStepSettle   // let the game act on what it was just told
	};
	struct Step {
		StepKind kind;
		int x, y;
		uint32 notBeforeFrame;
	};

	static const uint32 kFrameMs = 40;
	static const uint32 kSkipMs = 1500;
	// Frames between the steps of an action, so a click is never sent before
	// the hover that should precede it has been taken.
	static const uint32 kStepFrames = 3;

	bool engineReady() const;
	bool playerHasControl() const;
	int roomNumber() const;

	// The words the game shows for an item, or an empty string when it has
	// none. This is the same lookup displayName() makes when the pointer
	// rests on something.
	Common::String itemLabel(const Item *item) const;

	// Everything clickable in the scene, named and disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// What the player is carrying, which in this engine is the children of
	// the player item.
	void collectInventory(Common::Array<Target> &out) const;
	bool resolveTarget(const Common::String &name, Target &out,
	                   Common::String &errorOut) const;

	// Where on screen the verb at `index` is written, so it can be clicked.
	// False when this game has no bar showing.
	bool verbButtonPosition(int index, int &x, int &y) const;

	void queueStep(StepKind kind, int x, int y, uint32 delayFrames);

	AGOSEngine *_vm;

	bool _inPump;
	bool _skipStream;
	uint32 _lastFrameMs;

	Common::Array<Step> _steps;

	// Pre-action snapshot, for the changes an action reports.
	int _ssePreRoom;
	Common::Array<Common::String> _ssePreTargets;
	Common::Array<Common::String> _ssePreInventory;
	int _sseTrackRoom;
	uint _sseTrackSteps;
};

} // End of namespace AGOS

#endif
