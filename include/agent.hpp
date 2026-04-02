#pragma once

#include "lua_context.hpp"
#include "message.hpp"
#include "options.hpp"
#include "sqlite3raii.hpp"

class Agent {
  public:
    explicit Agent(const app::Options &opts);

    // Executa um turno: envia para LLM -> extrai Lua -> executa -> retorna
    // observação
    void run_step(std::string_view input);

  private:
    // --- Memória e Mensagens ---
    sqlite3_db_ptr brain_db;
    std::vector<Message> history; // Contexto atual em memória
    size_t total_tokens = 0;      // Acumulado da sessão

    MessageRole current_role = MessageRole::User;

    // --- Infraestrutura ---
    LuaContext luaContext; // Seu executor de scripts
    app::Options options;  // Configs (model, db_path, etc)

    // --- Métodos de Apoio ---
    void persist_message(const Message &msg); // Salva no SQLite
    void prune_context(); // Se o histórico ficar grande demais para o modelo
};
