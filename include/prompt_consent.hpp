#pragma once

#include "agent.hpp"
#include "options.hpp"

#include <iosfwd>

// Simple interactive consent provider for the PoC.
// If --yes is set, auto-approves all tool calls; otherwise asks on stdin.
class PromptConsentProvider : public IToolConsentProvider {
  public:
    PromptConsentProvider(const app::Options &opts, std::istream &in = std::cin,
                          std::ostream &out = std::cout);

    ToolDecision requestToolUse(const ToolInvocationContext &ctx) override;

  private:
    app::Options options_;
    std::istream &in_;
    std::ostream &out_;
};
