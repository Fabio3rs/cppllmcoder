#include "runtime_defaults.hpp"

#include "db_tools.hpp"
#include "fs_tools.hpp"

RuntimeDefaults buildDefaultRuntime(const app::Options &opts,
                                    const std::string &log_path,
                                    bool echo_stdout, size_t max_read_bytes) {
    RuntimeDefaults r;
    auto [registry, done_signal] =
        buildDefaultToolRegistry(opts, max_read_bytes);

    // Expose DB tools (read-only) to Lua so LLM can page large messages.
    if (registry) {
        r.brain_store = std::make_shared<BrainStore>(
            BrainStore::open(opts.db_path,
                             /*enable_vector=*/false));
        registerBrainDbTools(*registry, r.brain_store->raw_db(),
                             max_read_bytes);
    }
    r.tools = std::move(registry);
    r.done_signal = std::move(done_signal);
    r.prompts = std::make_shared<DefaultPromptManager>();
    r.consent = std::make_shared<PromptConsentProvider>(opts);
    r.logger = std::make_shared<FileExecutionLogger>(log_path, echo_stdout);
    r.stats = std::make_shared<CounterStatsRecorder>();
    return r;
}
