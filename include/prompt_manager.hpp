#pragma once

#include "prompts/prompt_manager.hpp"

class DefaultPromptManager : public IPromptManager {
  public:
    DefaultPromptManager() = default;

    std::string
    buildSystemPrompt([[maybe_unused]] const Agent &agent) const override;

    std::string
    buildToolDecisionPrompt(const ToolInvocationContext &ctx) const override {
        return "Default tool decision prompt" + ctx.metadata.name;
    }
};
