#pragma once
// llm/retry_policy.hpp
//
// Stateless classifier for OpenAI-compatible HTTP errors.
//
// Design notes:
//   - This module is PURE: no I/O, no threading, no OpenAI headers.
//     Tests can exercise it without a mock server.
//   - Taxonomy follows the analysis in docs/retry_policy_rationale.md:
//       Retry    : transport errors, 408, 409, rate_limit_exceeded 429, 5xx
//       Stop     : auth (401), permission (403), invalid payload (400),
//                  insufficient_quota / billing 429, model-not-found (404)
//   - Delay uses Full Jitter (AWS recommendation) unless Retry-After is set,
//     in which case Retry-After + small jitter takes priority.

#include <chrono>
#include <string>

namespace llm {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

struct RetryConfig {
    int max_attempts = 5;        // total attempts (1 = no retry)
    float base_delay_s = 0.5f;   // initial backoff seed (seconds)
    float factor = 2.0f;         // exponential growth factor
    float cap_s = 30.0f;         // maximum individual sleep (seconds)
    float jitter_extra_s = 1.0f; // extra jitter added after Retry-After
};

// ─────────────────────────────────────────────────────────────────────────────
// Decision
// ─────────────────────────────────────────────────────────────────────────────

enum class RetryOutcome {
    Retry,              // transient — wait and try again
    StopUserAction,     // auth / invalid-request — user must act
    StopBillingOrQuota, // quota / billing exhausted — user must act
    DoNotRetry,         // ambiguous — don't retry by default
};

struct RetryDecision {
    RetryOutcome outcome;
    std::chrono::milliseconds delay{0};
    std::string reason; // short human-readable explanation for logging
};

// ─────────────────────────────────────────────────────────────────────────────
// Main classifier
//
// Parameters:
//   http_code           — raw HTTP status (0 = transport error / no response)
//   error_body          — raw response body (may be JSON {"error":{...}})
//   retry_after_seconds — value from Retry-After header (-1 if absent)
//   attempt             — 0-based attempt index (0 = first call)
//   cfg                 — retry configuration
// ─────────────────────────────────────────────────────────────────────────────

RetryDecision classify(long http_code, const std::string &error_body,
                       int retry_after_seconds, int attempt,
                       const RetryConfig &cfg = {});

// Convenience: compute full-jitter delay for a given attempt without calling
// classify().  Useful for tests.
std::chrono::milliseconds full_jitter_delay(int attempt,
                                            const RetryConfig &cfg = {});

} // namespace llm
