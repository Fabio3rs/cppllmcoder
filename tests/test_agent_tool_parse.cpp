#include "agents/agent_action.hpp"
#include <gtest/gtest.h>

TEST(AgentActionParse, ExtractsCodeBetweenTags) {
    const std::string raw = R"(Lorem ipsum...

<code>
    local result = fs.ls(".")
    return result[1]
</code>
)";

    AgentAction action = AgentAction::parse(raw);

    EXPECT_TRUE(action.has_code);
    EXPECT_EQ(action.thought, "Lorem ipsum...\n\n");
    EXPECT_EQ(action.lua_code, "local result = fs.ls(\".\")\n"
                               "    return result[1]");
}

TEST(AgentActionParse, HandlesPlainTextWhenNoTags) {
    const std::string raw = "Just a reflection without code blocks.";

    AgentAction action = AgentAction::parse(raw);

    EXPECT_FALSE(action.has_code);
    EXPECT_EQ(action.thought, raw);
    EXPECT_TRUE(action.lua_code.empty());
}

TEST(AgentActionParse, StripsMarkdownLuaFenceInsideCodeTag) {
    const std::string raw = R"(<code>```lua
print("hello")
```
</code>)";

    AgentAction action = AgentAction::parse(raw);

    EXPECT_TRUE(action.has_code);
    EXPECT_EQ(action.thought, "");
    EXPECT_EQ(action.lua_code, "print(\"hello\")");
}
