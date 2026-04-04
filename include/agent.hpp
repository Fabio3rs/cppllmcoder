#pragma once

#include "agent_types.hpp"
#include "lua_context.hpp"
#include "message.hpp"
#include "options.hpp"
#include "prompts/prompt_manager.hpp"
#include "sqlite3raii.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <openai/openai.hpp>
#include <string>
#include <string_view>
#include <vector>

class BrainStore;
class IAgentDriver;
class DoneTaskSignal;

class Agent {
  public:
    Agent(const app::Options &opts, std::shared_ptr<ToolRegistry> tools,
          std::shared_ptr<IPromptManager> prompts,
          std::shared_ptr<IToolConsentProvider> consent,
          std::shared_ptr<IExecutionLogger> logger,
          std::shared_ptr<IStatsRecorder> stats,
          std::shared_ptr<DoneTaskSignal> done = {});
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
    const std::vector<Message> &get_history() const { return history; }
    const std::vector<std::string> &get_child_agents() const {
        return child_agents;
    }
    void register_child_agent(std::string id) {
        child_agents.push_back(std::move(id));
    }

    const app::Options &get_options() const noexcept { return options; }

    app::Options &get_options() noexcept { return options; }

    LuaContext &get_lua_context() noexcept { return luaContext; }

    nlohmann::json get_messages_cache() const { return messages_cache; }

    std::string prune_message(const Message &msg_ref) const;

  private:
    // --- Memória e Mensagens ---
    sqlite3_db_ptr brain_db;
    std::vector<Message> history;  // Contexto atual em memória
    nlohmann::json messages_cache; // Cache do array JSON para a LLM
    size_t total_tokens = 0;       // Acumulado da sessão

    MessageRole current_role = MessageRole::User;

    // --- Infraestrutura ---
    LuaContext luaContext;                       // Executor de scripts
    app::Options options;                        // Configs (model, db_path)
    std::shared_ptr<ToolRegistry> tool_registry; // Registro de tools
    std::shared_ptr<IPromptManager> prompt_manager;
    std::shared_ptr<IToolConsentProvider> consent_provider;
    std::shared_ptr<IExecutionLogger> execution_logger;
    std::shared_ptr<IStatsRecorder> stats_recorder;
    std::shared_ptr<DoneTaskSignal> done_task_signal;
    SessionInfo session_info;
    std::unique_ptr<BrainStore> brain_store;
    std::vector<std::string> child_agents;

    // --- Métodos de Apoio ---
    void add_to_history(Message msg);   // Adiciona ao history e ao cache JSON
    void persist_message(Message &msg); // Salva no SQLite, atualiza o ID
    void prune_context(); // Se o histórico ficar grande demais para o modelo
    void append_to_cache(const Message &msg);
};
