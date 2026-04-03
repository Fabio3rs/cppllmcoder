#pragma once

#include "agent_types.hpp"
#include <string>
#include <string_view>

constexpr inline std::string_view TOOL_RESPONSE_TAG_OPEN = "<tool_response>";
constexpr inline std::string_view TOOL_RESPONSE_TAG_CLOSE = "</tool_response>";

class Agent;

class IPromptManager {
  public:
    virtual ~IPromptManager() = default;

    // Monta o system prompt com histórico + ferramentas
    virtual std::string buildSystemPrompt(const Agent &agent) const = 0;

    // Opcional: prompt específico para decisão de tool
    virtual std::string
    buildToolDecisionPrompt(const ToolInvocationContext &ctx) const = 0;
};
