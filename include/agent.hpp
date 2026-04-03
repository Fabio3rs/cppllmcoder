#pragma once

#include "lua_context.hpp"
#include "message.hpp"
#include "options.hpp"
#include "sqlite3raii.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <openai/openai.hpp>
#include <string>
#include <string_view>
#include <vector>

class BrainStore;
class IAgentDriver;

// --- Metadados e autorização de ferramentas ---

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
    std::string usage_example{};            // Exemplo curto de chamada
    std::string returns{};                  // Descrição do retorno
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
    invoke(const sol::object &lua_args) const = 0;
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

class Agent {
  public:
    Agent(const app::Options &opts, std::shared_ptr<ToolRegistry> tools,
          std::shared_ptr<IPromptManager> prompts,
          std::shared_ptr<IToolConsentProvider> consent,
          std::shared_ptr<IExecutionLogger> logger,
          std::shared_ptr<IStatsRecorder> stats);
    ~Agent();

    // Executa um turno: envia para LLM -> extrai Lua -> executa -> retorna
    // observação
    std::string run_step(std::string_view input, IAgentDriver &driver,
                         openai::OpenAI &openai_client);

    // Permite testar unidades internas sem I/O real
    ToolDecision evaluate_tool_consent(const ToolInvocationContext &ctx);

    // Exposição de dependências (útil para wiring em main.cpp)
    std::shared_ptr<ToolRegistry> get_tool_registry() const;
    std::shared_ptr<IPromptManager> get_prompt_manager() const;
    std::shared_ptr<IToolConsentProvider> get_consent_provider() const;
    const SessionInfo &get_session_info() const;

  private:
    // --- Memória e Mensagens ---
    sqlite3_db_ptr brain_db;
    std::vector<Message> history; // Contexto atual em memória
    size_t total_tokens = 0;      // Acumulado da sessão

    MessageRole current_role = MessageRole::User;

    // --- Infraestrutura ---
    LuaContext luaContext;                       // Executor de scripts
    app::Options options;                        // Configs (model, db_path)
    std::shared_ptr<ToolRegistry> tool_registry; // Registro de tools
    std::shared_ptr<IPromptManager> prompt_manager;
    std::shared_ptr<IToolConsentProvider> consent_provider;
    std::shared_ptr<IExecutionLogger> execution_logger;
    std::shared_ptr<IStatsRecorder> stats_recorder;
    SessionInfo session_info;
    std::unique_ptr<BrainStore> brain_store;

    // --- Métodos de Apoio ---
    void persist_message(Message &msg); // Salva no SQLite, atualiza o ID
    void prune_context(); // Se o histórico ficar grande demais para o modelo
};
