#pragma once

#include <string>
namespace app {

struct Options {
    bool verbose = false;
    bool version = false;
    bool auto_approve = false;
    int max_iterations = 100;
    std::string db_path = ".cppllmcoder/brain.db";
    std::string workdir = ".";
    std::string model = "qwen2.5-coder:7b";
    std::string model_version;
    std::string endpoint = "http://localhost:11434/v1/";
    double temperature = 0.7;
    double top_p = 0.95;
    int top_k = 0; // 0 deve desabilitar
    int max_tokens = 0;
    int seed = -1;                   // -1 indica aleatório
    std::string session_id_override; // Permite replays ou uso determinístico
    bool supports_tool_role = true;  // False se usar OpenAI porque eles exigem
                                     // protocolo especial JSON para tool_call
    bool list_sessions = false;      // Lista sessões persistidas e sai
    std::string restore_session_id;  // ID de sessão a restaurar
    bool restore_history = false;    // Recarrega histórico do DB se possível
};

} // namespace app
