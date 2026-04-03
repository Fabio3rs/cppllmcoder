#pragma once

#include "agent.hpp"

#include <memory>

#include "done_task_signal.hpp"

// Registers an always-available Lua tool: done_task()
// Returns the signal instance that will be set when called.
std::shared_ptr<DoneTaskSignal>
registerDoneTaskTool(ToolRegistry &registry,
                     std::shared_ptr<DoneTaskSignal> signal = nullptr);
