#include <cxxtest/TestSuite.h>

#include "scumm/mcp.h"

class ScummMcpBridgeTestSuite : public CxxTest::TestSuite {
public:
	// --- normalizeActionName: alias mappings ---
	void test_normalize_aliases() {
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("walk"),   "walk_to");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("look"),   "look_at");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("pick"),   "pick_up");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("pickup"), "pick_up");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("talk"),   "talk_to");
	}

	// --- normalizeActionName: non-alias verbs pass through unchanged ---
	void test_normalize_passthrough() {
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("open"),    "open");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("use"),     "use");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("push"),    "push");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("pull"),    "pull");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("close"),   "close");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("give"),    "give");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("walk_to"), "walk_to");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("pick_up"), "pick_up");
	}

	// --- normalizeActionName: case folding and whitespace trimming ---
	void test_normalize_case_and_trim() {
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("  Walk To  "), "walk_to");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("LOOK-AT"),     "look_at");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("Pick Up"),     "pick_up");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("OPEN"),        "open");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("TALK TO"),     "talk_to");
	}

	// --- normalizeActionName: empty and whitespace-only inputs ---
	void test_normalize_empty() {
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName(""),    "");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("   "), "");
	}
};
