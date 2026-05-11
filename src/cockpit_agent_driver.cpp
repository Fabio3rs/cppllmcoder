#include "cockpit_agent_driver.hpp"

CockpitAgentDriver::CockpitAgentDriver(std::shared_ptr<AgentEventBus> bus,
                                       std::string agent_id)
    : bus_(std::move(bus)), agent_id_(std::move(agent_id)) {}

void CockpitAgentDriver::on_token(std::string_view token) {
    bus_->post(EvToken{agent_id_, std::string(token)});
}

void CockpitAgentDriver::on_turn_complete(std::string_view response) {
    bus_->post(EvTurnComplete{agent_id_, std::string(response)});
}

void CockpitAgentDriver::on_tool_result(std::string_view tool_name,
                                        bool success,
                                        std::string_view summary) {
    bus_->post(EvToolCall{
        agent_id_, std::string(tool_name), {}, success, std::string(summary)});
}

void CockpitAgentDriver::on_retry(int attempt) {
    bus_->post(EvRetry{agent_id_, attempt});
}

bool CockpitAgentDriver::stop_requested() const {
    return stop_.load(std::memory_order_relaxed);
}

void CockpitAgentDriver::request_stop() {
    stop_.store(true, std::memory_order_relaxed);
}

std::optional<std::string> CockpitAgentDriver::next_injection() {
    std::lock_guard lock(inject_mutex_);
    if (inject_queue_.empty())
        return std::nullopt;
    auto msg = std::move(inject_queue_.front());
    inject_queue_.pop();
    return msg;
}

void CockpitAgentDriver::inject(std::string message) {
    std::lock_guard lock(inject_mutex_);
    inject_queue_.push(std::move(message));
}

std::optional<std::chrono::milliseconds> CockpitAgentDriver::timeout() const {
    return std::nullopt; // Interativo: sem timeout
}

bool CockpitAgentDriver::should_finish(int /*idle_turns*/) const {
    return false; // Interativo: deixa o operador decidir
}
