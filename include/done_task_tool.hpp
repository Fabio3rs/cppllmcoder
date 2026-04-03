#pragma once

#include "agent.hpp"

#include <atomic>
#include <memory>

// Per-agent signal that can be polled to know if done_task() was invoked.
class DoneTaskSignal {
  public:
    void mark_done() { flag_.store(true, std::memory_order_relaxed); }
    bool consume() {
        bool expected = true;
        return flag_.compare_exchange_strong(expected, false,
                                             std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> flag_{false};
};

// Registers an always-available Lua tool: done_task()
// Returns the signal instance that will be set when called.
std::shared_ptr<DoneTaskSignal>
registerDoneTaskTool(ToolRegistry &registry,
                     std::shared_ptr<DoneTaskSignal> signal = nullptr);
