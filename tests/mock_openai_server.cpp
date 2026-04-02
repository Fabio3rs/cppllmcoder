#include "mock_openai_server.hpp"

#include <chrono>
#include <random>

namespace test_support {

using httplib::Request;
using httplib::Response;

namespace {

int pick_ephemeral_port(httplib::Server &srv, std::string &host) {
    auto port = srv.bind_to_any_port(host);
    if (port <= 0) {
        if (host != "0.0.0.0") {
            host = "0.0.0.0";
            port = srv.bind_to_any_port(host);
        }
        if (port <= 0) {
            throw std::runtime_error("MockOpenAIServer: failed to bind to " +
                                     host);
        }
    }
    return port;
}

void set_json_response(Response &res, const nlohmann::json &body,
                       int status = 200) {
    res.status = status;
    res.set_header("Content-Type", "application/json");
    res.set_content(body.dump(), "application/json");
}

} // namespace

MockOpenAIServer::MockOpenAIServer(std::string host) : host_(std::move(host)) {
    installRoutes();
}

MockOpenAIServer::~MockOpenAIServer() { stop(); }

void MockOpenAIServer::start() {
    try {
        port_ = pick_ephemeral_port(server_, host_);
    } catch (...) {
        port_ = -1;
        return;
    }
    thread_ = std::jthread([this] { server_.listen_after_bind(); });
    server_.wait_until_ready();
}

void MockOpenAIServer::stop() {
    server_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::string MockOpenAIServer::baseUrl() const {
    return "http://" + host_ + ":" + std::to_string(port_);
}

void MockOpenAIServer::setChatHandler(ChatHandler handler) {
    chat_handler_ = std::move(handler);
}

void MockOpenAIServer::setChatStreamHandler(ChatStreamHandler handler) {
    chat_stream_handler_ = std::move(handler);
}

void MockOpenAIServer::setEmbeddingHandler(EmbeddingHandler handler) {
    embedding_handler_ = std::move(handler);
}

void MockOpenAIServer::setModelListHandler(ModelListHandler handler) {
    model_list_handler_ = std::move(handler);
}

void MockOpenAIServer::installRoutes() {
    server_.Post(
        "/v1/chat/completions", [this](const Request &req, Response &res) {
            auto body = nlohmann::json::parse(req.body, nullptr, false);
            if (body.is_discarded()) {
                set_json_response(res, {{"error", "invalid json"}}, 400);
                return;
            }
            const bool wants_stream = body.contains("stream") &&
                                      body["stream"].is_boolean() &&
                                      body["stream"].get<bool>();
            if (wants_stream) {
                if (chat_stream_handler_) {
                    chat_stream_handler_(body, res);
                    return;
                }
                // default streaming: two deltas then finish + [DONE]
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [body, sent = false](size_t offset,
                                         httplib::DataSink &sink) mutable {
                        (void)offset;
                        if (sent) {
                            sink.done();
                            return false;
                        }
                        std::string stream;
                        const std::string id =
                            body.value("model", "mock-model") + "-stream-1";
                        stream +=
                            "data: {\"id\":\"" + id +
                            "\",\"object\":\"chat.completion.chunk\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":"
                            "\"hello \"},\"finish_reason\":null}]}\n\n";
                        stream +=
                            "data: {\"id\":\"" + id +
                            "\",\"object\":\"chat.completion.chunk\","
                            "\"choices\":[{\"index\":0,\"delta\":{\"content\":"
                            "\"world\"},\"finish_reason\":null}]}\n\n";
                        stream += "data: {\"id\":\"" + id +
                                  "\",\"object\":\"chat.completion.chunk\","
                                  "\"choices\":[{\"index\":0,\"delta\":{},"
                                  "\"finish_reason\":\"stop\"}]}\n\n";
                        stream += "data: [DONE]\n\n";
                        sink.write(stream.data(), stream.size());
                        sent = true;
                        sink.done();
                        return true;
                    });
            } else {
                auto reply = chat_handler_ ? chat_handler_(body)
                                           : defaultChatResponse(body);
                set_json_response(res, reply);
            }
        });

    server_.Post("/v1/embeddings", [this](const Request &req, Response &res) {
        auto body = nlohmann::json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            set_json_response(res, {{"error", "invalid json"}}, 400);
            return;
        }
        auto reply = embedding_handler_ ? embedding_handler_(body)
                                        : defaultEmbeddingResponse(body);
        set_json_response(res, reply);
    });

    server_.Get("/v1/models", [this](const Request &, Response &res) {
        auto reply = model_list_handler_ ? model_list_handler_()
                                         : defaultModelListResponse();
        set_json_response(res, reply);
    });
}

MockOpenAIServer::Json
MockOpenAIServer::defaultChatResponse(const Json &request) {
    // Echo prompt back with a simple tag; keep shape compatible with chat
    // completions.
    std::string content;
    if (request.contains("messages") && request["messages"].is_array() &&
        !request["messages"].empty()) {
        const auto &last = request["messages"].back();
        if (last.contains("content")) {
            content = last["content"].get<std::string>();
        }
    }
    return {{"id", "mock-chat-1"},
            {"object", "chat.completion"},
            {"created",
             static_cast<int>(
                 std::chrono::system_clock::now().time_since_epoch().count() %
                 1'000'000)},
            {"model", request.value("model", "mock-model")},
            {"choices",
             {{{"index", 0},
               {"finish_reason", "stop"},
               {"message",
                {{"role", "assistant"}, {"content", "(mock) " + content}}}}}},
            {"usage",
             {{"prompt_tokens", 1},
              {"completion_tokens", 1},
              {"total_tokens", 2}}}};
}

MockOpenAIServer::Json
MockOpenAIServer::defaultEmbeddingResponse(const Json &request) {
    std::vector<double> vec(4, 0.0);
    if (request.contains("input")) {
        std::seed_seq seed{static_cast<unsigned>(request.dump().size())};
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> dist(-1.0, 1.0);
        for (auto &v : vec)
            v = dist(rng);
    }
    return {
        {"object", "list"},
        {"data", {{{"object", "embedding"}, {"index", 0}, {"embedding", vec}}}},
        {"model", request.value("model", "mock-embedding")},
        {"usage", {{"prompt_tokens", 1}, {"total_tokens", 1}}}};
}

MockOpenAIServer::Json MockOpenAIServer::defaultModelListResponse() {
    return {{"object", "list"},
            {"data",
             {{{"id", "mock-gpt"}, {"object", "model"}},
              {{"id", "mock-embed"}, {"object", "model"}}}}};
}

} // namespace test_support
