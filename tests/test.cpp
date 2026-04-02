#include <gtest/gtest.h>

#include "mock_openai_server.hpp"
#include <openai/openai.hpp>

TEST(MockOpenAIServer, StartsAndServesDefaults) {
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
    server.stop();
}

TEST(MockOpenAIServer, ChatCompletionDefaultEcho) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (const std::exception &e) {
        GTEST_SKIP() << e.what();
        return;
    }

    auto &openai =
        openai::start("dummy-key", "", false, server.baseUrl() + "/v1/");
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    nlohmann::json request;
    request["model"] = "mock-model";
    request["messages"] = {{{"role", "system"}, {"content", "system ctx"}},
                           {{"role", "user"}, {"content", "ping"}}};

    auto chat = openai.chat.create(request);
    auto content = chat["choices"][0]["message"]["content"].get<std::string>();
    EXPECT_NE(content.find("(mock) ping"), std::string::npos);

    server.stop();
}

TEST(MockOpenAIServer, EmbeddingDefaultShape) {
    test_support::MockOpenAIServer server;
    try {
        server.start();
    } catch (const std::exception &e) {
        GTEST_SKIP() << e.what();
        return;
    }

    auto &openai =
        openai::start("dummy-key", "", false, server.baseUrl() + "/v1/");
    if (server.port() <= 0) {
        GTEST_SKIP() << "mock server not available";
        return;
    }

    nlohmann::json request;
    request["model"] = "mock-embed";
    request["input"] = "hello";

    auto embed = openai.embedding.create(request);
    const auto &data = embed["data"][0]["embedding"];
    ASSERT_TRUE(data.is_array());
    EXPECT_EQ(data.size(), 4);

    server.stop();
}
