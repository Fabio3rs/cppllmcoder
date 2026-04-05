#pragma once

#include "agent.hpp"
#include "done_task_tool.hpp"
#include "options.hpp"
#include <memory>
#include <string>
#include <utility>

class ToolRegistry;

// Registra o conjunto mínimo de ferramentas de filesystem.
void registerFilesystemTools(ToolRegistry &registry, const std::string &root,
                             size_t max_read_bytes = 32 * 1024);

// Cria um DefaultToolRegistry, registra as FS tools e done_task(), retorna
// registry + sinal associado.
std::pair<std::shared_ptr<ToolRegistry>, std::shared_ptr<DoneTaskSignal>>
buildDefaultToolRegistry(const app::Options &opts,
                         size_t max_read_bytes = 32 * 1024);
