#pragma once

#include "agent_driver.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>

// Driver especializado para o cockpit: envia eventos para o AgentEventBus
// e permite injeção de mensagens/stop de forma thread-safe.
class CockpitAgentDriver final : public IAgentDriver {
  public:
    explicit CockpitAgentDriver(std::shared_ptr<AgentEventBus> bus,
                                std::string agent_id = "main");

    void on_token(std::string_view token) override;
    void on_turn_complete(std::string_view response) override;
    void on_tool_result(std::string_view tool_name, bool success,
                        std::string_view summary) override;

    bool stop_requested() const override;
    void request_stop() override;

    std::optional<std::string> next_injection() override;
    void inject(std::string message) override;

    std::optional<std::chrono::milliseconds> timeout() const override;
    bool should_finish(int idle_turns) const override;

  private:
    std::shared_ptr<AgentEventBus> bus_;
    std::string agent_id_;
    std::atomic<bool> stop_{false};
    std::mutex inject_mutex_;
    std::queue<std::string> inject_queue_;
};
