#include "stdafx.hpp"
#include <filesystem>
#include <print>
#include <sqlite3.h>

#include "exe_path_utils.hpp"

int main([[maybe_unused]] int argc, [[maybe_unused]] char **argv) {
    sqlite3 *db = nullptr;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        return 1;
    }

    sqlite3_enable_load_extension(db, 1);

    const std::string vec_path = exe_path_utils::get_vec_extension_path();

    char *errmsg = nullptr;
    if (sqlite3_load_extension(db, vec_path.c_str(), nullptr, &errmsg) !=
        SQLITE_OK) {
        std::print("Failed to load extension from '{}': {}\n", vec_path,
                   errmsg ? errmsg : "unknown error");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return 1;
    }

    sqlite3_enable_load_extension(db, 0);
    sqlite3_close(db);
    return 0;
}
