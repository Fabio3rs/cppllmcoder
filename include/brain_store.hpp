#pragma once

#include "agent.hpp"
#include "db_schema_sql.hpp"
#include "sqlite3raii.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

class BrainStore {
  public:
    static BrainStore open(const std::string &path, bool enable_vector = true);

    sqlite3 *raw_db() const { return db_.get(); }

    std::string ensureSession(const SessionInfo &session);
    void ensureTool(const ToolMetadata &meta);
    void insertMessage(Message &msg);
    void insertMessage(const Message &msg);
    void insertToolInvocation(const ToolInvocationContext &ctx,
                              const ToolDecision &decision,
                              std::chrono::milliseconds duration,
                              std::chrono::milliseconds consent_latency,
                              bool success, std::string_view result_summary);
    void insertExecutionLog(std::string_view task_id,
                            std::string_view session_id,
                            std::string_view lua_script,
                            std::string_view stdout_out,
                            std::string_view stderr_hints, int tokens_used,
                            std::chrono::milliseconds duration);
    void insertPromptLog(std::string_view task_id, std::string_view session_id,
                         std::string_view prompt_type, std::string_view model,
                         std::string_view model_version,
                         std::string_view prompt_text,
                         std::string_view completion_text, int token_estimate,
                         int token_used, std::chrono::milliseconds duration);

  private:
    explicit BrainStore(SqliteDb db);

    SqliteDb db_;

    // Prepared statements reused per instance
    mutable std::optional<SqliteStatement> stmt_insert_tool_;
    mutable std::optional<SqliteStatement> stmt_insert_session_;
    mutable std::optional<SqliteStatement> stmt_insert_message_;
    mutable std::optional<SqliteStatement> stmt_update_message_;
    mutable std::optional<SqliteStatement> stmt_check_message_;
    mutable std::optional<SqliteStatement> stmt_insert_tool_invocation_;
    mutable std::optional<SqliteStatement> stmt_insert_execution_log_;
    mutable std::optional<SqliteStatement> stmt_insert_prompt_log_;
};
