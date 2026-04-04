#pragma once

#include <cstddef>

struct sqlite3;
class ToolRegistry;

// Registra ferramentas de acesso somente-leitura ao brain.db.
// Atualmente expõe db.head() para ler um trecho de uma coluna TEXT.
void registerBrainDbTools(ToolRegistry &registry, sqlite3 *db,
                          size_t max_read_bytes = 8192);
