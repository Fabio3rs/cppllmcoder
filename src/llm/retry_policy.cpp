// src/llm/retry_policy.cpp
//
// See include/llm/retry_policy.hpp for the rationale.
//
// Error classification priority:
//   1. HTTP status (most reliable)
//   2. error.type  (OpenAI-compatible JSON body)
//   3. error.code  (string or int)
//   4. error.message keywords (last resort, brittle — used only for well-known
//      quota/billing phrases that don't have a unique code)
//
// Jitter strategy: Full Jitter (AWS recommendation).
//   cap_n  = min(cap, base * factor^attempt)
//   sleep  = uniform(0, cap_n)
//
// When Retry-After is present it overrides the computed backoff, but a small
// jitter is added to avoid thundering-herd among multiple clients that all
// received the same header value.

#include "llm/retry_policy.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <random>
#include <string>

namespace llm {

namespace {

// ── random helpers
// ────────────────────────────────────────────────────────────

// Thread-local RNG so callers don't need to synchronise.
std::mt19937 &rng() {
    thread_local std::mt19937 gen{std::random_device{}()};
    return gen;
}

float uniform(float lo, float hi) {
    if (hi <= lo)
        return lo;
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng());
}

// ── JSON helpers
// ──────────────────────────────────────────────────────────────

// Extract a string field from the "error" object in an OpenAI-compatible body.
// Returns empty string on any parse/access error.
std::string error_field(const std::string &body, const char *field) {
    try {
        auto j = nlohmann::json::parse(body, nullptr, /*exceptions=*/false);
        if (j.is_discarded())
            return {};
        const auto &err = j.contains("error") ? j["error"] : j;
        if (!err.is_object())
            return {};
        if (!err.contains(field))
            return {};
        const auto &val = err[field];
        if (val.is_string())
            return val.get<std::string>();
        if (val.is_number())
            return std::to_string(val.get<int>());
    } catch (...) {
    }
    return {};
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool contains(const std::string &haystack, const char *needle) {
    return lower(haystack).find(needle) != std::string::npos;
}

// ── Classification helpers
// ────────────────────────────────────────────────────

// Codes that are always non-retryable regardless of HTTP status.
bool is_non_retryable_code(const std::string &code) {
    static constexpr const char *non_retryable[] = {
        "insufficient_quota",
        "invalid_api_key",
        "account_deactivated",
        "billing_not_active",
        "context_length_exceeded",
        "model_not_found",
        "invalid_model",
        // LocalAI / OpenAI-compatible variants
        "content_filter",
        "content_policy_violation",
    };
    const std::string lc = lower(code);
    for (const auto *c : non_retryable) {
        if (lc == c)
            return true;
    }
    return false;
}

bool is_billing_message(const std::string &msg) {
    const std::string lc = lower(msg);
    return contains(lc, "exceeded your current quota") ||
           contains(lc, "check your plan and billing") ||
           contains(lc, "monthly spend") || contains(lc, "insufficient_quota");
}

RetryOutcome outcome_for_non_retryable_code(const std::string &code) {
    const std::string lc = lower(code);
    if (lc == "insufficient_quota" || lc == "billing_not_active" ||
        lc == "account_deactivated") {
        return RetryOutcome::StopBillingOrQuota;
    }
    return RetryOutcome::StopUserAction;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

std::chrono::milliseconds full_jitter_delay(int attempt,
                                            const RetryConfig &cfg) {
    const float cap = std::min(
        cfg.cap_s,
        cfg.base_delay_s * std::pow(cfg.factor, static_cast<float>(attempt)));
    const float seconds = uniform(0.0f, cap);
    return std::chrono::milliseconds{static_cast<long long>(seconds * 1000.0f)};
}

RetryDecision classify(long http_code, const std::string &error_body,
                       int retry_after_seconds, int attempt,
                       const RetryConfig &cfg) {
    const std::string err_type = error_field(error_body, "type");
    const std::string err_code = error_field(error_body, "code");
    const std::string err_msg = error_field(error_body, "message");

    // ── 1. Check error.code for known non-retryable values ──────────────────
    if (!err_code.empty() && is_non_retryable_code(err_code)) {
        return {outcome_for_non_retryable_code(err_code), {}, err_code};
    }

    // ── 2. HTTP 400/401/403/404 — never retry ───────────────────────────────
    if (http_code == 400 || http_code == 401 || http_code == 403 ||
        http_code == 404) {
        return {RetryOutcome::StopUserAction,
                {},
                "http " + std::to_string(http_code)};
    }

    // ── 3. Billing/quota phrases in the message ──────────────────────────────
    if (!err_msg.empty() && is_billing_message(err_msg)) {
        return {RetryOutcome::StopBillingOrQuota, {}, "billing/quota message"};
    }

    // ── 4. 429 — distinguish rate-limit vs quota ─────────────────────────────
    if (http_code == 429) {
        const bool is_rate_limit =
            err_type == "rate_limit_error" ||
            lower(err_code).find("rate_limit") != std::string::npos ||
            contains(err_msg, "rate limit") ||
            contains(err_msg, "rate_limit") ||
            contains(err_msg, "too many requests");

        if (is_rate_limit) {
            // Respect Retry-After if the server sent one.
            std::chrono::milliseconds delay{};
            std::string reason = "429 rate_limit";
            if (retry_after_seconds >= 0) {
                const float jitter = uniform(0.0f, cfg.jitter_extra_s);
                delay = std::chrono::milliseconds{static_cast<long long>(
                    (static_cast<float>(retry_after_seconds) + jitter) *
                    1000.0f)};
                reason +=
                    " (Retry-After: " + std::to_string(retry_after_seconds) +
                    "s)";
            } else {
                delay = full_jitter_delay(attempt, cfg);
            }
            return {RetryOutcome::Retry, delay, reason};
        }

        // Unknown 429 — cautious: try once more then stop.
        if (attempt == 0) {
            return {RetryOutcome::Retry, full_jitter_delay(attempt, cfg),
                    "429 unknown (cautious single retry)"};
        }
        return {RetryOutcome::StopBillingOrQuota,
                {},
                "429 unknown after first retry — assume quota/billing"};
    }

    // ── 5. Transient server/gateway errors ───────────────────────────────────
    if (http_code == 408 || http_code == 409 || http_code == 500 ||
        http_code == 502 || http_code == 503 || http_code == 504) {
        std::chrono::milliseconds delay{};
        if (retry_after_seconds >= 0 &&
            (http_code == 503 || http_code == 429)) {
            const float jitter = uniform(0.0f, cfg.jitter_extra_s);
            delay = std::chrono::milliseconds{static_cast<long long>(
                (static_cast<float>(retry_after_seconds) + jitter) * 1000.0f)};
        } else {
            delay = full_jitter_delay(attempt, cfg);
        }
        return {RetryOutcome::Retry, delay,
                "http " + std::to_string(http_code)};
    }

    // ── 6. Transport error (http_code == 0) ──────────────────────────────────
    if (http_code == 0) {
        return {RetryOutcome::Retry, full_jitter_delay(attempt, cfg),
                "transport error"};
    }

    // ── 7. Everything else — don't retry by default ──────────────────────────
    return {RetryOutcome::DoNotRetry,
            {},
            "unclassified http " + std::to_string(http_code)};
}

} // namespace llm
