#include "stdafx.hpp"

#include "agent_driver.hpp"
#include "llm/retry_policy.hpp"
#include "llm_chat_streamer.hpp"

#include <chrono>
#include <thread>

namespace llm {

std::string ChatStreamer::stream(nlohmann::json request, IAgentDriver &driver,
                                 Usage *usage_out) {
    for (int attempt = 0; attempt < retry_cfg_.max_attempts; ++attempt) {
        Usage local_usage{};
        bool got_usage = false;
        if (driver.stop_requested()) {
            break;
        }

        control_.store(openai::StreamControl::Continue);
        std::string response_buf;
        bool saw_error = false;
        openai::StreamError last_error{};

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
                        local_usage.total_tokens =
                            usage["total_tokens"].get<int>();
                        got_usage = true;
                    }
                }
                if (!chunk.contains("choices") ||
                    !chunk["choices"].is_array() || chunk["choices"].empty()) {
                    return openai::StreamControl::Continue;
                }
                const auto &choice = chunk["choices"][0];
                if (!choice.contains("delta")) {
                    return openai::StreamControl::Continue;
                }
                const auto &delta = choice["delta"];
                if (delta.contains("content") && delta["content"].is_string()) {
                    const std::string piece =
                        delta["content"].get<std::string>();
                    response_buf += piece;
                    driver.on_token(piece);
                }
                if (driver.stop_requested()) {
                    control_.store(openai::StreamControl::Stop);
                }
                return control_.load();
            },
            // on_done — only fire on the final successful attempt
            [&] { /* handled after the loop */ },
            // on_error — capture error context for classification
            [&](const openai::StreamError &err) {
                saw_error = true;
                last_error = err;
            },
            // control
            [&] { return control_.load(); }};

        try {
            client_.chat.stream(request, cb); // keep request for retries
        } catch (const std::exception &e) {
            saw_error = true;
            last_error = {e.what(), {}, 0, -1};
        }

        // ── Success ─────────────────────────────────────────────────────────
        if (!saw_error) {
            driver.on_turn_complete(response_buf);
            if (usage_out && got_usage) {
                *usage_out = local_usage;
            }
            return response_buf;
        }

        // ── Classify error ───────────────────────────────────────────────────
        const auto decision =
            llm::classify(last_error.http_code, last_error.raw_body,
                          last_error.retry_after_seconds, attempt, retry_cfg_);

        const bool is_last_attempt = (attempt + 1 >= retry_cfg_.max_attempts);

        if (decision.outcome != RetryOutcome::Retry || is_last_attempt) {
            // Non-retryable or exhausted — surface to driver and return.
            const std::string prefix =
                decision.outcome == RetryOutcome::StopBillingOrQuota
                    ? "[quota/billing] "
                : decision.outcome == RetryOutcome::StopUserAction
                    ? "[auth/config] "
                : is_last_attempt ? "[retry exhausted] "
                                  : "[error] ";
            driver.on_token(prefix + last_error.message);
            driver.on_turn_complete(response_buf);
            return response_buf;
        }

        // ── Wait with interruptible sleep ────────────────────────────────────
        const auto wake_at = std::chrono::steady_clock::now() + decision.delay;
        while (std::chrono::steady_clock::now() < wake_at) {
            if (driver.stop_requested()) {
                return {};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        // Notify driver that a new attempt is starting.
        //
        // PARTIAL TOKEN WARNING: if the previous attempt failed mid-stream
        // (e.g. TCP reset after some chunks), on_token() will already have
        // been called with those partial chunks. response_buf is discarded
        // here, but the driver may have already rendered that content (e.g.
        // a TUI has already displayed it). on_retry() gives drivers a chance
        // to clear/overwrite the partial output. Drivers that don't need
        // visual cleanup can leave the default no-op implementation.
        //
        // In practice, most retryable errors (401, 429, 500) are returned by
        // the server before any content chunks, so no tokens are forwarded
        // before the error in the typical case.
        driver.on_retry(attempt + 1);
        // response_buf is discarded (partial tokens dropped); next iteration
        // starts a fresh attempt.
    }

    // Only reached if stop_requested() interrupted the retry loop before any
    // successful attempt — the last response_buf was local and already dropped.
    return {};
}

} // namespace llm
