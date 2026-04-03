#pragma once

#include "agent_types.hpp"
#include <string>
#include <vector>

class IPromptManager {
  public:
    virtual ~IPromptManager() = default;

    // Monta o system prompt com histórico + ferramentas
    virtual std::string
    buildSystemPrompt(const std::vector<Message> &history,
                      const std::vector<ToolDocView> &tools) const = 0;

    // Opcional: prompt específico para decisão de tool
    virtual std::string
    buildToolDecisionPrompt(const ToolInvocationContext &ctx) const = 0;
};
