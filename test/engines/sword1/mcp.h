#include <cxxtest/TestSuite.h>

#include "sword1/mcp_names.h"
#include "sword1/sworddefs.h"

#include "engines/mcp_bridge.h"

// The sword1 naming tables (mcp_names.cpp) are deliberately free of any
// SwordEngine dependency — like engines/mcp_bridge_text.cpp — so they link into
// the cxxtest runner without the engine runtime. This suite covers the pocket
// and compact name lookups, the "object_<section>_<index>" fallback, and the
// round-tripping resolver, plus a guard that every authored name is unique and
// resolves back to its id.

class Sword1McpNamesTestSuite : public CxxTest::TestSuite {
public:
	// --- Inventory item names --------------------------------------------
	void test_pocket_names() {
		TS_ASSERT_EQUALS(Common::String(Sword1::sword1PocketName(1)), "newspaper");
		TS_ASSERT_EQUALS(Common::String(Sword1::sword1PocketName(4)), "hotel_key");
		TS_ASSERT_EQUALS(Common::String(Sword1::sword1PocketName(TOTAL_pockets)), "pipe");
	}

	void test_pocket_name_bounds() {
		TS_ASSERT(Sword1::sword1PocketName(0) == nullptr);
		TS_ASSERT(Sword1::sword1PocketName(-1) == nullptr);
		TS_ASSERT(Sword1::sword1PocketName(TOTAL_pockets + 1) == nullptr);
	}

	void test_pocket_number_roundtrip() {
		TS_ASSERT_EQUALS(Sword1::sword1PocketNumber("newspaper"), 1);
		TS_ASSERT_EQUALS(Sword1::sword1PocketNumber("pipe"), TOTAL_pockets);
		TS_ASSERT_EQUALS(Sword1::sword1PocketNumber("not_an_item"), 0);
	}

	// --- Compact names ----------------------------------------------------
	void test_compact_names() {
		TS_ASSERT_EQUALS(Common::String(Sword1::sword1CompactName(PLAYER)), "george");
		TS_ASSERT_EQUALS(Common::String(Sword1::sword1CompactName(NICO)), "nico");
		TS_ASSERT(Sword1::sword1CompactName(0x00010063) == nullptr);
	}

	// --- Fallback naming --------------------------------------------------
	void test_object_name_fallback() {
		// id = section 1, index 23 -> unknown -> object_1_23.
		uint32 id = 1 * ITM_PER_SEC + 23;
		TS_ASSERT_EQUALS(Sword1::sword1ObjectName(id), "object_1_23");
	}

	void test_object_name_uses_authored() {
		TS_ASSERT_EQUALS(Sword1::sword1ObjectName(PLAYER), "george");
	}

	// --- Resolver ---------------------------------------------------------
	void test_resolve_authored_name() {
		uint32 id = 0;
		TS_ASSERT(Sword1::sword1ResolveName("george", id));
		TS_ASSERT_EQUALS(id, (uint32)PLAYER);
	}

	void test_resolve_fallback_form() {
		uint32 id = 0;
		TS_ASSERT(Sword1::sword1ResolveName("object_1_23", id));
		TS_ASSERT_EQUALS(id, (uint32)(1 * ITM_PER_SEC + 23));
	}

	void test_resolve_decimal_id() {
		uint32 id = 0;
		TS_ASSERT(Sword1::sword1ResolveName("8388608", id));
		TS_ASSERT_EQUALS(id, (uint32)PLAYER);
	}

	void test_resolve_hex_id() {
		uint32 id = 0;
		TS_ASSERT(Sword1::sword1ResolveName("0x800000", id));
		TS_ASSERT_EQUALS(id, (uint32)PLAYER);
	}

	void test_resolve_case_and_space_folding() {
		uint32 id = 0;
		TS_ASSERT(Sword1::sword1ResolveName("  George  ", id));
		TS_ASSERT_EQUALS(id, (uint32)PLAYER);
		id = 0;
		TS_ASSERT(Sword1::sword1ResolveName("GEORGE", id));
		TS_ASSERT_EQUALS(id, (uint32)PLAYER);
	}

	void test_resolve_unknown_fails() {
		uint32 id = 42;
		TS_ASSERT(!Sword1::sword1ResolveName("definitely_not_a_thing", id));
	}

	// Every authored compact name must be unique and round-trip back to its id.
	// This catches the single most likely authoring bug: a duplicated name.
	void test_authored_table_roundtrips() {
		int count = Sword1::sword1CompactNameCount();
		for (int i = 0; i < count; ++i) {
			uint32 id = 0;
			const char *name = nullptr;
			Sword1::sword1CompactNameAt(i, id, name);
			uint32 resolved = 0;
			TS_ASSERT(Sword1::sword1ResolveName(name, resolved));
			TS_ASSERT_EQUALS(resolved, id);
		}
	}
};
