#pragma once

#include "free_wrap.hpp"
#include <memory>
#include <sqlite3.h>
#include <stdexcept>
#include <string>

struct SQLiteFinalizer {
    void operator()(sqlite3_stmt *ptr) const noexcept { sqlite3_finalize(ptr); }
};

struct SQLiteClose {
    void operator()(sqlite3 *ptr) const noexcept { sqlite3_close(ptr); }
};

using sqlite3_stmt_ptr = std::unique_ptr<sqlite3_stmt, SQLiteFinalizer>;
using sqlite3_db_ptr = std::unique_ptr<sqlite3, SQLiteClose>;
using sqlite3_errmsg_ptr = std::unique_ptr<char, freewrapper>;

inline void exec_or_throw(sqlite3 *db, const char *sql) {
    sqlite3_errmsg_ptr errmsg;
    const int rc =
        sqlite3_exec(db, sql, nullptr, nullptr, std::out_ptr(errmsg));
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg.get() : "unknown sqlite error";
        throw std::runtime_error(msg);
    }
}

inline void exec_or_throw(const sqlite3_db_ptr &db, const char *sql) {
    exec_or_throw(db.get(), sql);
}

inline void check_sqlite_rc(sqlite3 *db, int rc, const char *context) {
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) {
        return;
    }

    const char *errmsg = sqlite3_errmsg(db);
    std::string msg = context;
    msg += ": ";
    msg += (errmsg != nullptr ? errmsg : "unknown sqlite error");
    throw std::runtime_error(msg);
}

inline void check_sqlite_rc(const sqlite3_db_ptr &db, int rc,
                            const char *context) {
    check_sqlite_rc(db.get(), rc, context);
}

inline sqlite3_stmt_ptr prepare_or_throw(sqlite3 *db, const char *sql,
                                         const char *context) {
    sqlite3_stmt_ptr stmt;
    const int rc = sqlite3_prepare_v2(db, sql, -1, std::out_ptr(stmt), nullptr);
    check_sqlite_rc(db, rc, context);
    return stmt;
}

inline sqlite3_stmt_ptr prepare_or_throw(const sqlite3_db_ptr &db,
                                         const char *sql, const char *context) {
    return prepare_or_throw(db.get(), sql, context);
}
