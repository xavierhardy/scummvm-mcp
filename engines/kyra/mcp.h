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

#ifndef KYRA_MCP_H
#define KYRA_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Kyra {

class KyraEngine_v1;

// MCP bridge for the Kyrandia engine (Kyrandia 1, 2 and 3).
//
// Kyrandia is a pointer game with no verb bar: the left button does whatever
// the thing under it is for and the right button looks at it, and an item is
// used by picking it up into the hand and clicking it on something. So the
// bridge's vocabulary is small and honest - use, look_at, take, and a second
// target for "use the ring on the altar" - and act() is a click.
//
// What it does *not* have is any label on what is on screen. The engine keeps
// no script names and paints no hover text, so the one list of names the game
// itself owns is its item-name table: the strings it writes into its own
// sentence line ("a golden ring"). Everything the bridge can name comes from
// there, plus the four compass exits, which have no names at all and are
// given ones.
//
// Two scene models sit behind one bridge, because two generations of the
// engine sit behind one game series. The first game keeps the scene's items
// in a per-room table (Room::itemsTable, with x/y beside it); the later two
// keep one flat item list and stamp each entry with the scene it is lying in.
// Both are read here rather than in two subclasses: the difference is a dozen
// lines, and a reader comparing them wants them side by side.
class KyraMcpBridge : public MCP::McpBridge {
public:
	// Factory mirroring the other bridges' two-phase construction.
	static KyraMcpBridge *create(KyraEngine_v1 *vm);

	explicit KyraMcpBridge(KyraEngine_v1 *vm);
	~KyraMcpBridge() override;

	// Once per pass of the game's own loop - each of the three games has one
	// of its own, and they all call this.
	void pump() override;
	// From the engine's delay(), which is where every one of its blocking
	// waits ends up: a cutscene, a spoken line, a fade. The loop is not
	// reached from there, so this is the only thing that answers a call made
	// during one. Does not advance the frame counter.
	void pumpFromStall();

	// Every line the game prints into its text area.
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

	// Nothing here is typed, and no question is ever put as a numbered list:
	// conversations play themselves out.
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

	// A frame is one pass of the game loop, which runs at about 60 a second.
	uint32 minStreamFrames() const override { return 6; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 90 : 20; }
	uint32 timeoutFrames() const override { return 600; }
	uint32 absoluteTimeoutFrames() const override { return 2400; }
	uint32 settleFrames() const override { return 20; }
	// The loop is not reached at all while a cutscene plays, so a skip sent
	// into one has no frames to count; see McpBridge::wallClockCloseMs().
	uint32 wallClockTimeoutMs() const override { return 180000; }
	uint32 wallClockCloseMs() const override { return _skipStream ? kSkipMs : 0; }
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// Something an agent can name: an item lying in the scene, or one of the
	// four ways out of it.
	struct Target {
		Common::String name;
		Common::String label;   // what the game itself calls it, where it does
		int x, y;               // where to click, in game coordinates
		bool isExit;
	};

	static const uint32 kSkipMs = 1500;
	// Frames the cursor is left on a target before the click is sent, so the
	// game's own hit-testing has seen the pointer arrive.
	static const uint32 kPointFrames = 3;

	bool engineReady() const;
	bool isFirstGame() const;   // Kyrandia 1, which keeps a different scene table
	bool playerHasControl() const;

	int roomNumber() const;
	void heroPosition(int &x, int &y) const;

	// The name the game prints for an item, or an empty string when it has
	// none. This is the only naming the engine offers.
	Common::String itemLabel(int itemId) const;

	// Everything in the scene an agent could click, named and disambiguated.
	void collectTargets(Common::Array<Target> &out) const;
	// What the player is carrying.
	void collectInventory(Common::Array<Target> &out) const;
	// Resolve a name (or an index into the last snapshot) to something to
	// click.
	bool resolveTarget(const Common::String &name, Target &out,
	                   Common::String &errorOut) const;

	// Put the pointer somewhere and queue the click that follows once the game
	// has noticed it arrive. A real mouse always hovers before it clicks, and
	// this engine's scripts key off the hover.
	void pointAndClick(int x, int y, bool rightButton);

	KyraEngine_v1 *_vm;

	bool _inStallPump;
	bool _skipStream;

	// The click waiting on the pointer having been noticed.
	bool _pendingClick;
	bool _pendingRight;
	int _pendingX, _pendingY;
	uint32 _pendingFrame;

	// Pre-action snapshot, for the changes an action reports.
	int _ssePreRoom;
	int _ssePreX, _ssePreY;
	Common::Array<Common::String> _ssePreTargets;
	Common::Array<Common::String> _ssePreInventory;
	// Progress tracking for the stream deadline.
	int _sseTrackRoom, _sseTrackX, _sseTrackY;
};

} // End of namespace Kyra

#endif
