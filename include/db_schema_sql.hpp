#pragma once

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <string_view>

namespace db::schema {

// Ativar FK em todas as conexões
inline constexpr std::string_view kPragmas = R"sql(
PRAGMA foreign_keys = ON;
)sql";

// Tabelas relacionais principais: tarefas, mensagens, ponteiros, ferramentas e
// logs
inline constexpr std::string_view kCoreTables = R"sql(
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    model TEXT,
    model_version TEXT,
    endpoint TEXT,
    temperature REAL,
    top_p REAL,
    top_k INTEGER,
    max_tokens INTEGER,
    seed INTEGER,
    params_json TEXT,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'))
);

CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY,
    parent_task_id TEXT,
    description TEXT NOT NULL,
    status TEXT DEFAULT 'pending' CHECK (status IN ('pending','running','completed','failed')),
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (parent_task_id) REFERENCES tasks(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('system','user','assistant','tool')),
    content TEXT NOT NULL,
    token_count INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS pointers (
    id TEXT PRIMARY KEY,
    task_id TEXT,
    file_path TEXT NOT NULL,
    offset_start INTEGER,
    offset_end INTEGER,
    micro_summary TEXT NOT NULL,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS knowledge_graph (
    source_pointer_id TEXT,
    target_pointer_id TEXT,
    relationship_type TEXT NOT NULL,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    PRIMARY KEY (source_pointer_id, target_pointer_id, relationship_type),
    FOREIGN KEY (source_pointer_id) REFERENCES pointers(id) ON DELETE CASCADE,
    FOREIGN KEY (target_pointer_id) REFERENCES pointers(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS tools (
    name TEXT PRIMARY KEY,
    description TEXT NOT NULL,
    args_schema TEXT NOT NULL, -- JSON Schema ou contrato simples
    is_sensitive INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'))
);

CREATE TABLE IF NOT EXISTS tool_invocations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    tool_name TEXT NOT NULL,
    json_args TEXT NOT NULL,
    decision TEXT DEFAULT 'allow' CHECK (decision IN ('allow','deny','modify_args','choose_alternative','abort')),
    decision_reason TEXT,
    consent_latency_ms INTEGER DEFAULT 0,
    status TEXT DEFAULT 'pending' CHECK (status IN ('pending','running','succeeded','failed')),
    result_summary TEXT,
    stderr_output TEXT,
    token_cost INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (tool_name) REFERENCES tools(name) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS execution_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    lua_script TEXT NOT NULL,
    stdout_output TEXT,
    stderr_hints TEXT,
    tokens_used INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);

-- Log de prompts para auditoria/validação
CREATE TABLE IF NOT EXISTS prompt_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    prompt_type TEXT NOT NULL CHECK (prompt_type IN ('system','assistant','tool_decision','subagent','other')),
    model TEXT,
    model_version TEXT,
    prompt_text TEXT NOT NULL,
    completion_text TEXT,
    token_estimate INTEGER DEFAULT 0,
    token_used INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);
)sql";

// Índices auxiliares para acelerar filtros comuns
inline constexpr std::string_view kIndexes = R"sql(
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id);
CREATE INDEX IF NOT EXISTS idx_messages_task_role ON messages(task_id, role);
CREATE INDEX IF NOT EXISTS idx_pointers_task ON pointers(task_id);
CREATE INDEX IF NOT EXISTS idx_tool_invocations_task ON tool_invocations(task_id);
CREATE INDEX IF NOT EXISTS idx_tool_invocations_session ON tool_invocations(session_id);
CREATE INDEX IF NOT EXISTS idx_tool_invocations_status ON tool_invocations(status);
CREATE INDEX IF NOT EXISTS idx_execution_logs_task ON execution_logs(task_id);
CREATE INDEX IF NOT EXISTS idx_execution_logs_session ON execution_logs(session_id);
CREATE INDEX IF NOT EXISTS idx_prompt_logs_session ON prompt_logs(session_id);
CREATE INDEX IF NOT EXISTS idx_prompt_logs_task_type ON prompt_logs(task_id, prompt_type);
)sql";

// Triggers para manter updated_at automático
inline constexpr std::string_view kTriggers = R"sql(
CREATE TRIGGER IF NOT EXISTS trg_sessions_updated
AFTER UPDATE ON sessions
BEGIN
    UPDATE sessions SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_tasks_updated
AFTER UPDATE ON tasks
BEGIN
    UPDATE tasks SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_messages_updated
AFTER UPDATE ON messages
BEGIN
    UPDATE messages SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_pointers_updated
AFTER UPDATE ON pointers
BEGIN
    UPDATE pointers SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_knowledge_graph_updated
AFTER UPDATE ON knowledge_graph
BEGIN
    UPDATE knowledge_graph
    SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')
    WHERE source_pointer_id = old.source_pointer_id AND target_pointer_id = old.target_pointer_id
          AND relationship_type = old.relationship_type;
END;

CREATE TRIGGER IF NOT EXISTS trg_tools_updated
AFTER UPDATE ON tools
BEGIN
    UPDATE tools SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE name = old.name;
END;

CREATE TRIGGER IF NOT EXISTS trg_tool_invocations_updated
AFTER UPDATE ON tool_invocations
BEGIN
    UPDATE tool_invocations SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_prompt_logs_updated
AFTER UPDATE ON prompt_logs
BEGIN
    UPDATE prompt_logs SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_execution_logs_updated
AFTER UPDATE ON execution_logs
BEGIN
    UPDATE execution_logs SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;
)sql";

// Esquema vetorial para busca semântica com similaridade de cosseno
inline constexpr std::string_view kVectorSchema = R"sql(
-- Extensão sqlite-vec (ou sqlite-vss). Armazena embeddings normalizados.
CREATE VIRTUAL TABLE IF NOT EXISTS pointer_embeddings USING vec0(
    embedding FLOAT[768], -- Tabela default (ex.: nomic-embed-text 768d)
    pointer_id TEXT
);

-- Armazena embeddings crus + metadados para múltiplos modelos/dimensões
CREATE TABLE IF NOT EXISTS embedding_records (
    pointer_id TEXT,
    model TEXT NOT NULL,
    model_version TEXT NOT NULL,
    embedding_dim INTEGER NOT NULL,
    normalized INTEGER DEFAULT 1,
    embedding_json TEXT NOT NULL, -- vetor serializado (JSON array)
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    PRIMARY KEY (pointer_id, model, model_version),
    FOREIGN KEY (pointer_id) REFERENCES pointers(id) ON DELETE CASCADE
);

-- Registry de tabelas vetoriais criadas por modelo/dimensão (para consultas rápidas)
CREATE TABLE IF NOT EXISTS embedding_indexes (
    table_name TEXT PRIMARY KEY, -- ex.: pointer_embeddings_768, pointer_embeddings_1536
    model TEXT NOT NULL,
    model_version TEXT NOT NULL,
    embedding_dim INTEGER NOT NULL,
    normalized INTEGER DEFAULT 1,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'))
);

-- View de conveniência para consultas de texto + vetor
CREATE VIEW IF NOT EXISTS pointer_search_view AS
SELECT p.id AS pointer_id,
       p.file_path,
       p.micro_summary,
       v.embedding
FROM pointers p
JOIN pointer_embeddings v ON v.pointer_id = p.id;
)sql";

// Consulta exemplo (cosine) para referência de implementação em C++/Lua:
inline constexpr std::string_view kVectorSearchExample = R"sql(
-- SELECT pointer_id, cosine_distance(embedding, :query_embedding) AS score
-- FROM pointer_embeddings
-- ORDER BY score ASC
-- LIMIT 10;
)sql";

// ------------------------------------------------------------
// Helpers em C++ para criar VTs de embedding por modelo/dimensão
// ------------------------------------------------------------

// Constrói um nome de tabela seguro: pointer_embeddings_<dim>_<model>_<version>
// substituindo caracteres não alfanuméricos por '_'.
inline std::string make_vector_table_name(std::string_view model,
                                          std::string_view model_version,
                                          int embedding_dim) {
    std::string safe_model(model);
    std::string safe_version(model_version);
    auto sanitize = [](std::string &s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
            return std::isalnum(ch) ? static_cast<char>(std::tolower(ch)) : '_';
        });
    };
    sanitize(safe_model);
    sanitize(safe_version);
    return std::format("pointer_embeddings_{}_{}_{}", embedding_dim, safe_model,
                       safe_version);
}

// Gera o SQL para criar uma VT vec0 com dimensão arbitrária.
inline std::string make_create_vector_table_sql(std::string_view table_name,
                                                int embedding_dim) {
    return std::format("CREATE VIRTUAL TABLE IF NOT EXISTS {} USING vec0(\n"
                       "    embedding FLOAT[{}],\n"
                       "    pointer_id TEXT\n"
                       ");",
                       table_name, embedding_dim);
}

// Gera INSERT para registrar a VT na tabela embedding_indexes.
inline std::string make_register_vector_index_sql(
    std::string_view table_name, std::string_view model,
    std::string_view model_version, int embedding_dim, bool normalized) {
    return std::format(
        "INSERT OR IGNORE INTO embedding_indexes(table_name, model, "
        "model_version, embedding_dim, normalized)\n"
        "VALUES('{}', '{}', '{}', {}, {});",
        table_name, model, model_version, embedding_dim, normalized ? 1 : 0);
}

// Script completo para inicialização: PRAGMA + tabelas + índices + triggers +
// vetores
inline constexpr std::string_view kFullSchema = R"sql(
PRAGMA foreign_keys = ON;

-- Core
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    model TEXT,
    model_version TEXT,
    endpoint TEXT,
    temperature REAL,
    top_p REAL,
    top_k INTEGER,
    max_tokens INTEGER,
    seed INTEGER,
    params_json TEXT,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'))
);

CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY,
    parent_task_id TEXT,
    description TEXT NOT NULL,
    status TEXT DEFAULT 'pending' CHECK (status IN ('pending','running','completed','failed')),
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (parent_task_id) REFERENCES tasks(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('system','user','assistant','tool')),
    content TEXT NOT NULL,
    token_count INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS pointers (
    id TEXT PRIMARY KEY,
    task_id TEXT,
    file_path TEXT NOT NULL,
    offset_start INTEGER,
    offset_end INTEGER,
    micro_summary TEXT NOT NULL,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL
);

CREATE TABLE IF NOT EXISTS knowledge_graph (
    source_pointer_id TEXT,
    target_pointer_id TEXT,
    relationship_type TEXT NOT NULL,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    PRIMARY KEY (source_pointer_id, target_pointer_id, relationship_type),
    FOREIGN KEY (source_pointer_id) REFERENCES pointers(id) ON DELETE CASCADE,
    FOREIGN KEY (target_pointer_id) REFERENCES pointers(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS tools (
    name TEXT PRIMARY KEY,
    description TEXT NOT NULL,
    args_schema TEXT NOT NULL,
    is_sensitive INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'))
);

CREATE TABLE IF NOT EXISTS tool_invocations (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    tool_name TEXT NOT NULL,
    json_args TEXT NOT NULL,
    decision TEXT DEFAULT 'allow' CHECK (decision IN ('allow','deny','modify_args','choose_alternative','abort')),
    decision_reason TEXT,
    consent_latency_ms INTEGER DEFAULT 0,
    status TEXT DEFAULT 'pending' CHECK (status IN ('pending','running','succeeded','failed')),
    result_summary TEXT,
    stderr_output TEXT,
    token_cost INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
    FOREIGN KEY (tool_name) REFERENCES tools(name) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS execution_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    lua_script TEXT NOT NULL,
    stdout_output TEXT,
    stderr_hints TEXT,
    tokens_used INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE CASCADE,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS prompt_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    task_id TEXT,
    session_id TEXT NOT NULL,
    prompt_type TEXT NOT NULL CHECK (prompt_type IN ('system','assistant','tool_decision','subagent','other')),
    model TEXT,
    model_version TEXT,
    prompt_text TEXT NOT NULL,
    completion_text TEXT,
    token_estimate INTEGER DEFAULT 0,
    token_used INTEGER DEFAULT 0,
    duration_ms INTEGER DEFAULT 0,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    updated_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    FOREIGN KEY (task_id) REFERENCES tasks(id) ON DELETE SET NULL,
    FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
);

-- Vetorial (sqlite-vec / sqlite-vss)
CREATE VIRTUAL TABLE IF NOT EXISTS pointer_embeddings USING vec0(
    embedding FLOAT[768],
    pointer_id TEXT
);

CREATE TABLE IF NOT EXISTS embedding_records (
    pointer_id TEXT,
    model TEXT NOT NULL,
    model_version TEXT NOT NULL,
    embedding_dim INTEGER NOT NULL,
    normalized INTEGER DEFAULT 1,
    embedding_json TEXT NOT NULL,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')),
    PRIMARY KEY (pointer_id, model, model_version),
    FOREIGN KEY (pointer_id) REFERENCES pointers(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS embedding_indexes (
    table_name TEXT PRIMARY KEY,
    model TEXT NOT NULL,
    model_version TEXT NOT NULL,
    embedding_dim INTEGER NOT NULL,
    normalized INTEGER DEFAULT 1,
    created_at DATETIME DEFAULT (STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW'))
);

CREATE VIEW IF NOT EXISTS pointer_search_view AS
SELECT p.id AS pointer_id,
       p.file_path,
       p.micro_summary,
       v.embedding
FROM pointers p
JOIN pointer_embeddings v ON v.pointer_id = p.id;

-- Índices
CREATE INDEX IF NOT EXISTS idx_tasks_status ON tasks(status);
CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id);
CREATE INDEX IF NOT EXISTS idx_messages_task_role ON messages(task_id, role);
CREATE INDEX IF NOT EXISTS idx_pointers_task ON pointers(task_id);
CREATE INDEX IF NOT EXISTS idx_tool_invocations_task ON tool_invocations(task_id);
CREATE INDEX IF NOT EXISTS idx_tool_invocations_session ON tool_invocations(session_id);
CREATE INDEX IF NOT EXISTS idx_tool_invocations_status ON tool_invocations(status);
CREATE INDEX IF NOT EXISTS idx_execution_logs_task ON execution_logs(task_id);
CREATE INDEX IF NOT EXISTS idx_execution_logs_session ON execution_logs(session_id);
CREATE INDEX IF NOT EXISTS idx_prompt_logs_session ON prompt_logs(session_id);
CREATE INDEX IF NOT EXISTS idx_prompt_logs_task_type ON prompt_logs(task_id, prompt_type);

-- Triggers
CREATE TRIGGER IF NOT EXISTS trg_tasks_updated
AFTER UPDATE ON tasks
BEGIN
    UPDATE tasks SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_sessions_updated
AFTER UPDATE ON sessions
BEGIN
    UPDATE sessions SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_messages_updated
AFTER UPDATE ON messages
BEGIN
    UPDATE messages SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_pointers_updated
AFTER UPDATE ON pointers
BEGIN
    UPDATE pointers SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_knowledge_graph_updated
AFTER UPDATE ON knowledge_graph
BEGIN
    UPDATE knowledge_graph
    SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW')
    WHERE source_pointer_id = old.source_pointer_id AND target_pointer_id = old.target_pointer_id
          AND relationship_type = old.relationship_type;
END;

CREATE TRIGGER IF NOT EXISTS trg_tools_updated
AFTER UPDATE ON tools
BEGIN
    UPDATE tools SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE name = old.name;
END;

CREATE TRIGGER IF NOT EXISTS trg_tool_invocations_updated
AFTER UPDATE ON tool_invocations
BEGIN
    UPDATE tool_invocations SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_prompt_logs_updated
AFTER UPDATE ON prompt_logs
BEGIN
    UPDATE prompt_logs SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;

CREATE TRIGGER IF NOT EXISTS trg_execution_logs_updated
AFTER UPDATE ON execution_logs
BEGIN
    UPDATE execution_logs SET updated_at = STRFTIME('%Y-%m-%d %H:%M:%f', 'NOW') WHERE id = old.id;
END;
)sql";

} // namespace db::schema
