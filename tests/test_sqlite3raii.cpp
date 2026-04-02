#include "sqlite3raii.hpp"
#include <gtest/gtest.h>

TEST(SqliteRaii, BindStepAndQuery) {
    SqliteDb db(":memory:");
    db.exec("CREATE TABLE t(id INTEGER, name TEXT);");

    {
        auto stmt = db.prepare("INSERT INTO t VALUES(?, ?);", "insert t");
        stmt.bind(1, 1).bind(2, "alpha");
        stmt.step();
    }

    auto query = db.prepare("SELECT COUNT(*) FROM t;", "count t");
    int rc = query.step();
    ASSERT_EQ(rc, SQLITE_ROW);
    const int count = sqlite3_column_int(query.get(), 0);
    EXPECT_EQ(count, 1);
}

TEST(SqliteRaii, TransactionRollbackOnDestruct) {
    SqliteDb db(":memory:");
    db.exec("CREATE TABLE t(id INTEGER);");

    {
        SqliteTxn txn(db);
        auto stmt = db.prepare("INSERT INTO t VALUES(?);", "insert t");
        stmt.bind(1, 42);
        stmt.step();
        // No commit -> rollback on destructor
    }

    auto query = db.prepare("SELECT COUNT(*) FROM t;", "count t");
    int rc = query.step();
    ASSERT_EQ(rc, SQLITE_ROW);
    const int count = sqlite3_column_int(query.get(), 0);
    EXPECT_EQ(count, 0);
}
