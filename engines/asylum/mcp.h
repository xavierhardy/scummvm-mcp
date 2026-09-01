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

#ifndef ASYLUM_MCP_H
#define ASYLUM_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Asylum {

class AsylumEngine;

// MCP bridge for Sanitarium (the asylum engine).
//
// A pointer game with no verb bar and no verbs: the cursor changes shape to
// say what a click would do, and clicking is the whole vocabulary. So `act`
// is a click, and the two buttons are the difference between doing a thing and
// looking at it.
//
// What Sanitarium has that most of these games do not is a name for every
// object in its own data - not a label the player is shown, but the name its
// authors typed in their editor ("DOOR TO HALLWAY"). That is what the bridge
// publishes, filtered: the game ships hundreds of objects called "0" or "xxx"
// that are scenery the scripts push around, and offering those as things to
// try is offering noise.
//
// Pumping: the engine's loop is run() calling handleEvents() every pass, and
// handleEvents() is also reached from the menu, the video player and the
// puzzles - every screen the game has. So it is the one choke point, and the
// frame counter is advanced there at most once per kFrameMs so the streaming
// budgets keep their meaning however hot the caller is.
class AsylumMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static AsylumMcpBridge *create(AsylumEngine *vm);

	explicit AsylumMcpBridge(AsylumEngine *vm);
	~AsylumMcpBridge() override;

	// From AsylumEngine::handleEvents(), which every screen goes through.
	void pumpFromEvents();

	// Every line of speech or narration the game shows.
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
	// Something an agent can name: an object in the scene with a real name.
	struct Target {
		Common::String name;
		Common::String label;  // the name the game's own data carries
		int x, y;
	};

	static const uint32 kFrameMs = 40;
	static const uint32 kSkipMs = 1500;
	// Frames the cursor is left on a target before the click is sent.
	static const uint32 kPointFrames = 3;

	bool engineReady() const;
	bool playerHasControl() const;
	int roomNumber() const;
	void heroPosition(int &x, int &y) const;

	void collectTargets(Common::Array<Target> &out) const;
	bool resolveTarget(const Common::String &name, Target &out,
	                   Common::String &errorOut) const;
	void pointAndClick(int x, int y, bool rightButton);

	AsylumEngine *_vm;

	bool _inPump;
	bool _skipStream;
	uint32 _lastFrameMs;

	bool _pendingClick;
	bool _pendingRight;
	int _pendingX, _pendingY;
	uint32 _pendingFrame;

	int _ssePreRoom;
	int _ssePreX, _ssePreY;
	Common::Array<Common::String> _ssePreTargets;
	int _sseTrackRoom, _sseTrackX, _sseTrackY;
};

} // End of namespace Asylum

#endif
