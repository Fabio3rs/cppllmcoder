#include <gtest/gtest.h>

#include "mock_openai_server.hpp"
#include <openai/openai.hpp>

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
                     [&](const std::string &err) {
                         errored = true;
                         FAIL() << "stream error: " << err;
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
                     [&](const std::string &err) {
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
                     [&](const std::string &err) {
                         errored = true;
                         FAIL() << "stream error: " << err;
                     },
                     {},
                 });

    EXPECT_FALSE(errored);
    EXPECT_TRUE(done);
    EXPECT_EQ(collected, "hello world");

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
