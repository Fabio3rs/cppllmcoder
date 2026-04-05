#include <gtest/gtest.h>

#include "db_tools.hpp"
#include "lua_context.hpp"
#include "sqlite3raii.hpp"
#include "tool_registry.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <string>

using nlohmann::json;

namespace {

// Minimal helper to set up an in-memory DB with the messages table populated.
struct DbFixture {
    SqliteDb db{":memory:"};

    DbFixture() {
        db.exec("CREATE TABLE messages("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "content TEXT"
                ");");
    }

    int64_t insert(std::string_view content) {
        auto stmt = db.prepare("INSERT INTO messages(content) VALUES(?);",
                               "insert message");
        stmt.bind(1, content);
        stmt.step();
        stmt.reset();
        stmt.clear();
        return db.lastInsertRowId();
    }
};

json exec_json(LuaContext &ctx, std::string_view code) {
    auto res = ctx.execute(code);
    EXPECT_TRUE(res.has_value()) << res.error();
    return json::parse(*res);
}

} // namespace

TEST(DbHeadTool, ReadsSliceWithinLimit) {
    DbFixture fx;
    const auto row_id = fx.insert("hello world");

    auto reg = std::make_shared<DefaultToolRegistry>();
    registerBrainDbTools(*reg, fx.db.get(), /*max_read_bytes=*/8);
    LuaContext lua;
    lua.bindTools(*reg);

    auto script = std::format(
        "return tools.db.head('messages', {}, 'content', 0, 3)", row_id);
    const auto j = exec_json(lua, script);

    EXPECT_EQ(j["data"], "hel");
    EXPECT_EQ(j["bytes_read"], 3);
    EXPECT_FALSE(j["eof"].get<bool>());
}

TEST(DbHeadTool, CapsRequestedBytesToMaxRead) {
    DbFixture fx;
    const auto row_id = fx.insert("abcdefghij");

    auto reg = std::make_shared<DefaultToolRegistry>();
    registerBrainDbTools(*reg, fx.db.get(), /*max_read_bytes=*/4);
    LuaContext lua;
    lua.bindTools(*reg);

    auto script = std::format(
        "return tools.db.head('messages', {}, 'content', 0, 16)", row_id);
    const auto j = exec_json(lua, script);

    EXPECT_EQ(j["data"], "abcd");
    EXPECT_EQ(j["bytes_read"], 4);
}

TEST(DbHeadTool, MarksEofAtEndOfContent) {
    DbFixture fx;
    const auto row_id = fx.insert("abcdef");

    auto reg = std::make_shared<DefaultToolRegistry>();
    registerBrainDbTools(*reg, fx.db.get(), /*max_read_bytes=*/8);
    LuaContext lua;
    lua.bindTools(*reg);

    auto script = std::format(
        "return tools.db.head('messages', {}, 'content', 4, 4)", row_id);
    const auto j = exec_json(lua, script);

    EXPECT_EQ(j["data"], "ef");
    EXPECT_EQ(j["bytes_read"], 2);
    EXPECT_TRUE(j["eof"].get<bool>());
}

TEST(DbHeadTool, RejectsUnexpectedTableAndColumn) {
    DbFixture fx;
    const auto row_id = fx.insert("content");

    auto reg = std::make_shared<DefaultToolRegistry>();
    registerBrainDbTools(*reg, fx.db.get(), /*max_read_bytes=*/8);
    LuaContext lua;
    lua.bindTools(*reg);

    {
        auto res = lua.execute(std::format(
            "return tools.db.head('tasks', {}, 'content', 0, 4)", row_id));
        ASSERT_TRUE(res.has_value());
        EXPECT_NE(res->find("only table 'messages'"), std::string::npos);
    }
    {
        auto res = lua.execute(std::format(
            "return tools.db.head('messages', {}, 'role', 0, 4)", row_id));
        ASSERT_TRUE(res.has_value());
        EXPECT_NE(res->find("only column 'content' is allowed"),
                  std::string::npos);
    }
}

TEST(DbHeadTool, PreservesUtf8BoundariesWithinBudget) {
    DbFixture fx;
    const auto row_id = fx.insert("a"
                                  "\xC3\xA9"
                                  "\xF0\x9F\x98\x80"
                                  "b");

    auto reg = std::make_shared<DefaultToolRegistry>();
    registerBrainDbTools(*reg, fx.db.get(), /*max_read_bytes=*/5);
    LuaContext lua;
    lua.bindTools(*reg);

    auto script = std::format(
        "return tools.db.head('messages', {}, 'content', 0, 5)", row_id);
    const auto j = exec_json(lua, script);

    EXPECT_EQ(j["data"], "a"
                         "\xC3\xA9");
    EXPECT_EQ(j["bytes_read"], 3);
    EXPECT_FALSE(j["eof"].get<bool>());
}

TEST(DbHeadTool, RealignsOffsetInsideUtf8Sequence) {
    DbFixture fx;
    const auto row_id = fx.insert("a"
                                  "\xC3\xA9"
                                  "\xF0\x9F\x98\x80"
                                  "b");

    auto reg = std::make_shared<DefaultToolRegistry>();
    registerBrainDbTools(*reg, fx.db.get(), /*max_read_bytes=*/8);
    LuaContext lua;
    lua.bindTools(*reg);

    auto script = std::format(
        "return tools.db.head('messages', {}, 'content', 2, 8)", row_id);
    const auto j = exec_json(lua, script);

    EXPECT_EQ(j["data"], "\xF0\x9F\x98\x80"
                         "b");
    EXPECT_EQ(j["bytes_read"], 5);
    EXPECT_TRUE(j["eof"].get<bool>());
}

TEST(DbHeadTool, ReturnsWholeGraphemeWhenBudgetCannotFitFirstOne) {
    DbFixture fx;
    const auto row_id = fx.insert("e"
                                  "\xCC\x81"
                                  "x");

    auto reg = std::make_shared<DefaultToolRegistry>();
    registerBrainDbTools(*reg, fx.db.get(), /*max_read_bytes=*/2);
    LuaContext lua;
    lua.bindTools(*reg);

    auto script = std::format(
        "return tools.db.head('messages', {}, 'content', 0, 2)", row_id);
    const auto j = exec_json(lua, script);

    EXPECT_EQ(j["data"], "e"
                         "\xCC\x81");
    EXPECT_EQ(j["bytes_read"], 3);
    EXPECT_FALSE(j["eof"].get<bool>());
}
