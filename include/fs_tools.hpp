#pragma once

#include "agent.hpp"
#include "options.hpp"
#include <memory>
#include <string>

class ToolRegistry;

// Registra o conjunto mínimo de ferramentas de filesystem.
void registerFilesystemTools(ToolRegistry &registry, const std::string &root,
                             size_t max_read_bytes = 8192);

// Cria um DefaultToolRegistry, registra as FS tools e retorna pronto para uso.
std::shared_ptr<ToolRegistry>
buildDefaultToolRegistry(const app::Options &opts,
                         size_t max_read_bytes = 8192);
