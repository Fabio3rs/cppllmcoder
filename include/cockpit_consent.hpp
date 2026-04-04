#pragma once

#include "agent_types.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

// TUI-aware consent provider: instead of blocking on stdin, it posts
// a pending approval to the cockpit state and waits for the UI thread
// to resolve it via approve/deny.
class CockpitConsentProvider : public IToolConsentProvider {
  public:
    // The callback is invoked (from the agent thread) when approval is needed.
    // It should post the request to the cockpit approval queue and return.
    using RequestCallback = std::function<void(const ToolInvocationContext &ctx,
                                               const std::string &approval_id)>;

    explicit CockpitConsentProvider(bool auto_approve = false)
        : auto_approve_(auto_approve) {}

    void set_auto_approve(bool v) {
        auto_approve_.store(v, std::memory_order_relaxed);
    }

    void set_request_callback(RequestCallback cb) {
        std::lock_guard lock(mutex_);
        on_request_ = std::move(cb);
    }

    // Called by the agent thread (via ToolRegistry wiring).
    // Blocks until the TUI thread calls resolve().
    ToolDecision requestToolUse(const ToolInvocationContext &ctx) override {
        if (auto_approve_.load(std::memory_order_relaxed)) {
            return ToolDecision{ToolDecisionKind::Allow, "auto-approved", {}};
        }

        const std::string approval_id =
            "consent-" + std::to_string(++counter_) + "-" + ctx.metadata.name;

        {
            std::lock_guard lock(mutex_);
            pending_id_ = approval_id;
            pending_decision_.reset();
            if (on_request_) {
                on_request_(ctx, approval_id);
            }
        }

        // Wait for TUI to resolve
        std::unique_lock lock(mutex_);
        cv_.wait_for(lock, std::chrono::seconds(120), [&] {
            return pending_decision_.has_value() || pending_id_.empty();
        });

        if (pending_decision_.has_value()) {
            auto d = *pending_decision_;
            pending_id_.clear();
            pending_decision_.reset();
            return d;
        }

        // Timeout or spurious wakeup → deny
        pending_id_.clear();
        return ToolDecision{ToolDecisionKind::Deny, "approval timeout", {}};
    }

    // Called by the TUI thread when the user approves or denies.
    void resolve(const std::string &approval_id, ToolDecision decision) {
        std::lock_guard lock(mutex_);
        if (pending_id_ == approval_id) {
            pending_decision_ = std::move(decision);
            cv_.notify_all();
        }
    }

    // Check if there's a pending consent request.
    std::optional<std::string> pending_approval_id() const {
        std::lock_guard lock(mutex_);
        if (pending_id_.empty())
            return std::nullopt;
        return pending_id_;
    }

  private:
    std::atomic<bool> auto_approve_{false};
    std::atomic<int> counter_{0};
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string pending_id_;
    std::optional<ToolDecision> pending_decision_;
    RequestCallback on_request_;
};
