#include "brain_store.hpp"

#include "exe_path_utils.hpp"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sstream>

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
