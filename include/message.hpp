#pragma once

#include <chrono>
#include <string>

enum class MessageRole {
    System,
    User,
    Assistant,
    Tool // Para o retorno do script Lua (Observation)
};

struct Message {
    size_t id; // ID sequencial ou vindo do DB
    MessageRole role;
    std::string content;
    std::string session_id;             // FK para sessions.id
    std::string created_at;             // ISO8601 com ms (persistido no DB)
    std::string updated_at;             // ISO8601 com ms (persistido no DB)
    std::chrono::milliseconds duration; // Tempo gasto para gerar a resposta
                                        // (útil para análise de performance)
    int token_count = 0;                // Cache para não recontar toda hora

    // Converte MessageRole para string que o Ollama/OpenAI entende
    std::string role_to_string() const {
        switch (role) {
        case MessageRole::System:
            return "system";
        case MessageRole::User:
            return "user";
        case MessageRole::Assistant:
            return "assistant";
        case MessageRole::Tool:
            return "tool";
        default:
            return "user";
        }
    }
};
