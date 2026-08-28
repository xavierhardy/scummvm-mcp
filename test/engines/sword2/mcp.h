#include <cxxtest/TestSuite.h>

#include "sword2/mcp_names.h"

#include "engines/mcp_bridge.h"

// The sword2 naming helpers (mcp_names.cpp) are deliberately free of any
// Sword2Engine dependency — like engines/mcp_bridge_text.cpp — so they link
// into the cxxtest runner without the engine runtime. Unlike the first game,
// this one labels its own objects, so there is no authored table to guard:
// what has to hold is the cursor classification, the label folding and the
// duplicate suffixing.

class Sword2McpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- What the cursor says the thing is --------------------------------
	void test_pointer_kind_floor() {
		TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(17)), "floor");
		TS_ASSERT(Sword2::sword2PointerIsFloor(17));
		TS_ASSERT(!Sword2::sword2PointerIsExit(17));
	}

	void test_pointer_kind_exits() {
		for (int32 res = 788; res <= 795; ++res) {
			TS_ASSERT(Sword2::sword2PointerIsExit(res));
			TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(res)), "exit");
		}
		TS_ASSERT(Sword2::sword2PointerIsExit(796));
		TS_ASSERT(Sword2::sword2PointerIsExit(797));
		TS_ASSERT(!Sword2::sword2PointerIsExit(787));
		TS_ASSERT(!Sword2::sword2PointerIsExit(798));
	}

	void test_pointer_kind_others() {
		TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(787)), "person");
		TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(3099)), "item");
		TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(1440)), "scroll");
		TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(1441)), "scroll");
		TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(3100)), "object");
		TS_ASSERT_EQUALS(Common::String(Sword2::sword2PointerKind(0)), "object");
	}

	// --- Folding an on-screen label into an identifier ---------------------
	void test_clean_name_basic() {
		TS_ASSERT_EQUALS(Sword2::sword2CleanName("Boat hook"), "boat_hook");
		TS_ASSERT_EQUALS(Sword2::sword2CleanName("SIGN"), "sign");
	}

	void test_clean_name_punctuation_and_runs() {
		TS_ASSERT_EQUALS(Sword2::sword2CleanName("George's  hat!"), "georges_hat");
		TS_ASSERT_EQUALS(Sword2::sword2CleanName("half-open door"), "half_open_door");
		TS_ASSERT_EQUALS(Sword2::sword2CleanName("  padded  "), "padded");
	}

	void test_clean_name_empty() {
		TS_ASSERT_EQUALS(Sword2::sword2CleanName(""), "");
		TS_ASSERT_EQUALS(Sword2::sword2CleanName("...!"), "");
	}

	// --- Unlabelled things --------------------------------------------------
	void test_fallback_name() {
		TS_ASSERT_EQUALS(Sword2::sword2FallbackName(1234), "object_1234");
	}

	// --- Duplicate labels ---------------------------------------------------
	void test_disambiguate() {
		TS_ASSERT_EQUALS(Sword2::sword2Disambiguate("door", 0), "door");
		TS_ASSERT_EQUALS(Sword2::sword2Disambiguate("door", 1), "door_2");
		TS_ASSERT_EQUALS(Sword2::sword2Disambiguate("door", 2), "door_3");
	}
};
