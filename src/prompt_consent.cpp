#include "prompt_consent.hpp"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {
bool is_yes(const std::string &s) {
    return s == "y" || s == "Y" || s == "yes" || s == "YES" || s == "Yes";
}

std::string summarize_args(const ToolInvocationContext &ctx) {
    if (ctx.json_args.size() > 200) {
        return ctx.json_args.substr(0, 200) + "...";
    }
    return ctx.json_args;
}
} // namespace

PromptConsentProvider::PromptConsentProvider(const app::Options &opts,
                                             std::istream &in,
                                             std::ostream &out)
    : options_(opts), in_(in), out_(out) {}

ToolDecision
PromptConsentProvider::requestToolUse(const ToolInvocationContext &ctx) {
    ToolDecision decision{};

    if (options_.auto_approve) {
        return decision; // Allow
    }

    out_ << "\n[Consent] Tool: " << ctx.metadata.name << "\n";
    if (!ctx.metadata.description.empty()) {
        out_ << "  Desc: " << ctx.metadata.description << "\n";
    }
    out_ << "  Args: " << summarize_args(ctx) << "\n";
    out_ << "  Sensitive: " << (ctx.metadata.is_sensitive ? "yes" : "no")
         << "\n";
    out_ << "Executar? [y/N]: " << std::flush;

    std::string line;
    if (!std::getline(in_, line)) {
        decision.action = ToolDecisionKind::Deny;
        decision.reason = "stdin closed";
        return decision;
    }

    if (is_yes(line)) {
        decision.action = ToolDecisionKind::Allow;
        decision.reason = "approved by user";
    } else {
        decision.action = ToolDecisionKind::Deny;
        decision.reason = "denied by user";
    }

    out_ << "[Consent] " << decision.reason << "\n";
    return decision;
}
