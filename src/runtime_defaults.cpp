#include "runtime_defaults.hpp"

#include "fs_tools.hpp"

RuntimeDefaults buildDefaultRuntime(const app::Options &opts,
                                    const std::string &log_path,
                                    bool echo_stdout, size_t max_read_bytes) {
    RuntimeDefaults r;
    auto [registry, done_signal] =
        buildDefaultToolRegistry(opts, max_read_bytes);
    r.tools = std::move(registry);
    r.done_signal = std::move(done_signal);
    r.prompts = std::make_shared<DefaultPromptManager>();
    r.consent = std::make_shared<PromptConsentProvider>(opts);
    r.logger = std::make_shared<FileExecutionLogger>(log_path, echo_stdout);
    r.stats = std::make_shared<CounterStatsRecorder>();
    return r;
}
