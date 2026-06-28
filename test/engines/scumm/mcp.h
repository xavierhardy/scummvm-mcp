#include <cxxtest/TestSuite.h>

#include "scumm/mcp.h"

#include "backends/networking/mcp/mcp_server.h"

// mcpStripNamePadding has external linkage (defined in mcp_actionname.cpp,
// pulled from libscumm.a) but is not declared in any header. Forward-declare it
// here so the pure name-padding helper can be unit-tested directly.
namespace Scumm {
Common::String mcpStripNamePadding(const Common::String &s);
}

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

	// --- normalizeActionName: the full alias table (every synonym folds) ---
	void test_normalize_all_aliases() {
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("goto"),     "walk_to");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("what is"),  "look_at");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("examine"),  "look_at");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("take"),     "pick_up");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("get"),      "pick_up");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("interact"), "use");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("use_item"), "use");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("use item"), "use");
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
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("look_at"), "look_at");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("pick_up"), "pick_up");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("talk_to"), "talk_to");
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

	// --- normalizeActionName: trailing '@' name padding is stripped ---
	// SCUMM pads names with trailing '@'; a client echoing one back must still
	// match (mcpStripNamePadding runs before the alias lookup).
	void test_normalize_strips_at_padding() {
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("walk@@@@"), "walk_to");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("open@@"),   "open");
	}

	// --- normalizeActionName: V8 "/<room>.<id>/<name>" metadata is stripped ---
	// Curse of Monkey Island object names carry a leading "/room.id/" prefix.
	void test_normalize_strips_v8_prefix() {
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("/2.130/rope"), "rope");
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("/5.7/Talk"),   "talk_to");
	}

	// --- normalizeActionName: UTF-8 Latin-1 uppercase folds to lowercase ---
	// The German "Öffne" (open) verb begins with U+00D6; Common::String's ASCII
	// toLowercase would have left it uppercase and failed the "öffne" match.
	void test_normalize_utf8_lowercase() {
		// "\xC3\x96ffne" == "Öffne" -> "\xC3\xB6ffne" == "öffne"
		TS_ASSERT_EQUALS(Scumm::ScummMcpBridge::normalizeActionName("\xC3\x96""ffne"),
		                 "\xC3\xB6""ffne");
	}
};

// Pure, engine-independent MCP string helpers in the Networking namespace
// (compiled into mcp_server.o, which the test links) plus the Scumm name-padding
// helper. None of these touch a running engine, so they unit-test cleanly.
class McpStringHelpersTestSuite : public CxxTest::TestSuite {
public:
	// --- mcpNormalizeSpaces: collapse runs and trim ---
	void test_normalize_spaces_collapse_and_trim() {
		TS_ASSERT_EQUALS(Networking::mcpNormalizeSpaces("  hello   world  "), "hello world");
		TS_ASSERT_EQUALS(Networking::mcpNormalizeSpaces("abc"),               "abc");
		TS_ASSERT_EQUALS(Networking::mcpNormalizeSpaces(""),                  "");
		TS_ASSERT_EQUALS(Networking::mcpNormalizeSpaces("     "),             "");
	}

	// --- mcpNormalizeSpaces: NBSP (UTF-8 0xC2 0xA0) becomes a plain space ---
	void test_normalize_spaces_nbsp() {
		// "a\xC2\xA0\xC2\xA0b" -> the two NBSPs collapse to one plain space.
		TS_ASSERT_EQUALS(Networking::mcpNormalizeSpaces("a\xC2\xA0\xC2\xA0""b"), "a b");
		// Leading/trailing NBSP is trimmed like a plain space.
		TS_ASSERT_EQUALS(Networking::mcpNormalizeSpaces("\xC2\xA0hi\xC2\xA0"), "hi");
	}

	// --- mcpSanitizeString: valid UTF-8 passes through unchanged ---
	void test_sanitize_valid_utf8_passes() {
		TS_ASSERT_EQUALS(Networking::mcpSanitizeString("hello"),       "hello");
		TS_ASSERT_EQUALS(Networking::mcpSanitizeString("\xC3\x96"),    "\xC3\x96");      // Ö (2-byte)
		TS_ASSERT_EQUALS(Networking::mcpSanitizeString("\xE2\x82\xAC"), "\xE2\x82\xAC"); // € (3-byte)
	}

	// --- mcpSanitizeString: invalid bytes become U+FFFD (EF BF BD) ---
	void test_sanitize_invalid_utf8_replaced() {
		// A 2-byte lead with no continuation byte.
		TS_ASSERT_EQUALS(Networking::mcpSanitizeString("\xC3"),  "\xEF\xBF\xBD");
		// A lone continuation byte.
		TS_ASSERT_EQUALS(Networking::mcpSanitizeString("\x80"),  "\xEF\xBF\xBD");
		// Valid ASCII around a bad byte: only the bad byte is replaced.
		TS_ASSERT_EQUALS(Networking::mcpSanitizeString("a\xFF""b"), "a\xEF\xBF\xBD""b");
	}

	// --- mcpLowerTrimmed: ASCII lowercase + trim ---
	void test_lower_trimmed() {
		TS_ASSERT_EQUALS(Networking::mcpLowerTrimmed("  HELLO  "), "hello");
		TS_ASSERT_EQUALS(Networking::mcpLowerTrimmed("Walk_To"),   "walk_to");
		TS_ASSERT_EQUALS(Networking::mcpLowerTrimmed(""),          "");
	}

	// --- mcpStripNamePadding: trailing '@' and spaces are removed ---
	void test_strip_name_padding() {
		TS_ASSERT_EQUALS(Scumm::mcpStripNamePadding("roter Hering@@@@@"), "roter Hering");
		TS_ASSERT_EQUALS(Scumm::mcpStripNamePadding("name "),            "name");
		TS_ASSERT_EQUALS(Scumm::mcpStripNamePadding("name@ @"),          "name");
		TS_ASSERT_EQUALS(Scumm::mcpStripNamePadding("@@@@"),             "");
		TS_ASSERT_EQUALS(Scumm::mcpStripNamePadding("clean"),            "clean");
	}

	// --- mcpStripNamePadding: only trailing padding is stripped ---
	void test_strip_name_padding_leading_kept() {
		// A '@' that is not at the end stays put.
		TS_ASSERT_EQUALS(Scumm::mcpStripNamePadding("a@b"),  "a@b");
		TS_ASSERT_EQUALS(Scumm::mcpStripNamePadding("a@b@"), "a@b");
	}
};
