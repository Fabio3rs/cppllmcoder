#pragma once
#include <atomic>

// Per-agent signal that can be polled to know if done_task() was invoked.
class DoneTaskSignal {
  public:
    void mark_done() { flag_.store(true, std::memory_order_relaxed); }
    bool consume() {
        bool expected = true;
        return flag_.compare_exchange_strong(expected, false,
                                             std::memory_order_relaxed);
    }

    bool is_done() const { return flag_.load(std::memory_order_relaxed); }

    void reset() { flag_.store(false, std::memory_order_relaxed); }

  private:
    std::atomic<bool> flag_{false};
};
