#include <gtest/gtest.h>

#include "cockpit_chat.hpp"
#include "llm/retry_policy.hpp"
#include "llm_chat_streamer.hpp"
#include "mock_openai_server.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <openai/openai.hpp>
#include <thread>

using nlohmann::json;

TEST(ChatStreaming, HappyPathAggregatesTokens) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (const std::exception &e) {
        GTEST_SKIP() << e.what();
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    openai::OpenAI openai{"dummy-key", "", false, server.baseUrl() + "/v1/"};

    json request;
    request["model"] = "mock-model";
    request["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    request["stream"] = true;

    std::string collected;
    bool done = false;
    bool errored = false;

    openai.chat.stream(
        request, openai::CategoryChat::StreamCallbacks{
                     [&](const json &chunk) {
                         const auto &choice = chunk["choices"][0];
                         if (choice.contains("delta") &&
                             choice["delta"].contains("content")) {
                             collected +=
                                 choice["delta"]["content"].get<std::string>();
                         }
                         return openai::StreamControl::Continue;
                     },
                     [&] { done = true; },
                     [&](const openai::StreamError &err) {
                         errored = true;
                         FAIL() << "stream error: " << err.message;
                     },
                     {},
                 });

    EXPECT_FALSE(errored);
    EXPECT_TRUE(done);
    EXPECT_EQ(collected, "hello world");

    server.stop();
}

TEST(ChatStreaming, StopEarlyAbortsStream) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (const std::exception &e) {
        GTEST_SKIP() << e.what();
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    openai::OpenAI openai{"dummy-key", "", false, server.baseUrl() + "/v1/"};

    json request;
    request["model"] = "mock-model";
    request["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    request["stream"] = true;

    std::string collected;
    bool done = false;
    bool errored = false;
    int calls = 0;

    openai.chat.stream(
        request, openai::CategoryChat::StreamCallbacks{
                     [&](const json &chunk) {
                         ++calls;
                         const auto &choice = chunk["choices"][0];
                         if (choice.contains("delta") &&
                             choice["delta"].contains("content")) {
                             collected +=
                                 choice["delta"]["content"].get<std::string>();
                         }
                         return calls == 1 ? openai::StreamControl::Stop
                                           : openai::StreamControl::Continue;
                     },
                     [&] { done = true; },
                     [&](const openai::StreamError &err) {
                         (void)err;
                         errored = true;
                     },
                     {},
                 });

    EXPECT_FALSE(errored);
    EXPECT_TRUE(done);
    EXPECT_EQ(collected, "hello ");

    server.stop();
}

TEST(ChatStreaming, PauseThenContinueDeliversAll) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (const std::exception &e) {
        GTEST_SKIP() << e.what();
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    openai::OpenAI openai{"dummy-key", "", false, server.baseUrl() + "/v1/"};

    json request;
    request["model"] = "mock-model";
    request["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    request["stream"] = true;

    std::string collected;
    bool done = false;
    bool errored = false;
    bool paused_once = false;

    openai.chat.stream(
        request, openai::CategoryChat::StreamCallbacks{
                     [&](const json &chunk) {
                         const auto &choice = chunk["choices"][0];
                         if (choice.contains("delta") &&
                             choice["delta"].contains("content")) {
                             collected +=
                                 choice["delta"]["content"].get<std::string>();
                         }
                         if (!paused_once) {
                             paused_once = true;
                             return openai::StreamControl::Pause;
                         }
                         return openai::StreamControl::Continue;
                     },
                     [&] { done = true; },
                     [&](const openai::StreamError &err) {
                         errored = true;
                         FAIL() << "stream error: " << err.message;
                     },
                     {},
                 });

    EXPECT_FALSE(errored);
    EXPECT_TRUE(done);
    EXPECT_EQ(collected, "hello world");

    server.stop();
}

TEST(ChatStreaming, StopWhileWaitingReturnsCleanly) {
    test_support::MockOpenAIServer server;
    server.setChatStreamHandler([](const json &body, httplib::Response &res) {
        (void)body;
        res.set_chunked_content_provider(
            "text/event-stream", [](size_t, httplib::DataSink &sink) {
                std::this_thread::sleep_for(std::chrono::milliseconds{300});
                std::string stream;
                stream += "data: {\"choices\":[{\"delta\":"
                          "{\"content\":\"late\"}}]}\n\n";
                stream += "data: [DONE]\n\n";
                sink.write(stream.data(), stream.size());
                sink.done();
                return true;
            });
    });
    try {
        server.start();
    } catch (const std::exception &e) {
        GTEST_SKIP() << e.what();
        return;
    }
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    openai::OpenAI openai{"dummy-key", "", false, server.baseUrl() + "/v1/"};

    json request;
    request["model"] = "mock-model";
    request["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    request["stream"] = true;

    std::string collected;
    bool done = false;
    bool errored = false;
    std::atomic<bool> stop_requested{false};
    std::promise<void> started;
    auto started_future = started.get_future();

    std::thread worker([&] {
        started.set_value();
        openai.chat.stream(
            request,
            openai::CategoryChat::StreamCallbacks{
                [&](const json &chunk) {
                    const auto &choice = chunk["choices"][0];
                    if (choice.contains("delta") &&
                        choice["delta"].contains("content")) {
                        collected +=
                            choice["delta"]["content"].get<std::string>();
                    }
                    return openai::StreamControl::Continue;
                },
                [&] { done = true; },
                [&](const openai::StreamError &err) {
                    (void)err;
                    errored = true;
                },
                [&] {
                    return stop_requested.load()
                               ? openai::StreamControl::Stop
                               : openai::StreamControl::Continue;
                },
            });
    });

    started_future.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    stop_requested.store(true);
    worker.join();

    EXPECT_FALSE(errored);
    EXPECT_TRUE(done);
    EXPECT_TRUE(collected.empty());

    server.stop();
}

TEST(SseParser, HandlesCRLFAndMultipleDataLines) {
    using openai::_detail::SseEvent;
    using openai::_detail::SseParser;

    std::vector<SseEvent> events;
    SseParser parser([&](const SseEvent &ev) { events.push_back(ev); });

    const std::string chunk = "event: foo\r\n"
                              "data: line1\r\n"
                              "data: line2\r\n"
                              "\r\n"
                              "data: [DONE]\r\n"
                              "\r\n";

    ASSERT_TRUE(parser.feed(chunk));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].event, "foo");
    EXPECT_EQ(events[0].data, "line1\nline2");
    EXPECT_EQ(events[1].data, "[DONE]");
}

// ─────────────────────────────────────────────────────────────────────────────
// ChatStreamer retry integration tests
// Uses MockOpenAIServer with a raw handler that fails N times then succeeds.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Minimal IAgentDriver stub that records tokens and turn-complete calls.
struct StubDriver : IAgentDriver {
    std::string tokens;
    std::string last_turn;
    int turn_count = 0;
    int retry_count = 0;

    void on_token(std::string_view t) override { tokens += t; }
    void on_turn_complete(std::string_view r) override {
        last_turn = r;
        ++turn_count;
    }
    void on_retry(int /*attempt*/) override { ++retry_count; }
    void on_tool_result(std::string_view, bool, std::string_view) override {}
    bool stop_requested() const override { return false; }
    void request_stop() override {}
    std::optional<std::string> next_injection() override { return {}; }
    void inject(std::string) override {}
    std::optional<std::chrono::milliseconds> timeout() const override {
        return {};
    }
    bool should_finish(int) const override { return false; }
};

struct ThrowOnFirstTokenDriver : StubDriver {
    bool first_token_seen = false;

    void on_token(std::string_view t) override {
        tokens += t;
        if (!first_token_seen) {
            first_token_seen = true;
            throw std::runtime_error("simulated stream failure");
        }
    }
};

// Helper: build a default SSE stream response (same as mock default).
void send_default_stream(httplib::Response &res) {
    const std::string id = "retry-stream-1";
    std::string body;
    body += "data: {\"id\":\"" + id +
            "\",\"object\":\"chat.completion.chunk\","
            "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ok\"},"
            "\"finish_reason\":null}]}\n\n";
    body += "data: [DONE]\n\n";
    res.status = 200;
    res.set_header("Content-Type", "text/event-stream");
    res.set_content(body, "text/event-stream");
}

} // namespace

TEST(ChatStreamerRetry, SucceedsImmediatelyWithNoErrors) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP();
    }
    if (server.port() <= 0)
        GTEST_SKIP();

    openai::OpenAI client{"dummy-key", "", false, server.baseUrl() + "/v1/"};
    StubDriver driver;

    llm::RetryConfig cfg;
    cfg.max_attempts = 3;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;

    llm::ChatStreamer streamer(client, cfg);
    json req;
    req["model"] = "mock";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    const std::string result = streamer.stream(req, driver);
    EXPECT_EQ(result, "hello world");
    EXPECT_EQ(driver.turn_count, 1);
    // tokens should contain only content, not error prefixes
    EXPECT_EQ(driver.tokens, "hello world");
    EXPECT_EQ(driver.tokens.find("[error]"),
              std::string::npos);     // no error tokens
    EXPECT_EQ(driver.retry_count, 0); // no retries on immediate success

    server.stop();
}

TEST(ChatStreamerRetry, Retries500AndEventuallySucceeds) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP();
    }
    if (server.port() <= 0)
        GTEST_SKIP();

    std::atomic<int> call_count{0};
    server.setRawChatHandler([&](const json &, httplib::Response &res) {
        const int n = ++call_count;
        if (n < 3) {
            // First 2 calls → 500
            res.status = 500;
            res.set_header("Content-Type", "application/json");
            res.set_content(
                R"({"error":{"type":"server_error","message":"overloaded"}})",
                "application/json");
            return;
        }
        send_default_stream(res);
    });

    openai::OpenAI client{"dummy-key", "", false, server.baseUrl() + "/v1/"};
    StubDriver driver;

    llm::RetryConfig cfg;
    cfg.max_attempts = 5;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;
    cfg.jitter_extra_s = 0.0f;

    llm::ChatStreamer streamer(client, cfg);
    json req;
    req["model"] = "mock";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    const std::string result = streamer.stream(req, driver);
    EXPECT_EQ(result, "ok");
    EXPECT_EQ(call_count.load(), 3);
    EXPECT_EQ(driver.turn_count, 1);
    // 2 failures → 2 retries → on_retry called twice
    EXPECT_EQ(driver.retry_count, 2);

    server.stop();
}

TEST(ChatStreamerRetry, DoesNotLeakUsageFromFailedAttempt) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP();
    }
    if (server.port() <= 0)
        GTEST_SKIP();

    std::atomic<int> call_count{0};
    server.setRawChatHandler([&](const json &, httplib::Response &res) {
        const int n = ++call_count;
        if (n == 1) {
            std::string body;
            body +=
                "data: {\"id\":\"retry-stream-usage\","
                "\"object\":\"chat.completion.chunk\","
                "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":22,"
                "\"total_tokens\":33},"
                "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"part\"},"
                "\"finish_reason\":null}]}\n\n";
            res.status = 200;
            res.set_header("Content-Type", "text/event-stream");
            res.set_content(body, "text/event-stream");
            return;
        }
        send_default_stream(res);
    });

    openai::OpenAI client{"dummy-key", "", false, server.baseUrl() + "/v1/"};
    ThrowOnFirstTokenDriver driver;

    llm::RetryConfig cfg;
    cfg.max_attempts = 3;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;
    cfg.jitter_extra_s = 0.0f;

    llm::ChatStreamer streamer(client, cfg);
    json req;
    req["model"] = "mock";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    llm::Usage usage{};
    const std::string result = streamer.stream(req, driver, &usage);

    EXPECT_EQ(result, "ok");
    EXPECT_EQ(call_count.load(), 2);
    EXPECT_EQ(driver.retry_count, 1);
    EXPECT_EQ(usage.prompt_tokens, -1);
    EXPECT_EQ(usage.completion_tokens, -1);
    EXPECT_EQ(usage.total_tokens, -1);

    server.stop();
}

TEST(ChatStreamerRetry, Retries429RateLimitAndEventuallySucceeds) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP();
    }
    if (server.port() <= 0)
        GTEST_SKIP();

    std::atomic<int> call_count{0};
    server.setRawChatHandler([&](const json &, httplib::Response &res) {
        const int n = ++call_count;
        if (n == 1) {
            res.status = 429;
            res.set_header("Content-Type", "application/json");
            res.set_content(
                R"({"error":{"type":"rate_limit_error","code":"rate_limit_exceeded","message":"Rate limit reached"}})",
                "application/json");
            return;
        }
        send_default_stream(res);
    });

    openai::OpenAI client{"dummy-key", "", false, server.baseUrl() + "/v1/"};
    StubDriver driver;

    llm::RetryConfig cfg;
    cfg.max_attempts = 3;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;
    cfg.jitter_extra_s = 0.0f;

    llm::ChatStreamer streamer(client, cfg);
    json req;
    req["model"] = "mock";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    const std::string result = streamer.stream(req, driver);
    EXPECT_EQ(result, "ok");
    EXPECT_EQ(call_count.load(), 2);
    EXPECT_EQ(driver.turn_count, 1);

    server.stop();
}

TEST(ChatStreamerRetry, DoesNotRetryOnInsufficientQuota) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP();
    }
    if (server.port() <= 0)
        GTEST_SKIP();

    std::atomic<int> call_count{0};
    server.setRawChatHandler([&](const json &, httplib::Response &res) {
        ++call_count;
        res.status = 429;
        res.set_header("Content-Type", "application/json");
        res.set_content(
            R"({"error":{"type":"invalid_request_error","code":"insufficient_quota","message":"You exceeded your current quota"}})",
            "application/json");
    });

    openai::OpenAI client{"dummy-key", "", false, server.baseUrl() + "/v1/"};
    StubDriver driver;

    llm::RetryConfig cfg;
    cfg.max_attempts = 5;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;

    llm::ChatStreamer streamer(client, cfg);
    json req;
    req["model"] = "mock";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    streamer.stream(req, driver);
    // Should have called the server exactly once — no retry for quota errors.
    EXPECT_EQ(call_count.load(), 1);
    EXPECT_EQ(driver.turn_count, 1);
    EXPECT_NE(driver.tokens.find("quota"), std::string::npos)
        << "tokens: " << driver.tokens;

    server.stop();
}

TEST(ChatStreamerRetry, DoesNotRetryOnInvalidApiKey) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP();
    }
    if (server.port() <= 0)
        GTEST_SKIP();

    std::atomic<int> call_count{0};
    server.setRawChatHandler([&](const json &, httplib::Response &res) {
        ++call_count;
        res.status = 401;
        res.set_header("Content-Type", "application/json");
        res.set_content(
            R"({"error":{"type":"authentication_error","code":"invalid_api_key","message":"Incorrect API key"}})",
            "application/json");
    });

    openai::OpenAI client{"dummy-key", "", false, server.baseUrl() + "/v1/"};
    StubDriver driver;

    llm::RetryConfig cfg;
    cfg.max_attempts = 5;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;

    llm::ChatStreamer streamer(client, cfg);
    json req;
    req["model"] = "mock";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    streamer.stream(req, driver);
    EXPECT_EQ(call_count.load(), 1);
    EXPECT_NE(driver.tokens.find("auth"), std::string::npos)
        << "tokens: " << driver.tokens;

    server.stop();
}

TEST(ChatStreamerRetry, ExhaustsRetriesAndSurfaces) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (...) {
        GTEST_SKIP();
    }
    if (server.port() <= 0)
        GTEST_SKIP();

    std::atomic<int> call_count{0};
    server.setRawChatHandler([&](const json &, httplib::Response &res) {
        ++call_count;
        res.status = 503;
        res.set_header("Content-Type", "application/json");
        res.set_content(
            R"({"error":{"type":"server_error","message":"Service unavailable"}})",
            "application/json");
    });

    openai::OpenAI client{"dummy-key", "", false, server.baseUrl() + "/v1/"};
    StubDriver driver;

    llm::RetryConfig cfg;
    cfg.max_attempts = 3;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;
    cfg.jitter_extra_s = 0.0f;

    llm::ChatStreamer streamer(client, cfg);
    json req;
    req["model"] = "mock";
    req["messages"] = {{{"role", "user"}, {"content", "hi"}}};
    req["stream"] = true;

    streamer.stream(req, driver);
    EXPECT_EQ(call_count.load(), cfg.max_attempts);
    EXPECT_EQ(driver.turn_count, 1);
    EXPECT_NE(driver.tokens.find("retry exhausted"), std::string::npos)
        << "tokens: " << driver.tokens;

    server.stop();
}

TEST(CockpitChat, RetryCleanupTargetsMatchingAgent) {
    std::vector<ChatItem> conversation{
        {ChatRole::User, "user", "hello", ""},
        {ChatRole::Assistant, "assistant-a", "partial-a", "agent-a"},
        {ChatRole::Tool, "tool", "later tool", ""},
        {ChatRole::Assistant, "assistant-b", "partial-b", "agent-b"},
    };

    int selected_chat = 1;
    EXPECT_TRUE(clearRetryTarget(conversation, "agent-a", &selected_chat));
    EXPECT_EQ(conversation[1].text, "");
    EXPECT_EQ(conversation[3].text, "partial-b");
    EXPECT_EQ(conversation[2].text, "later tool");
    EXPECT_EQ(selected_chat, 1);
}
