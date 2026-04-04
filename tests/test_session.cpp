#include "agent.hpp"
#include "brain_store.hpp"
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>

class DummyLogger : public IExecutionLogger {
  public:
    void logMessage(const Message &msg, const SessionInfo &session) override {
        log_message_called = true;
        last_session_id = session.id;
        last_message_content = msg.content;
    }

    void logToolEvent(const ToolInvocationContext &ctx, const ToolDecision &,
                      std::chrono::milliseconds, bool, std::string_view,
                      const SessionInfo &session) override {
        log_tool_called = true;
        last_session_id = session.id;
        last_tool_name = ctx.metadata.name;
    }

    bool log_message_called = false;
    bool log_tool_called = false;
    std::string last_session_id;
    std::string last_message_content;
    std::string last_tool_name;
};

TEST(AgentSession, GeneratesIdWhenMissing) {
    app::Options opts;
    opts.db_path = ":memory:";
    Agent agent(opts, nullptr, nullptr, nullptr, nullptr, nullptr);

    const auto &session = agent.get_session_info();
    EXPECT_FALSE(session.id.empty());
    EXPECT_EQ(session.model, opts.model);
    EXPECT_EQ(session.endpoint, opts.endpoint);
}

TEST(AgentSession, RespectsSessionIdOverride) {
    app::Options opts;
    opts.db_path = ":memory:";
    opts.session_id_override = "fixed-session";
    Agent agent(opts, nullptr, nullptr, nullptr, nullptr, nullptr);

    EXPECT_EQ(agent.get_session_info().id, "fixed-session");
}

TEST(LoggerInterface, ReceivesSessionId) {
    DummyLogger logger;

    Message msg{.id = 1,
                .role = MessageRole::User,
                .content = "hi",
                .session_id = "sess-1",
                .created_at = "t1",
                .updated_at = "t1",
                .duration = std::chrono::milliseconds{0},
                .token_count = 0};

    SessionInfo session{.id = "sess-1"};

    ToolMetadata meta{.name = "dummy"};
    ToolInvocationContext ctx{meta, "{}", 0, std::chrono::milliseconds{0},
                              session};

    logger.logMessage(msg, session);
    logger.logToolEvent(ctx, {}, std::chrono::milliseconds{0}, true, "ok",
                        session);

    EXPECT_TRUE(logger.log_message_called);
    EXPECT_TRUE(logger.log_tool_called);
    EXPECT_EQ(logger.last_session_id, "sess-1");
    EXPECT_EQ(logger.last_tool_name, "dummy");
}

TEST(AgentSession, RestoresHistoryFromDb) {
    namespace fs = std::filesystem;
    auto db_path = fs::temp_directory_path() / "cppllmcoder_restore.db";
    std::error_code ec;
    fs::remove(db_path, ec);

    {
        BrainStore store = BrainStore::open(db_path.string(),
                                            /*enable_vector=*/false);
        SessionInfo session;
        session.id = "sess-restore";
        session.model = "model-a";
        session.model_version = "v1";
        session.endpoint = "endpoint";
        session.temperature = 0.5;
        session.top_p = 0.9;
        session.top_k = 40;
        session.max_tokens = 1000;
        session.seed = 123;
        session.params_json = "{}";

        store.ensureSession(session);

        const std::string now = "2024-01-01 00:00:00.000";
        Message sys{.id = 0,
                    .role = MessageRole::System,
                    .content = "sys",
                    .session_id = session.id,
                    .created_at = now,
                    .updated_at = now,
                    .duration = std::chrono::milliseconds{0},
                    .token_count = 1};
        Message user{.id = 0,
                     .role = MessageRole::User,
                     .content = "hi",
                     .session_id = session.id,
                     .created_at = now,
                     .updated_at = now,
                     .duration = std::chrono::milliseconds{0},
                     .token_count = 2};
        store.insertMessage(sys);
        store.insertMessage(user);
    }

    app::Options opts;
    opts.db_path = db_path.string();
    opts.restore_session_id = "sess-restore";
    opts.session_id_override = "sess-restore";
    opts.restore_history = true;

    Agent agent(opts, nullptr, nullptr, nullptr, nullptr, nullptr);

    const auto &hist = agent.get_history();
    ASSERT_EQ(hist.size(), 2);
    EXPECT_EQ(hist[0].role, MessageRole::System);
    EXPECT_EQ(hist[0].content, "sys");
    EXPECT_EQ(hist[1].role, MessageRole::User);
    EXPECT_EQ(hist[1].content, "hi");
    EXPECT_EQ(agent.get_session_info().id, "sess-restore");

    fs::remove(db_path, ec);
}
