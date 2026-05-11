// Simple configurable mock server that speaks a subset of the OpenAI HTTP API
// (chat completions, embeddings, models). Intended for unit tests only.
#pragma once

#include <functional>
#include <string>
#include <thread>

#include "httplib.h"
#include <nlohmann/json.hpp>

namespace test_support {

class MockOpenAIServer {
  public:
    using Json = nlohmann::json;
    using ChatHandler = std::function<Json(const Json &)>;
    using ChatStreamHandler =
        std::function<void(const Json &, httplib::Response &)>;
    using EmbeddingHandler = std::function<Json(const Json &)>;
    using ModelListHandler = std::function<Json()>;
    // Handler that can set any HTTP status + headers (used for error/retry
    // tests)
    using RawChatHandler =
        std::function<void(const Json &, httplib::Response &)>;

    explicit MockOpenAIServer(std::string host = "127.0.0.1");
    ~MockOpenAIServer();

    // Non-copyable, non-movable (server owns a thread).
    MockOpenAIServer(const MockOpenAIServer &) = delete;
    MockOpenAIServer &operator=(const MockOpenAIServer &) = delete;

    void start();
    void stop();

    std::string baseUrl() const; // e.g. http://127.0.0.1:18080
    int port() const { return port_; }

    void setChatHandler(ChatHandler handler);
    void setChatStreamHandler(ChatStreamHandler handler);
    void setEmbeddingHandler(EmbeddingHandler handler);
    void setModelListHandler(ModelListHandler handler);
    // Override ALL /v1/chat/completions traffic (stream or not) with a raw
    // handler; takes priority over setChatHandler / setChatStreamHandler.
    void setRawChatHandler(RawChatHandler handler);

  private:
    void installRoutes();
    static Json defaultChatResponse(const Json &request);
    static Json defaultEmbeddingResponse(const Json &request);
    static Json defaultModelListResponse();

    httplib::Server server_{};
    std::jthread thread_{};
    std::string host_;
    int port_{-1};

    ChatHandler chat_handler_{};
    ChatStreamHandler chat_stream_handler_{};
    RawChatHandler raw_chat_handler_{};
    EmbeddingHandler embedding_handler_{};
    ModelListHandler model_list_handler_{};
};

} // namespace test_support
