#pragma once

#include "message.hpp"
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <sol/sol.hpp>
#include <string>
#include <vector>

struct ToolArgumentMeta {
    std::string name;
    std::string description;
    std::string type; // Ex.: "string", "int", "json"
    bool required = true;
};

struct ToolMetadata {
    std::string name;
    std::string description{};
    std::vector<ToolArgumentMeta> arguments{};
    std::string usage_example{}; // Exemplo curto de chamada
    std::string returns{};       // Descrição do retorno
    std::vector<std::string>
        tags{}; // Tags normalizadas para filtragem/pontuação de busca
    std::vector<std::string> danger_tags{}; // Palavras-chave de risco
    bool is_sensitive = false; // Indica se exige consentimento explícito
    bool always_show_in_prompt = false; // Força inclusão no prompt
};

struct ToolDocView {
    std::string name;
    std::string signature;
    std::string brief;
    bool sensitive = false;
    bool always = false;
};

struct SessionInfo {
    std::string id{};
    std::string model{};
    std::string model_version{};
    std::string endpoint{};
    double temperature = 0.0;
    double top_p = 1.0;
    int top_k = 0;
    int max_tokens = 0;
    int seed = 0;
    std::string params_json{};
};

struct ToolInvocationContext {
    ToolMetadata metadata;
    std::string json_args; // Args já serializados
    int estimated_token_cost = 0;
    std::chrono::milliseconds estimated_latency{0};
    SessionInfo session;
};

enum class ToolDecisionKind {
    Allow,
    Deny,
    ModifyArgs,
    ChooseAlternative,
    AbortConversation
};

struct ToolDecision {
    ToolDecisionKind action = ToolDecisionKind::Allow;
    std::string reason;             // Explica ao usuário/telemetria
    std::string modified_json_args; // Preenchido se action == ModifyArgs
};

class IToolConsentProvider {
  public:
    virtual ~IToolConsentProvider() = default;
    virtual ToolDecision requestToolUse(const ToolInvocationContext &ctx) = 0;
};

class ITool {
  public:
    virtual ~ITool() = default;
    virtual ToolMetadata describe() const = 0;

    virtual std::expected<sol::object, std::string>
    invoke(sol::variadic_args args, sol::this_state s) const = 0;
};

class ToolRegistry {
  public:
    virtual ~ToolRegistry() = default;
    virtual void registerTool(std::shared_ptr<ITool> tool) = 0;
    virtual std::shared_ptr<ITool> findTool(std::string_view name) const = 0;
    virtual std::vector<ToolMetadata> listMetadata() const = 0;
    virtual void
    forEach(const std::function<void(const ToolMetadata &, const ITool &)> &fn)
        const = 0;
    virtual std::vector<ToolDocView> topKDocs(std::string_view user_input,
                                              size_t k) const = 0;
};

class IExecutionLogger {
  public:
    virtual ~IExecutionLogger() = default;
    virtual void logMessage(const Message &msg, const SessionInfo &session) = 0;
    virtual void logToolEvent(const ToolInvocationContext &ctx,
                              const ToolDecision &decision,
                              std::chrono::milliseconds duration, bool success,
                              std::string_view result_summary,
                              const SessionInfo &session) = 0;
};

class IStatsRecorder {
  public:
    virtual ~IStatsRecorder() = default;
    virtual void incrementTokenCount(int delta) = 0;
    virtual size_t totalTokens() const = 0;
};
