#include <gtest/gtest.h>

#include "fs_tools.hpp"
#include "lua_context.hpp"
#include "tool_registry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace {
using nlohmann::json;
namespace fs = std::filesystem;

fs::path make_temp_tree() {
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    auto base = fs::temp_directory_path() /
                fs::path("cppllmcoder-fs-" + std::to_string(nonce));
    fs::create_directories(base / "sub");
    std::ofstream(base / "a.txt") << "abc";
    std::ofstream(base / "sub" / "b.txt") << "def";
    // best-effort symlink; ignore failure on platforms that require elevation
    std::error_code ec;
    fs::create_symlink(base / "a.txt", base / "link_outside", ec);
    return base;
}

json exec_json(LuaContext &ctx, std::string_view code) {
    auto res = ctx.execute(code);
    EXPECT_TRUE(res.has_value()) << res.error();
    return json::parse(*res);
}

TEST(FsTools, LsBasicDepth) {
    const auto root = make_temp_tree();
    auto reg = std::make_shared<DefaultToolRegistry>();
    registerFilesystemTools(*reg, root.string(), 8192);
    LuaContext lua;
    lua.bindTools(*reg);

    const auto j = exec_json(lua, "return tools.fs.ls({dir='.', depth=1})");
    ASSERT_EQ(j.size(), 2u);
    EXPECT_EQ(j[0]["name"], "a.txt");
    EXPECT_EQ(j[0]["kind"], "file");
    EXPECT_EQ(j[1]["name"], "sub");
    EXPECT_EQ(j[1]["kind"], "dir");

    fs::remove_all(root);
}

TEST(FsTools, SizeAndRead) {
    const auto root = make_temp_tree();
    auto reg = std::make_shared<DefaultToolRegistry>();
    registerFilesystemTools(*reg, root.string(), 4); // small limit
    LuaContext lua;
    lua.bindTools(*reg);

    const auto size_j = exec_json(lua, "return tools.fs.size('a.txt')");
    EXPECT_EQ(size_j.get<int>(), 3);

    const auto read_j = exec_json(
        lua, "return tools.fs.read({path='a.txt', offset=1, max_bytes=8})");
    EXPECT_EQ(read_j["data"], "bc");
    EXPECT_EQ(read_j["bytes_read"], 2);
    EXPECT_TRUE(read_j["eof"].get<bool>());

    fs::remove_all(root);
}

TEST(FsTools, BlocksTraversal) {
    const auto root = make_temp_tree();
    auto reg = std::make_shared<DefaultToolRegistry>();
    registerFilesystemTools(*reg, root.string(), 8192);
    LuaContext lua;
    lua.bindTools(*reg);

    auto res = lua.execute("return tools.fs.read('../etc/passwd')");
    ASSERT_TRUE(res.has_value());
    EXPECT_NE(res->find("path escapes root"), std::string::npos);

    fs::remove_all(root);
}

TEST(FsTools, SymlinkListedButNotFollowed) {
    const auto root = make_temp_tree();
    if (!fs::is_symlink(root / "link_outside")) {
        GTEST_SKIP() << "symlink creation not supported on this platform";
    }
    auto reg = std::make_shared<DefaultToolRegistry>();
    registerFilesystemTools(*reg, root.string(), 8192);
    LuaContext lua;
    lua.bindTools(*reg);

    auto res = lua.execute(
        "return tools.fs.ls({dir='.', depth=2, include_symlinks=true})");
    ASSERT_TRUE(res.has_value());
    auto j = json::parse(*res);

    bool saw_symlink = false;
    for (const auto &row : j) {
        if (row.value("kind", "") == "symlink") {
            saw_symlink = true;
        }
    }
    EXPECT_TRUE(saw_symlink);

    fs::remove_all(root);
}

} // namespace
