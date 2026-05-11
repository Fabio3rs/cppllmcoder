#pragma once

#include "brain_store.hpp"
#include "cockpit_state.hpp"
#include "runtime_defaults.hpp"
#include "tool_registry.hpp"

#include <memory>
#include <string>

std::vector<std::shared_ptr<TaskNode>>
loadTaskTreeFromDb(const std::string &db_path);
std::vector<PointerItem> loadPointersFromDb(const std::string &db_path);
std::vector<DiffHunk> loadDiffFromGit();
std::vector<LogLine> loadLogsFromDb(const std::string &db_path,
                                    const std::string &session_id,
                                    int limit = 200);
std::vector<ToolItem> buildToolItemsFromRegistry(const ToolRegistry &registry);
