#include "engines/scumm/monkey_mcp.h"
#include "common/json.h"
#include "common/str.h"
#include <gtest/gtest.h>

namespace Scumm {

class MockMonkeyMcpBridge : public MonkeyMcpBridge {
public:
    MockMonkeyMcpBridge() : MonkeyMcpBridge(nullptr) {
        // Mock initialization
        _initialized = true;
    }
    
    // Override virtual methods to avoid null pointer issues
    bool isMonkey1() const override { return true; }
    
    // Test wrapContent function
    static Common::JSONValue* testWrapContent() {
        Common::JSONObject testObj;
        testObj.setVal("test", makeString("value"));
        testObj.setVal("number", makeInt(42));
        
        Common::JSONValue *result = new Common::JSONValue(testObj);
        Common::JSONObject structuredCopy(testObj);
        Common::JSONValue *structured = new Common::JSONValue(structuredCopy);
        
        return wrapContent(result, false, structured);
    }
};

TEST(McpTest, WrapContentTest) {
    Common::JSONValue *wrapped = MockMonkeyMcpBridge::testWrapContent();
    ASSERT_TRUE(wrapped != nullptr);
    ASSERT_TRUE(wrapped->isObject());
    
    const Common::JSONObject &obj = wrapped->asObject();
    ASSERT_TRUE(obj.contains("content"));
    ASSERT_TRUE(obj["content"]->isArray());
    
    const Common::JSONArray &content = obj["content"]->asArray();
    ASSERT_EQ(content.size(), 1);
    ASSERT_TRUE(content[0]->isObject());
    
    const Common::JSONObject &textObj = content[0]->asObject();
    ASSERT_TRUE(textObj.contains("type"));
    ASSERT_TRUE(textObj.contains("text"));
    ASSERT_EQ(textObj["type"]->asString(), "text");
    
    // Check structured content
    ASSERT_TRUE(obj.contains("structuredContent"));
    ASSERT_TRUE(obj["structuredContent"]->isObject());
    
    const Common::JSONObject &structured = obj["structuredContent"]->asObject();
    ASSERT_TRUE(structured.contains("test"));
    ASSERT_TRUE(structured.contains("number"));
    ASSERT_EQ(structured["test"]->asString(), "value");
    ASSERT_EQ(structured["number"]->asIntegerNumber(), 42);
    
    delete wrapped;
}

TEST(McpTest, WrapContentErrorTest) {
    Common::JSONObject testObj;
    testObj.setVal("error", makeString("test error"));
    
    Common::JSONValue *result = new Common::JSONValue(testObj);
    Common::JSONValue *wrapped = wrapContent(result, true);
    
    ASSERT_TRUE(wrapped != nullptr);
    ASSERT_TRUE(wrapped->isObject());
    
    const Common::JSONObject &obj = wrapped->asObject();
    ASSERT_TRUE(obj.contains("content"));
    ASSERT_TRUE(obj.contains("isError"));
    ASSERT_TRUE(obj["isError"]->asBool());
    
    delete wrapped;
}

TEST(McpTest, MakePropOneOfTest) {
    Common::JSONValue *prop = makePropOneOf("string", "integer", "Test property");
    ASSERT_TRUE(prop != nullptr);
    ASSERT_TRUE(prop->isObject());
    
    const Common::JSONObject &obj = prop->asObject();
    ASSERT_TRUE(obj.contains("oneOf"));
    ASSERT_TRUE(obj["oneOf"]->isArray());
    
    const Common::JSONArray &oneOf = obj["oneOf"]->asArray();
    ASSERT_EQ(oneOf.size(), 2);
    
    ASSERT_TRUE(oneOf[0]->isObject());
    ASSERT_TRUE(oneOf[0]->asObject()["type"]->asString() == "string");
    
    ASSERT_TRUE(oneOf[1]->isObject());
    ASSERT_TRUE(oneOf[1]->asObject()["type"]->asString() == "integer");
    
    delete prop;
}

} // namespace Scumm

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}