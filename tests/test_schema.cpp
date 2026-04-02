#include "db_schema_sql.hpp"
#include <gtest/gtest.h>
#include <string>

TEST(DbSchema, ContainsSessionsTableAndFkColumns) {
    const std::string schema{db::schema::kFullSchema};

    EXPECT_NE(schema.find("CREATE TABLE IF NOT EXISTS sessions"),
              std::string::npos);
    EXPECT_NE(schema.find("session_id TEXT NOT NULL"), std::string::npos);
    EXPECT_NE(schema.find("FOREIGN KEY (session_id) REFERENCES sessions(id)"),
              std::string::npos);
}

TEST(DbSchema, ContainsSessionIndexesAndTriggers) {
    const std::string schema{db::schema::kFullSchema};

    EXPECT_NE(schema.find("idx_messages_session"), std::string::npos);
    EXPECT_NE(schema.find("idx_tool_invocations_session"), std::string::npos);
    EXPECT_NE(schema.find("idx_execution_logs_session"), std::string::npos);
    EXPECT_NE(schema.find("idx_prompt_logs_session"), std::string::npos);
    EXPECT_NE(schema.find("trg_sessions_updated"), std::string::npos);
}
