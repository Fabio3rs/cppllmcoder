#include "agent.hpp"
#include <chrono>
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
