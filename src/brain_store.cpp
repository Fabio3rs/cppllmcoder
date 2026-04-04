#include "brain_store.hpp"

#include "db_schema_sql.hpp"
#include "exe_path_utils.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {
std::optional<std::string_view> null_if_empty(std::string_view v) {
    if (v.empty()) {
        return std::nullopt;
    }
    return v;
}

std::string decision_to_string(ToolDecisionKind kind) {
    switch (kind) {
    case ToolDecisionKind::Allow:
        return "allow";
    case ToolDecisionKind::Deny:
        return "deny";
    case ToolDecisionKind::ModifyArgs:
        return "modify_args";
    case ToolDecisionKind::ChooseAlternative:
        return "choose_alternative";
    case ToolDecisionKind::AbortConversation:
        return "abort";
    }
    return "allow";
}

MessageRole role_from_string(std::string_view s) {
    if (s == "system") {
        return MessageRole::System;
    }
    if (s == "assistant") {
        return MessageRole::Assistant;
    }
    if (s == "tool") {
        return MessageRole::Tool;
    }
    return MessageRole::User;
}

std::string column_text(sqlite3_stmt *stmt, int idx) {
    const unsigned char *raw = sqlite3_column_text(stmt, idx);
    return raw ? reinterpret_cast<const char *>(raw) : std::string{};
}
} // namespace

BrainStore::BrainStore(SqliteDb db) : db_(std::move(db)) {}

BrainStore BrainStore::open(const std::string &path, bool enable_vector) {
    namespace fs = std::filesystem;
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }

    SqliteDb db(path);

    const bool load_vec = enable_vector;

    if (load_vec) {
        // Load vec0 extension so vector VTs in schema succeed
        auto maybe_vec = exe_path_utils::get_vec_extension_path();
        fs::path vec_path = maybe_vec;
        if (!fs::exists(vec_path)) {
            vec_path = exe_path_utils::get_executable_dir() / ".." / "vec0.so";
        }
        if (fs::exists(vec_path)) {
            sqlite3_enable_load_extension(db.get(), 1);
            sqlite3_errmsg_ptr errmsg;
            const int rc =
                sqlite3_load_extension(db.get(), vec_path.string().c_str(),
                                       nullptr, std::out_ptr(errmsg));
            if (rc != SQLITE_OK) {
                std::string msg = "sqlite3_load_extension: ";
                msg += (errmsg ? errmsg.get() : "unknown error");
                throw std::runtime_error(msg);
            }
            sqlite3_enable_load_extension(db.get(), 0);
        }
        db.exec(db::schema::kFullSchema);
    } else {
        std::string core_sql;
        core_sql.reserve(db::schema::kCoreTables.size() +
                         db::schema::kIndexes.size() +
                         db::schema::kTriggers.size() + 10);
        core_sql.append(db::schema::kCoreTables);
        core_sql.append(db::schema::kIndexes);
        core_sql.append(db::schema::kTriggers);
        db.exec(core_sql);
    }

    return BrainStore(std::move(db));
}

std::string BrainStore::ensureSession(const SessionInfo &session) {
    if (!stmt_insert_session_.has_value()) {
        stmt_insert_session_.emplace(db_.prepare(
            R"sql(
            INSERT OR IGNORE INTO sessions
            (id, model, model_version, endpoint, temperature, top_p, top_k, max_tokens, seed, params_json)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )sql",
            "insert sessions"));
    }

    auto &stmt = *stmt_insert_session_;
    stmt.clear();
    stmt.reset();
    stmt.bind(1, session.id)
        .bind(2, session.model)
        .bind(3, session.model_version)
        .bind(4, session.endpoint)
        .bind(5, session.temperature)
        .bind(6, session.top_p)
        .bind(7, session.top_k)
        .bind(8, session.max_tokens)
        .bind(9, session.seed)
        .bind(10, session.params_json);
    stmt.step();
    stmt.reset();
    stmt.clear();
    return session.id;
}

void BrainStore::ensureTool(const ToolMetadata &meta) {
    if (!stmt_insert_tool_.has_value()) {
        stmt_insert_tool_.emplace(db_.prepare(
            R"sql(
            INSERT OR IGNORE INTO tools
            (name, description, args_schema, is_sensitive)
            VALUES (?, ?, ?, ?);
        )sql",
            "insert tools"));
    }
    auto &stmt = *stmt_insert_tool_;
    stmt.clear();
    stmt.reset();

    // Build a simple JSON schema from ToolMetadata::arguments
    nlohmann::json args = nlohmann::json::array();
    for (const auto &arg : meta.arguments) {
        args.push_back({{"name", arg.name},
                        {"type", arg.type},
                        {"required", arg.required},
                        {"description", arg.description}});
    }
    const std::string args_schema = args.dump();

    stmt.bind(1, meta.name)
        .bind(2, meta.description)
        .bind(3, args_schema)
        .bind(4, meta.is_sensitive ? 1 : 0);
    stmt.step();
    stmt.reset();
    stmt.clear();
}

void BrainStore::insertMessage(Message &msg) {
    if (msg.id > 0) {
        // If the row already exists, perform an update to avoid duplicates.
        if (!stmt_check_message_.has_value()) {
            stmt_check_message_.emplace(
                db_.prepare("SELECT 1 FROM messages WHERE id = ? LIMIT 1;",
                            "check message exists"));
        }
        auto &check = *stmt_check_message_;
        check.clear();
        check.reset();
        check.bind(1, msg.id);
        const bool exists = (check.step() == SQLITE_ROW);
        check.reset();
        check.clear();

        if (exists) {
            if (!stmt_update_message_.has_value()) {
                stmt_update_message_.emplace(db_.prepare(
                    R"sql(
                    UPDATE messages
                    SET task_id = ?,
                        session_id = ?,
                        role = ?,
                        content = ?,
                        token_count = ?,
                        duration_ms = ?,
                        updated_at = ?
                    WHERE id = ?;
                )sql",
                    "update messages"));
            }
            auto &upd = *stmt_update_message_;
            upd.clear();
            upd.reset();
            upd.bind(1, null_if_empty("")) // placeholder task_id
                .bind(2, msg.session_id)
                .bind(3, msg.role_to_string())
                .bind(4, msg.content)
                .bind(5, static_cast<int>(msg.token_count))
                .bind(6, static_cast<int64_t>(msg.duration.count()))
                .bind(7, msg.updated_at)
                .bind(8, msg.id);
            upd.step();
            upd.reset();
            upd.clear();
            return;
        }
    }

    insertMessage(static_cast<const Message &>(msg));
    // Update msg.id with last inserted ID
    msg.id = db_.lastInsertRowId();
}

void BrainStore::insertMessage(const Message &msg) {
    if (!stmt_insert_message_.has_value()) {
        stmt_insert_message_.emplace(db_.prepare(
            R"sql(
            INSERT INTO messages
            (task_id, session_id, role, content, token_count, duration_ms, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?);
        )sql",
            "insert messages"));
    }
    auto &stmt = *stmt_insert_message_;
    stmt.clear();
    stmt.reset();

    stmt.bind(1, null_if_empty("")) // placeholder task_id
        .bind(2, msg.session_id)
        .bind(3, msg.role_to_string())
        .bind(4, msg.content)
        .bind(5, static_cast<int>(msg.token_count))
        .bind(6, static_cast<int64_t>(msg.duration.count()))
        .bind(7, msg.created_at)
        .bind(8, msg.updated_at);

    stmt.step();
    stmt.reset();
    stmt.clear();
}

SessionInfo BrainStore::loadSession(std::string_view session_id) const {
    auto stmt = db_.prepare(
        R"sql(
        SELECT id, model, model_version, endpoint, temperature, top_p, top_k,
               max_tokens, seed, params_json
        FROM sessions
        WHERE id = ? LIMIT 1;
    )sql",
        "load session");
    stmt.bind(1, session_id);
    if (stmt.step() != SQLITE_ROW) {
        throw std::runtime_error("session not found");
    }
    SessionInfo info;
    info.id = column_text(stmt.get(), 0);
    info.model = column_text(stmt.get(), 1);
    info.model_version = column_text(stmt.get(), 2);
    info.endpoint = column_text(stmt.get(), 3);
    info.temperature = sqlite3_column_double(stmt.get(), 4);
    info.top_p = sqlite3_column_double(stmt.get(), 5);
    info.top_k = sqlite3_column_int(stmt.get(), 6);
    info.max_tokens = sqlite3_column_int(stmt.get(), 7);
    info.seed = sqlite3_column_int(stmt.get(), 8);
    info.params_json = column_text(stmt.get(), 9);
    return info;
}

std::vector<Message>
BrainStore::loadMessages(std::string_view session_id) const {
    auto stmt = db_.prepare(
        R"sql(
        SELECT id, role, content, token_count, duration_ms,
               created_at, updated_at
        FROM messages
        WHERE session_id = ?
        ORDER BY id ASC;
    )sql",
        "load messages");
    stmt.bind(1, session_id);

    std::vector<Message> out;
    while (stmt.step() == SQLITE_ROW) {
        Message m;
        m.id = sqlite3_column_int64(stmt.get(), 0);
        m.role = role_from_string(column_text(stmt.get(), 1));
        m.content = column_text(stmt.get(), 2);
        m.token_count =
            static_cast<unsigned int>(sqlite3_column_int(stmt.get(), 3));
        m.duration =
            std::chrono::milliseconds{sqlite3_column_int64(stmt.get(), 4)};
        m.created_at = column_text(stmt.get(), 5);
        m.updated_at = column_text(stmt.get(), 6);
        m.session_id = std::string(session_id);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<SessionSummary> BrainStore::listSessions() const {
    auto stmt = db_.prepare(
        R"sql(
        SELECT s.id, s.model, s.created_at, s.updated_at,
               COUNT(m.id) AS message_count
        FROM sessions s
        LEFT JOIN messages m ON m.session_id = s.id
        GROUP BY s.id
        ORDER BY s.updated_at DESC;
    )sql",
        "list sessions");
    std::vector<SessionSummary> out;
    while (stmt.step() == SQLITE_ROW) {
        SessionSummary ss;
        ss.id = column_text(stmt.get(), 0);
        ss.model = column_text(stmt.get(), 1);
        ss.created_at = column_text(stmt.get(), 2);
        ss.updated_at = column_text(stmt.get(), 3);
        ss.message_count = sqlite3_column_int(stmt.get(), 4);
        out.push_back(std::move(ss));
    }
    return out;
}

void BrainStore::insertToolInvocation(const ToolInvocationContext &ctx,
                                      const ToolDecision &decision,
                                      std::chrono::milliseconds duration,
                                      std::chrono::milliseconds consent_latency,
                                      bool success,
                                      std::string_view result_summary) {
    if (!stmt_insert_tool_invocation_.has_value()) {
        stmt_insert_tool_invocation_.emplace(db_.prepare(
            R"sql(
            INSERT INTO tool_invocations
            (task_id, session_id, tool_name, json_args, decision, decision_reason,
             consent_latency_ms, status, result_summary, stderr_output, token_cost, duration_ms)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )sql",
            "insert tool_invocations"));
    }
    // Ensure tool row exists to satisfy FK.
    ensureTool(ctx.metadata);

    auto &stmt = *stmt_insert_tool_invocation_;
    stmt.clear();
    stmt.reset();

    const std::string decision_str = decision_to_string(decision.action);
    const std::string status = success ? "succeeded" : "failed";

    stmt.bind(1, null_if_empty("")) // task_id not wired yet
        .bind(2, ctx.session.id)
        .bind(3, ctx.metadata.name)
        .bind(4, ctx.json_args)
        .bind(5, decision_str)
        .bind(6, null_if_empty(decision.reason))
        .bind(7, static_cast<int64_t>(consent_latency.count()))
        .bind(8, status)
        .bind(9, result_summary)
        .bind(10, null_if_empty("")) // stderr_output not captured here
        .bind(11, ctx.estimated_token_cost)
        .bind(12, static_cast<int64_t>(duration.count()));

    stmt.step();
    stmt.reset();
    stmt.clear();
}

void BrainStore::insertExecutionLog(std::string_view task_id,
                                    std::string_view session_id,
                                    std::string_view lua_script,
                                    std::string_view stdout_out,
                                    std::string_view stderr_hints,
                                    int tokens_used,
                                    std::chrono::milliseconds duration) {
    if (!stmt_insert_execution_log_.has_value()) {
        stmt_insert_execution_log_.emplace(db_.prepare(
            R"sql(
            INSERT INTO execution_logs
            (task_id, session_id, lua_script, stdout_output, stderr_hints, tokens_used, duration_ms)
            VALUES (?, ?, ?, ?, ?, ?, ?);
        )sql",
            "insert execution_logs"));
    }
    auto &stmt = *stmt_insert_execution_log_;
    stmt.clear();
    stmt.reset();

    stmt.bind(1, null_if_empty(task_id))
        .bind(2, session_id)
        .bind(3, lua_script)
        .bind(4, null_if_empty(stdout_out))
        .bind(5, null_if_empty(stderr_hints))
        .bind(6, tokens_used)
        .bind(7, static_cast<int64_t>(duration.count()));

    stmt.step();
    stmt.reset();
    stmt.clear();
}

void BrainStore::insertPromptLog(
    std::string_view task_id, std::string_view session_id,
    std::string_view prompt_type, std::string_view model,
    std::string_view model_version, std::string_view prompt_text,
    std::string_view completion_text, int token_estimate, int token_used,
    std::chrono::milliseconds duration) {
    if (!stmt_insert_prompt_log_.has_value()) {
        stmt_insert_prompt_log_.emplace(db_.prepare(
            R"sql(
            INSERT INTO prompt_logs
            (task_id, session_id, prompt_type, model, model_version, prompt_text, completion_text, token_estimate, token_used, duration_ms)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
        )sql",
            "insert prompt_logs"));
    }
    auto &stmt = *stmt_insert_prompt_log_;
    stmt.clear();
    stmt.reset();

    stmt.bind(1, null_if_empty(task_id))
        .bind(2, session_id)
        .bind(3, prompt_type)
        .bind(4, model)
        .bind(5, model_version)
        .bind(6, prompt_text)
        .bind(7, null_if_empty(completion_text))
        .bind(8, token_estimate)
        .bind(9, token_used)
        .bind(10, static_cast<int64_t>(duration.count()));

    stmt.step();
    stmt.reset();
    stmt.clear();
}
