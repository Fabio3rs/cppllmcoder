#pragma once

#include "agent_driver.hpp"
#include <atomic>
#include <nlohmann/json.hpp>
#include <openai/openai.hpp>
#include <string>

namespace llm {

struct Usage {
    int prompt_tokens = -1;
    int completion_tokens = -1;
    int total_tokens = -1;
};

class ChatStreamer {
  public:
    explicit ChatStreamer(openai::OpenAI &client)
        : client_{client}, control_(openai::StreamControl::Continue) {}

    // Streams chat completions and forwards tokens to the driver. Accumulates
    // the full response and emits on_turn_complete at the end (even on error).
    // Returns the full assistant message.
    std::string stream(nlohmann::json request, IAgentDriver &driver,
                       Usage *usage_out = nullptr);

    void request_stop() { control_.store(openai::StreamControl::Stop); }

  private:
    openai::OpenAI &client_;
    std::atomic<openai::StreamControl> control_;
};

} // namespace llm
