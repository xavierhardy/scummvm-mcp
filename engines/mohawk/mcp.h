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


#ifndef MOHAWK_MCP_H
#define MOHAWK_MCP_H

#include "engines/mcp_bridge.h"

#include "common/array.h"
#include "common/formats/json.h"
#include "common/str.h"

namespace Mohawk {

class MohawkEngine_CSTime;

// MCP bridge for the Mohawk engine's CSTime game - Where in Time is Carmen
// Sandiego?
//
// A pointer game with no verb bar and no verbs at all: a scene is a picture
// with regions marked on it, and the whole vocabulary is clicking one. What
// each region *does* is the game's business - some are ways out, some start a
// conversation, some pick something up - and the player finds out by clicking.
// So the bridge offers one verb, and says so rather than inventing a set that
// would go nowhere.
//
// Names come from the one place a player ever sees them: the line the game
// writes when the cursor rests on a region. Each hotspot carries the number of
// that line and the case holds the lines, so nothing has to be swept for.
class MohawkMcpBridge : public MCP::McpBridge {
public:
	static MohawkMcpBridge *create(MohawkEngine_CSTime *vm);

	explicit MohawkMcpBridge(MohawkEngine_CSTime *vm);
	~MohawkMcpBridge() override;

	// Called for every line the game puts in a speech bubble.
	void onGameText(const Common::String &text, int charId);

protected:
	Common::JSONValue *toolState(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAct(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolAnswer(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolWalk(const Common::JSONValue &args, Common::String &errorOut) override;
	bool toolSkip(const Common::JSONValue &args, Common::String &errorOut) override;
	Common::JSONValue *toolDebug(const Common::JSONValue &args, Common::String &errorOut) override;

	Common::String stateToolDescription() const override;
	Common::String actToolDescription() const override;
	Common::String walkToolDescription() const override;
	Common::String skipToolDescription() const override;
	Common::String debugToolDescription() const override;
	Common::JSONValue *buildDebugSchema() const override;
	void augmentStateSchema(Common::JSONObject &outputProps) override;
	void augmentChangesSchema(Common::JSONObject &props) override;

	// Conversations here are the game's own window of things to say, and
	// reading that list back is a separate job from this one.
	bool usesDialogQuestions() const override { return false; }

	Common::JSONValue *callTool(const Common::String &name,
	                            const Common::JSONValue &args,
	                            Common::String &errorOut) override;

	void injectKey(const Common::KeyState &ks) override;
	void injectMouseMove(int x, int y) override;
	void injectMouseClick(int x, int y, const Common::String &button, bool isDouble) override;

	Common::String messageActorName(int actorId) const override;
	int currentRoomForMessages() const override;

	void pumpGame() override;

	void snapshotPreAction() override;
	Common::JSONObject buildStateChanges() const override;
	bool isActionDone() const override;
	bool hasPendingQuestion() const override;
	bool streamRoomChanged() const override;
	void pumpStreamTrack() override;

	uint32 minStreamFrames() const override { return 4; }
	uint32 stuckFrames(bool hadActivity) const override { return hadActivity ? 60 : 12; }
	uint32 timeoutFrames() const override { return 400; }
	uint32 absoluteTimeoutFrames() const override { return 1200; }
	uint32 settleFrames() const override { return 10; }
	uint32 wallClockTimeoutMs() const override { return 120000; }
	uint32 streamTimeoutAnchor() const override {
		return _sseLastEventFrame > 0 ? _sseLastEventFrame : _sseStartFrame;
	}

private:
	// A region of the picture the player can click.
	struct Target {
		Common::String name;
		int id;
		int x, y;   // the middle of the region, in screen coordinates
	};

	static const uint32 kPointFrames = 2;
	static const uint32 kSkipFrames = 12;

	bool engineReady() const;
	int sceneId() const;
	bool sceneBusy() const;

	void collectTargets(Common::Array<Target> &out) const;
	bool resolveTarget(const Common::String &name, Target &out,
	                   Common::String &errorOut) const;

	void pointAndClick(int x, int y);
	void pumpPendingClick();
	void moveCursorTo(int x, int y);

	MohawkEngine_CSTime *_vm;

	bool _pendingClick;
	int _pendingX, _pendingY;
	uint32 _pendingFrame;
	bool _skipStream;

	int _sseTrackScene;
	Common::Array<Common::String> _ssePreTargets;

	Common::Array<int> _messageActors;
};

} // End of namespace Mohawk

#endif
