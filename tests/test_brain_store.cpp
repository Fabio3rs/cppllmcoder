#include "brain_store.hpp"
#include "sqlite3raii.hpp"
#include <gtest/gtest.h>

static int count_rows(sqlite3 *db, const char *table) {
    std::string sql = std::string("SELECT COUNT(*) FROM ") + table + ";";
    auto stmt = prepare_or_throw(db, sql.c_str(), "count rows");
    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_ROW) {
        throw std::runtime_error("failed to count rows");
    }
    return sqlite3_column_int(stmt.get(), 0);
}

TEST(BrainStore, InsertsSessionMessageAndToolInvocation) {
    BrainStore store = BrainStore::open(":memory:", /*enable_vector=*/false);

    SessionInfo session;
    session.id = "sess-1";
    session.model = "model";
    session.model_version = "v1";
    session.endpoint = "endpoint";
    session.temperature = 0.5;
    session.top_p = 0.9;
    session.top_k = 40;
    session.max_tokens = 1000;
    session.seed = 123;
    session.params_json = "{}";

    store.ensureSession(session);
    EXPECT_EQ(count_rows(store.raw_db(), "sessions"), 1);

    Message msg{.id = 1,
                .role = MessageRole::User,
                .content = "hello",
                .session_id = session.id,
                .created_at = "t",
                .updated_at = "t",
                .duration = std::chrono::milliseconds{0},
                .token_count = 0};
    store.insertMessage(msg);
    EXPECT_EQ(count_rows(store.raw_db(), "messages"), 1);

    ToolMetadata meta;
    meta.name = "echo";
    ToolInvocationContext ctx{meta, "{}", 0, std::chrono::milliseconds{0},
                              session};
    ToolDecision decision;
    store.insertToolInvocation(ctx, decision, std::chrono::milliseconds{5},
                               std::chrono::milliseconds{2}, true, "ok");
    EXPECT_EQ(count_rows(store.raw_db(), "tool_invocations"), 1);
}
