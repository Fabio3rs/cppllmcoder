#include "lua_context.hpp"
#include "tool_registry.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

class EchoTool : public ITool {
  public:
    ToolMetadata describe() const override {
        return ToolMetadata{
            .name = "echo",
            .description = "returns msg",
            .arguments = {},
            .usage_example = "",
            .returns = "string",
            .danger_tags = {},
            .is_sensitive = false,
            .always_show_in_prompt = false,
        };
    }

    std::expected<sol::object, std::string>
    invoke(const sol::object &lua_args) const override {
        if (lua_args.is<sol::table>()) {
            sol::table tbl = lua_args;
            if (tbl["msg"].valid()) {
                return tbl["msg"].get<sol::object>();
            }
        }
        return std::unexpected("missing msg");
    }
};

TEST(LuaContextBinding, CallsRegisteredTool) {
    DefaultToolRegistry reg;
    reg.registerTool(std::make_shared<EchoTool>());

    LuaContext lua;
    lua.bindTools(reg);

    const auto res = lua.execute(R"lua(
        local out = tools.echo({ msg = "hello" })
        return out
    )lua");

    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, "hello");
}

TEST(LuaContextBinding, ToolErrorPropagatesAsString) {
    class FailingTool : public ITool {
      public:
        ToolMetadata describe() const override {
            return ToolMetadata{
                .name = "fail",
                .description = "always fails",
                .arguments = {},
                .usage_example = "",
                .returns = "",
                .danger_tags = {},
                .is_sensitive = false,
                .always_show_in_prompt = false,
            };
        }
        std::expected<sol::object, std::string>
        invoke(const sol::object &) const override {
            return std::unexpected("boom");
        }
    };

    DefaultToolRegistry reg;
    reg.registerTool(std::make_shared<FailingTool>());

    LuaContext lua;
    lua.bindTools(reg);

    const auto res = lua.execute(R"lua(
        return tools.fail({})
    )lua");

    ASSERT_TRUE(res.has_value());
    EXPECT_NE(res->find("boom"), std::string::npos);
}

TEST(LuaContextBinding, InvokerCanDenyCall) {
    DefaultToolRegistry reg;
    reg.registerTool(std::make_shared<EchoTool>());

    LuaContext lua;
    lua.bindTools(reg,
                  [](const ToolMetadata &, const ITool &, const sol::object &) {
                      return std::unexpected("denied");
                  });

    const auto res = lua.execute(R"lua(
        return tools.echo({ msg = "hello" })
    )lua");

    ASSERT_TRUE(res.has_value());
    EXPECT_NE(res->find("denied"), std::string::npos);
}

TEST(LuaContextBinding, ToolThrowIsCaught) {
    class ThrowingTool : public ITool {
      public:
        ToolMetadata describe() const override {
            return {.name = "thrower",
                    .description = "throws std::runtime_error",
                    .arguments = {},
                    .usage_example = "",
                    .returns = "",
                    .danger_tags = {},
                    .is_sensitive = false,
                    .always_show_in_prompt = false};
        }
        std::expected<sol::object, std::string>
        invoke(const sol::object &) const override {
            throw std::runtime_error("kaboom");
        }
    };

    DefaultToolRegistry reg;
    reg.registerTool(std::make_shared<ThrowingTool>());

    LuaContext lua;
    lua.bindTools(reg);

    const auto res = lua.execute(R"lua(
        return tools.thrower({})
    )lua");

    ASSERT_TRUE(res.has_value());
    EXPECT_NE(res->find("exception"), std::string::npos);
}

TEST(LuaContextSandbox, BlocksDofile) {
    LuaContext lua;
    const auto res = lua.execute(R"lua(
        return pcall(function() dofile('test_lua_context.cpp') end)
    )lua");

    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, "false");
}

TEST(LuaContextReturnTypes, SerializesTables) {
    LuaContext lua;
    const auto res = lua.execute(R"lua(
        return { a = 1, b = { 2, 3 } }
    )lua");

    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res, "{\"a\":1.000000,\"b\":[2.000000,3.000000]}");
}
