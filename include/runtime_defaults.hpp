#pragma once

#include "agent.hpp"
#include "brain_store.hpp"
#include "counter_stats.hpp"
#include "done_task_tool.hpp"
#include "file_execution_logger.hpp"
#include "prompt_consent.hpp"
#include "prompt_manager.hpp"
#include "tool_registry.hpp"

struct RuntimeDefaults {
    std::shared_ptr<ToolRegistry> tools;
    std::shared_ptr<DoneTaskSignal> done_signal;
    std::shared_ptr<IPromptManager> prompts;
    std::shared_ptr<IToolConsentProvider> consent;
    std::shared_ptr<IExecutionLogger> logger;
    std::shared_ptr<IStatsRecorder> stats;
    std::shared_ptr<BrainStore> brain_store; // keeps DB alive for db.* tools
};

// Build default runtime dependencies for the CLI PoC.
RuntimeDefaults
buildDefaultRuntime(const app::Options &opts,
                    const std::string &log_path = ".cppllmcoder/agent.log",
                    bool echo_stdout = true, size_t max_read_bytes = 8192);
