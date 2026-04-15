#include <cxxtest/TestSuite.h>

#include "scumm/monkey_mcp.h"

class MonkeyMcpBridgeTestSuite : public CxxTest::TestSuite {
public:
	void test_parse_entity_id_object() {
		Scumm::MonkeyMcpBridge::ParsedEntityId parsed;
		TS_ASSERT(Scumm::MonkeyMcpBridge::parseEntityId("obj:42", parsed));
		TS_ASSERT_EQUALS(parsed.kind, Scumm::MonkeyMcpBridge::kEntityObject);
		TS_ASSERT_EQUALS(parsed.value, 42);
	}

	void test_parse_entity_id_actor() {
		Scumm::MonkeyMcpBridge::ParsedEntityId parsed;
		TS_ASSERT(Scumm::MonkeyMcpBridge::parseEntityId("actor:3", parsed));
		TS_ASSERT_EQUALS(parsed.kind, Scumm::MonkeyMcpBridge::kEntityActor);
		TS_ASSERT_EQUALS(parsed.value, 3);
	}

	void test_parse_entity_id_inventory() {
		Scumm::MonkeyMcpBridge::ParsedEntityId parsed;
		TS_ASSERT(Scumm::MonkeyMcpBridge::parseEntityId("inv:7", parsed));
		TS_ASSERT_EQUALS(parsed.kind, Scumm::MonkeyMcpBridge::kEntityInventory);
		TS_ASSERT_EQUALS(parsed.value, 7);
	}

	void test_parse_entity_id_place() {
		Scumm::MonkeyMcpBridge::ParsedEntityId parsed;
		TS_ASSERT(Scumm::MonkeyMcpBridge::parseEntityId("place:box:12", parsed));
		TS_ASSERT_EQUALS(parsed.kind, Scumm::MonkeyMcpBridge::kEntityPlaceBox);
		TS_ASSERT_EQUALS(parsed.value, 12);
	}

	void test_parse_entity_id_choice() {
		Scumm::MonkeyMcpBridge::ParsedEntityId parsed;
		TS_ASSERT(Scumm::MonkeyMcpBridge::parseEntityId("choice:verbslot:9", parsed));
		TS_ASSERT_EQUALS(parsed.kind, Scumm::MonkeyMcpBridge::kEntityChoiceVerbSlot);
		TS_ASSERT_EQUALS(parsed.value, 9);
	}

	void test_parse_entity_id_invalid() {
		Scumm::MonkeyMcpBridge::ParsedEntityId parsed;
		TS_ASSERT(!Scumm::MonkeyMcpBridge::parseEntityId("banana", parsed));
		TS_ASSERT_EQUALS(parsed.kind, Scumm::MonkeyMcpBridge::kEntityInvalid);
		TS_ASSERT(!Scumm::MonkeyMcpBridge::parseEntityId("obj:-1", parsed));
	}

	void test_normalize_action_name_aliases() {
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("walk"), "walk_to");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("look"), "look_at");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("pickup"), "pick_up");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("talk"), "talk_to");
	}

	void test_normalize_action_name_spacing_and_case() {
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("  Walk To  "), "walk_to");
		TS_ASSERT_EQUALS(Scumm::MonkeyMcpBridge::normalizeActionName("LOOK-AT"), "look_at");
	}
};
