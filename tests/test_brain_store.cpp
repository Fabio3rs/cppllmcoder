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

TEST(BrainStore, UpdatesExistingMessageWhenIdPresent) {
    BrainStore store = BrainStore::open(":memory:", /*enable_vector=*/false);

    SessionInfo session{.id = "sess-1"};
    store.ensureSession(session);

    Message msg{.id = 0,
                .role = MessageRole::Assistant,
                .content = "first",
                .session_id = session.id,
                .created_at = "t1",
                .updated_at = "t1",
                .duration = std::chrono::milliseconds{10},
                .token_count = 5};

    store.insertMessage(msg);
    ASSERT_GT(msg.id, 0);
    EXPECT_EQ(count_rows(store.raw_db(), "messages"), 1);

    // Mutate and persist again; should update same row (no new rows).
    const auto original_id = msg.id;
    msg.content = "second";
    msg.token_count = 42;
    msg.updated_at = "t2";
    store.insertMessage(msg);

    EXPECT_EQ(msg.id, original_id);
    EXPECT_EQ(count_rows(store.raw_db(), "messages"), 1);

    auto stmt = prepare_or_throw(
        store.raw_db(), "SELECT content, token_count FROM messages WHERE id=?;",
        "select message");
    sqlite3_bind_int64(stmt.get(), 1, msg.id);
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_EQ(
        reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0)),
        std::string("second"));
    EXPECT_EQ(sqlite3_column_int(stmt.get(), 1), 42);
}
