#include <gtest/gtest.h>

#include "agent.hpp"
#include "llm_chat_streamer.hpp"
#include "mock_openai_server.hpp"

#include <atomic>
#include <nlohmann/json.hpp>
#include <openai/openai.hpp>
#include <optional>
#include <queue>

using nlohmann::json;

namespace {

struct TestDriver : public IAgentDriver {
    std::vector<std::string> tokens;
    std::vector<std::string> turn_complete;
    struct ToolEvent {
        std::string name;
        bool success;
        std::string summary;
    };
    std::vector<ToolEvent> tool_events;

    std::atomic<bool> stop_flag{false};
    size_t stop_after_tokens = 0;
    std::queue<std::string> injections;

    void on_token(std::string_view token) override {
        tokens.emplace_back(token);
        if (stop_after_tokens > 0 && tokens.size() >= stop_after_tokens) {
            stop_flag.store(true);
        }
    }

    void on_turn_complete(std::string_view response) override {
        turn_complete.emplace_back(response);
    }

    void on_tool_result(std::string_view tool_name, bool success,
                        std::string_view summary) override {
        tool_events.push_back(
            {std::string(tool_name), success, std::string(summary)});
    }

    bool stop_requested() const override {
        return stop_flag.load(std::memory_order_relaxed);
    }

    void request_stop() override { stop_flag.store(true); }

    std::optional<std::string> next_injection() override {
        if (injections.empty()) {
            return std::nullopt;
        }
        auto msg = std::move(injections.front());
        injections.pop();
        return msg;
    }

    void inject(std::string message) override {
        injections.push(std::move(message));
    }

    std::optional<std::chrono::milliseconds> timeout() const override {
        return std::nullopt;
    }

    bool should_finish(int) const override { return false; }
};

class NullToolRegistry : public ToolRegistry {
  public:
    void registerTool(std::shared_ptr<ITool>) override {}
    std::shared_ptr<ITool> findTool(std::string_view) const override {
        return nullptr;
    }
    std::vector<ToolMetadata> listMetadata() const override { return {}; }
    void forEach(const std::function<void(const ToolMetadata &, const ITool &)>
                     &fn) const override {
        (void)fn;
    }
    std::vector<ToolDocView> topKDocs(std::string_view, size_t) const override {
        return {};
    }
};

class NullPromptManager : public IPromptManager {
  public:
    std::string
    buildSystemPrompt(const std::vector<Message> &,
                      const std::vector<ToolDocView> &) const override {
        return {};
    }
    std::string
    buildToolDecisionPrompt(const ToolInvocationContext &) const override {
        return {};
    }
};

class AllowAllConsent : public IToolConsentProvider {
  public:
    ToolDecision requestToolUse(const ToolInvocationContext &) override {
        return {};
    }
};

class NullLogger : public IExecutionLogger {
  public:
    void logMessage(const Message &, const SessionInfo &) override {}
    void logToolEvent(const ToolInvocationContext &, const ToolDecision &,
                      std::chrono::milliseconds, bool, std::string_view,
                      const SessionInfo &) override {}
};

class CounterStats : public IStatsRecorder {
  public:
    void incrementTokenCount(int delta) override {
        if (delta > 0) {
            total += static_cast<size_t>(delta);
        }
    }
    size_t totalTokens() const override { return total; }

  private:
    size_t total = 0;
};

app::Options make_opts(std::string db_path, const std::string &endpoint) {
    app::Options opts;
    opts.db_path = std::move(db_path);
    opts.endpoint = endpoint;
    opts.model = "mock-model";
    return opts;
}

} // namespace

TEST(ChatStreamer, StopsOnDriverRequest) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP() << "mock server unavailable";
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    openai::OpenAI openai{"dummy", "", false, server.baseUrl() + "/v1/"};
    llm::ChatStreamer streamer(openai);
    TestDriver driver;
    driver.stop_after_tokens = 1;

    json req;
    req["model"] = "mock-model";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    const std::string reply = streamer.stream(req, driver);

    EXPECT_EQ(reply, "hello ");
    ASSERT_EQ(driver.turn_complete.size(), 1u);
    EXPECT_EQ(driver.turn_complete[0], "hello ");
    EXPECT_GE(driver.tokens.size(), 1u);

    server.stop();
}

TEST(ChatStreamer, CapturesUsageWhenPresent) {
    test_support::MockOpenAIServer server;
    server.setChatStreamHandler([](const json &body, httplib::Response &res) {
        (void)body;
        res.set_chunked_content_provider("text/event-stream", [](size_t,
                                                                 httplib::
                                                                     DataSink &
                                                                         sink) {
            std::string stream;
            stream +=
                "data: {\"choices\":[{\"delta\":{\"content\":\"foo\"}}]}\n\n";
            stream += "data: "
                      "{\"choices\":[],\"usage\":{\"prompt_tokens\":5,"
                      "\"completion_tokens\":2,\"total_tokens\":7}}\n\n";
            stream += "data: [DONE]\n\n";
            sink.write(stream.data(), stream.size());
            sink.done();
            return true;
        });
    });
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP() << "mock server unavailable";
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    openai::OpenAI openai{"dummy", "", false, server.baseUrl() + "/v1/"};
    llm::ChatStreamer streamer(openai);
    TestDriver driver;
    llm::Usage usage{};

    json req;
    req["model"] = "mock-model";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;
    req["stream_options"] = {{"include_usage", true}};

    const std::string reply = streamer.stream(req, driver, &usage);

    EXPECT_EQ(reply, "foo");
    EXPECT_EQ(usage.total_tokens, 7);
    EXPECT_EQ(usage.prompt_tokens, 5);
    EXPECT_EQ(usage.completion_tokens, 2);
    server.stop();
}

TEST(AgentRunStep, StreamsAndPersistsAssistant) {
    test_support::MockOpenAIServer server;
    server.setChatStreamHandler([](const json &body, httplib::Response &res) {
        (void)body;
        res.set_chunked_content_provider("text/event-stream", [](size_t,
                                                                 httplib::
                                                                     DataSink &
                                                                         sink) {
            std::string stream;
            stream += "data: {\"choices\":[{\"delta\":{\"content\":\"hello "
                      "\"}}]}\n\n";
            stream +=
                "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n";
            stream += "data: "
                      "{\"choices\":[],\"usage\":{\"prompt_tokens\":3,"
                      "\"completion_tokens\":2,\"total_tokens\":5}}\n\n";
            stream += "data: [DONE]\n\n";
            sink.write(stream.data(), stream.size());
            sink.done();
            return true;
        });
    });
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP() << "mock server unavailable";
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    auto opts = make_opts(":memory:", server.baseUrl() + "/v1/");
    auto tools = std::make_shared<NullToolRegistry>();
    auto prompts = std::make_shared<NullPromptManager>();
    auto consent = std::make_shared<AllowAllConsent>();
    auto logger = std::make_shared<NullLogger>();
    auto stats = std::make_shared<CounterStats>();
    Agent agent(opts, tools, prompts, consent, logger, stats);

    openai::OpenAI openai{"dummy", "", false, server.baseUrl() + "/v1/"};
    TestDriver driver;

    const std::string reply = agent.run_step("hi", driver, openai);

    EXPECT_EQ(reply, "hello world");
    ASSERT_EQ(driver.turn_complete.size(), 1u);
    EXPECT_EQ(driver.turn_complete[0], "hello world");
    EXPECT_GE(driver.tokens.size(), 2u);
    EXPECT_EQ(stats->totalTokens(), 5u);

    server.stop();
}

TEST(AgentRunStep, ExecutesLuaWhenCodePresent) {
    test_support::MockOpenAIServer server;
    server.setChatStreamHandler([](const json &body, httplib::Response &res) {
        (void)body;
        res.set_chunked_content_provider("text/event-stream", [](size_t,
                                                                 httplib::
                                                                     DataSink &
                                                                         sink) {
            std::string stream;
            stream +=
                "data: {\"choices\":[{\"delta\":{\"content\":\"<code>return "
                "'ok'</code>\"}}]}\n\n";
            stream += "data: [DONE]\n\n";
            sink.write(stream.data(), stream.size());
            sink.done();
            return true;
        });
    });
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP() << "mock server unavailable";
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    auto opts = make_opts(":memory:", server.baseUrl() + "/v1/");
    auto tools = std::make_shared<NullToolRegistry>();
    auto prompts = std::make_shared<NullPromptManager>();
    auto consent = std::make_shared<AllowAllConsent>();
    auto logger = std::make_shared<NullLogger>();
    auto stats = std::make_shared<CounterStats>();
    Agent agent(opts, tools, prompts, consent, logger, stats);

    openai::OpenAI openai{"dummy", "", false, server.baseUrl() + "/v1/"};
    TestDriver driver;

    const std::string reply = agent.run_step("hi", driver, openai);

    EXPECT_EQ(reply, "<code>return 'ok'</code>");
    ASSERT_EQ(driver.turn_complete.size(), 1u);
    EXPECT_EQ(driver.turn_complete[0], "<code>return 'ok'</code>");
    ASSERT_EQ(driver.tool_events.size(), 1u);
    EXPECT_TRUE(driver.tool_events[0].success);
    EXPECT_EQ(driver.tool_events[0].summary, "ok");

    server.stop();
}
