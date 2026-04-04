#include "brain_store.hpp"
#include "sqlite3raii.hpp"
#include <algorithm>
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

TEST(BrainStore, InsertExecutionLogReturnsId) {
    BrainStore store = BrainStore::open(":memory:", /*enable_vector=*/false);

    SessionInfo session{.id = "sess-exec"};
    store.ensureSession(session);

    const auto log_id =
        store.insertExecutionLog("task-1", session.id, "print('hi')", "out", "",
                                 3, std::chrono::milliseconds{10});

    EXPECT_GT(log_id, 0);
    EXPECT_EQ(count_rows(store.raw_db(), "execution_logs"), 1);
}

TEST(BrainStore, InsertPromptLogReturnsId) {
    BrainStore store = BrainStore::open(":memory:", /*enable_vector=*/false);

    SessionInfo session{.id = "sess-prompt"};
    store.ensureSession(session);

    const auto prompt_id = store.insertPromptLog(
        "task-2", session.id, "system", "model", "v1", "prompt", "completion",
        10, 9, std::chrono::milliseconds{12});

    EXPECT_GT(prompt_id, 0);
    EXPECT_EQ(count_rows(store.raw_db(), "prompt_logs"), 1);
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
    const auto msg_id = store.insertMessage(msg);
    EXPECT_GT(msg_id, 0);
    EXPECT_EQ(msg_id, msg.id);
    EXPECT_EQ(count_rows(store.raw_db(), "messages"), 1);

    ToolMetadata meta;
    meta.name = "echo";
    ToolInvocationContext ctx{meta, "{}", 0, std::chrono::milliseconds{0},
                              session};
    ToolDecision decision;
    const auto tool_id =
        store.insertToolInvocation(ctx, decision, std::chrono::milliseconds{5},
                                   std::chrono::milliseconds{2}, true, "ok");
    EXPECT_GT(tool_id, 0);
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

    const auto first_id = store.insertMessage(msg);
    ASSERT_GT(msg.id, 0);
    EXPECT_EQ(count_rows(store.raw_db(), "messages"), 1);

    // Mutate and persist again; should update same row (no new rows).
    const auto original_id = msg.id;
    msg.content = "second";
    msg.token_count = 42;
    msg.updated_at = "t2";
    const auto second_id = store.insertMessage(msg);

    EXPECT_EQ(msg.id, original_id);
    EXPECT_EQ(first_id, original_id);
    EXPECT_EQ(second_id, original_id);
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

TEST(BrainStore, ListsSessionsWithMessageCounts) {
    BrainStore store = BrainStore::open(":memory:", /*enable_vector=*/false);

    SessionInfo s1{.id = "s1", .model = "m1", .params_json = "{}"};
    SessionInfo s2{.id = "s2", .model = "m2", .params_json = "{}"};
    store.ensureSession(s1);
    store.ensureSession(s2);

    Message m1{.id = 0,
               .role = MessageRole::User,
               .content = "hello",
               .session_id = s1.id,
               .created_at = "t1",
               .updated_at = "t1",
               .duration = std::chrono::milliseconds{0},
               .token_count = 1};
    Message m2 = m1;
    m2.session_id = s2.id;
    store.insertMessage(m1);
    store.insertMessage(m2);

    auto sessions = store.listSessions();
    ASSERT_EQ(sessions.size(), 2);

    auto find_by_id = [&](const std::string &id) {
        return std::find_if(
            sessions.begin(), sessions.end(),
            [&](const SessionSummary &s) { return s.id == id; });
    };
    auto it1 = find_by_id("s1");
    auto it2 = find_by_id("s2");
    ASSERT_NE(it1, sessions.end());
    ASSERT_NE(it2, sessions.end());
    EXPECT_EQ(it1->message_count, 1);
    EXPECT_EQ(it2->message_count, 1);
}
