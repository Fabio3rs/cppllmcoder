#pragma once

#include "prompts/prompt_manager.hpp"

class DefaultPromptManager : public IPromptManager {
  public:
    DefaultPromptManager() = default;

    std::string
    buildSystemPrompt([[maybe_unused]] const std::vector<Message> &history,
                      const std::vector<ToolDocView> &tools) const override {
        std::string_view head =
            R"(You are CPP-LLM-CODER. You operate via LUA scripts v5.4.
Whenever you need to interact with the system (files, databases, etc.), use the following format:

Reasoning: [your reasoning here]
<code>
  -- You Lua code here
  local result = fs.ls(".")
  return result[1]
</code>

)";
        std::string_view tools_available = "Tools available (Lua functions):\n";

        std::string prompt;
        prompt.reserve(head.size() + tools_available.size() +
                       tools.size() * 64);

        prompt = head;

        if (tools.size() > 0) {
            prompt += tools_available;
        }

        for (const auto &tool : tools) {
            prompt += tool.signature + " -- " + tool.brief + "\n";
        }

        return prompt;
    }

    std::string
    buildToolDecisionPrompt(const ToolInvocationContext &ctx) const override {
        return "Default tool decision prompt" + ctx.metadata.name;
    }
};
