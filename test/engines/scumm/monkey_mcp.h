#include <cxxtest/TestSuite.h>

#include "scumm/monkey_mcp.h"

class MonkeyMcpBridgeTestSuite : public CxxTest::TestSuite {
public:
	// --- normalizeActionName: alias mappings ---
	void test_normalize_aliases() {
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("walk"),   "walk_to");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("look"),   "look_at");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("pick"),   "pick_up");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("pickup"), "pick_up");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("talk"),   "talk_to");
	}

	// --- normalizeActionName: non-alias verbs pass through unchanged ---
	void test_normalize_passthrough() {
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("open"),    "open");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("use"),     "use");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("push"),    "push");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("pull"),    "pull");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("close"),   "close");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("give"),    "give");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("walk_to"), "walk_to");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("pick_up"), "pick_up");
	}

	// --- normalizeActionName: case folding and whitespace trimming ---
	void test_normalize_case_and_trim() {
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("  Walk To  "), "walk_to");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("LOOK-AT"),     "look_at");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("Pick Up"),     "pick_up");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("OPEN"),        "open");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("TALK TO"),     "talk_to");
	}

	// --- normalizeActionName: empty and whitespace-only inputs ---
	void test_normalize_empty() {
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName(""),    "");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("   "), "");
	}
};
