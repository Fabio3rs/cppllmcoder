#include "prompt_manager.hpp"
#include "agent.hpp"
#include <format>

std::string DefaultPromptManager::buildSystemPrompt(
    [[maybe_unused]] const Agent &agent) const {
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

    auto chosen_tools = agent.get_tool_registry()->topKDocs("", 16);

    std::string prompt;
    prompt.reserve(head.size() + tools_available.size() +
                   chosen_tools.size() * 64);

    prompt = head;

    if (chosen_tools.size() > 0) {
        prompt += tools_available;
    }

    for (const auto &tool : chosen_tools) {
        prompt += tool.signature + " -- " + tool.brief + "\n";
    }

    if (!agent.get_options().supports_tool_role) {
        prompt +=
            std::format("\n{}\n{}\n{}", TOOL_RESPONSE_TAG_OPEN,
                        "The automated tool response will be inside this tags.",
                        TOOL_RESPONSE_TAG_CLOSE);
    }

    return prompt;
}
