#include "stdafx.hpp"

#include "agent_driver.hpp"
#include "llm_chat_streamer.hpp"

namespace llm {

std::string ChatStreamer::stream(nlohmann::json request, IAgentDriver &driver,
                                 Usage *usage_out) {
    control_.store(openai::StreamControl::Continue);
    std::string response_buf;
    bool saw_error = false;
    Usage local_usage{};
    bool got_usage = false;

    openai::CategoryChat::StreamCallbacks cb{
        // on_data
        [&](const nlohmann::json &chunk) -> openai::StreamControl {
            if (chunk.contains("usage") && !chunk["usage"].is_null() &&
                chunk["usage"].is_object()) {
                const auto &usage = chunk["usage"];
                if (usage.contains("prompt_tokens") &&
                    usage["prompt_tokens"].is_number_integer() &&
                    usage.contains("completion_tokens") &&
                    usage["completion_tokens"].is_number_integer() &&
                    usage.contains("total_tokens") &&
                    usage["total_tokens"].is_number_integer()) {
                    local_usage.prompt_tokens =
                        usage["prompt_tokens"].get<int>();
                    local_usage.completion_tokens =
                        usage["completion_tokens"].get<int>();
                    local_usage.total_tokens = usage["total_tokens"].get<int>();
                    got_usage = true;
                }
            }
            if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
                chunk["choices"].empty()) {
                return openai::StreamControl::Continue;
            }
            const auto &choice = chunk["choices"][0];
            if (!choice.contains("delta")) {
                return openai::StreamControl::Continue;
            }
            const auto &delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string()) {
                const std::string piece = delta["content"].get<std::string>();
                response_buf += piece;
                driver.on_token(piece);
            }
            if (driver.stop_requested()) {
                control_.store(openai::StreamControl::Stop);
            }
            return control_.load();
        },
        // on_done
        [&] { driver.on_turn_complete(response_buf); },
        // on_error
        [&](const std::string &err) {
            saw_error = true;
            driver.on_token(std::string{"[error] "} + err);
            driver.on_turn_complete(response_buf);
        },
        // control
        [&] { return control_.load(); }};

    try {
        client_.chat.stream(std::move(request), cb);
    } catch (const std::exception &e) {
        saw_error = true;
        driver.on_token(std::string{"[exception] "} + e.what());
        driver.on_turn_complete(response_buf);
    }

    (void)saw_error; // reserved for future telemetry
    if (usage_out && got_usage) {
        *usage_out = local_usage;
    }
    return response_buf;
}

} // namespace llm
