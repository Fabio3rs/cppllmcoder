// tests/test_retry_policy.cpp
//
// Unit tests for llm::classify() — no network, no mock server.
// Tests cover the taxonomy described in include/llm/retry_policy.hpp.

#include <gtest/gtest.h>

#include "llm/retry_policy.hpp"
#include <cmath>

using llm::classify;
using llm::full_jitter_delay;
using llm::RetryConfig;
using llm::RetryDecision;
using llm::RetryOutcome;

namespace {

// ── helpers ──────────────────────────────────────────────────────────────────

std::string make_error(const char *type, const char *code,
                       const char *message = "") {
    return std::string{R"({"error":{"type":")"} + type + R"(","code":")" +
           code + R"(","message":")" + message + R"("}})";
}

RetryConfig instant_cfg() {
    // Zero delays so tests don't actually sleep.
    RetryConfig cfg;
    cfg.base_delay_s = 0.0f;
    cfg.cap_s = 0.0f;
    cfg.jitter_extra_s = 0.0f;
    cfg.max_attempts = 5;
    return cfg;
}

// ── full_jitter_delay ────────────────────────────────────────────────────────

TEST(FullJitterDelay, IsWithinBounds) {
    const RetryConfig cfg;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const auto d = full_jitter_delay(attempt, cfg);
        EXPECT_GE(d.count(), 0) << "attempt " << attempt;
        // Upper bound: min(cap, base * factor^attempt) converted to ms
        const float cap_s = std::min(
            cfg.cap_s, cfg.base_delay_s *
                           std::pow(cfg.factor, static_cast<float>(attempt)));
        EXPECT_LE(d.count(), static_cast<long long>(cap_s * 1000.0f) + 1)
            << "attempt " << attempt;
    }
}

TEST(FullJitterDelay, IsCappedAtMaxDelay) {
    RetryConfig cfg;
    cfg.base_delay_s = 1.0f;
    cfg.factor = 10.0f;
    cfg.cap_s = 5.0f;
    for (int i = 0; i < 20; ++i) {
        const auto d = full_jitter_delay(10, cfg);
        EXPECT_LE(d.count(), 5000LL);
    }
}

// ── Transport errors (http_code == 0) ────────────────────────────────────────

TEST(Classify, TransportErrorIsRetryable) {
    auto d = classify(0, "", -1, 0, instant_cfg());
    EXPECT_EQ(d.outcome, RetryOutcome::Retry);
}

// ── 5xx ──────────────────────────────────────────────────────────────────────

TEST(Classify, Http500IsRetryable) {
    EXPECT_EQ(classify(500, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http502IsRetryable) {
    EXPECT_EQ(classify(502, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http503IsRetryable) {
    EXPECT_EQ(classify(503, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http504IsRetryable) {
    EXPECT_EQ(classify(504, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http408IsRetryable) {
    EXPECT_EQ(classify(408, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http409IsRetryable) {
    EXPECT_EQ(classify(409, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

// ── 429 rate_limit
// ────────────────────────────────────────────────────────────

TEST(Classify, Http429RateLimitTypeIsRetryable) {
    const auto body = make_error("rate_limit_error", "rate_limit_exceeded");
    EXPECT_EQ(classify(429, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http429RateLimitCodeIsRetryable) {
    const auto body = make_error("", "rate_limit_exceeded");
    EXPECT_EQ(classify(429, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http429RateLimitMessageIsRetryable) {
    const auto body = make_error("", "", "Rate limit reached for gpt-4o");
    EXPECT_EQ(classify(429, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
}

TEST(Classify, Http429RateLimitRespectsRetryAfterHeader) {
    const auto body = make_error("rate_limit_error", "rate_limit_exceeded");
    RetryConfig cfg = instant_cfg();
    cfg.jitter_extra_s = 0.0f;
    const auto d = classify(429, body, 3, 0, cfg);
    EXPECT_EQ(d.outcome, RetryOutcome::Retry);
    EXPECT_GE(d.delay.count(), 3000LL);
}

// ── 429 quota/billing
// ─────────────────────────────────────────────────────────

TEST(Classify, Http429InsufficientQuotaIsNonRetryable) {
    const auto body = make_error("invalid_request_error", "insufficient_quota");
    EXPECT_EQ(classify(429, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopBillingOrQuota);
}

TEST(Classify, Http429QuotaMessageIsNonRetryable) {
    const auto body = make_error("", "", "You exceeded your current quota");
    EXPECT_EQ(classify(429, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopBillingOrQuota);
}

TEST(Classify, Http429BillingMessageIsNonRetryable) {
    const auto body = make_error("", "", "Check your plan and billing details");
    EXPECT_EQ(classify(429, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopBillingOrQuota);
}

TEST(Classify, Http429UnknownRetryOnceThenStop) {
    // First attempt → cautious single retry
    EXPECT_EQ(classify(429, "{}", -1, 0, instant_cfg()).outcome,
              RetryOutcome::Retry);
    // Second attempt → stop
    EXPECT_EQ(classify(429, "{}", -1, 1, instant_cfg()).outcome,
              RetryOutcome::StopBillingOrQuota);
}

// ── 4xx non-retryable
// ─────────────────────────────────────────────────────────

TEST(Classify, Http400IsNonRetryable) {
    EXPECT_EQ(classify(400, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopUserAction);
}

TEST(Classify, Http401IsNonRetryable) {
    EXPECT_EQ(classify(401, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopUserAction);
}

TEST(Classify, Http403IsNonRetryable) {
    EXPECT_EQ(classify(403, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopUserAction);
}

TEST(Classify, Http404IsNonRetryable) {
    EXPECT_EQ(classify(404, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopUserAction);
}

TEST(Classify, InvalidApiKeyCodeIsNonRetryable) {
    const auto body = make_error("authentication_error", "invalid_api_key");
    EXPECT_EQ(classify(401, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopUserAction);
}

TEST(Classify, ContextLengthExceededCodeIsNonRetryable) {
    const auto body =
        make_error("invalid_request_error", "context_length_exceeded");
    EXPECT_EQ(classify(400, body, -1, 0, instant_cfg()).outcome,
              RetryOutcome::StopUserAction);
}

// ── Retry-After on 503
// ────────────────────────────────────────────────────────

TEST(Classify, Http503WithRetryAfterUsesHeader) {
    RetryConfig cfg = instant_cfg();
    cfg.jitter_extra_s = 0.0f;
    const auto d = classify(503, "", 10, 0, cfg);
    EXPECT_EQ(d.outcome, RetryOutcome::Retry);
    EXPECT_GE(d.delay.count(), 10000LL);
}

// ── Ambiguous / unknown
// ───────────────────────────────────────────────────────

TEST(Classify, UnknownHttpCodeDoesNotRetry) {
    EXPECT_EQ(classify(418, "", -1, 0, instant_cfg()).outcome,
              RetryOutcome::DoNotRetry);
}

} // namespace
