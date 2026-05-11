#include "cli_options.hpp"
#include "cockpit_ui.hpp"
#include "getenv.hpp"

#include <filesystem>
#include <memory>
#include <print>

int main(int argc, char *argv[]) {
    auto parser = app::create_parser();
    auto result = parser.parse(argc, argv);

    switch (result.status) {
    case cli::ParseStatus::ShowHelp:
        std::print("{}", parser.generate_help(argv[0]));
        return 0;
    case cli::ParseStatus::ShowHelpVerbose:
        std::print("{}", parser.generate_help_verbose(argv[0]));
        return 0;
    case cli::ParseStatus::ShowVersion:
        std::print("CPP-LLM-CODER v0.1.0-alpha\n");
        return 0;
    case cli::ParseStatus::ShowCompletion:
        return 0;
    case cli::ParseStatus::Error:
        std::print(stderr, "Erro: {}\n", result.error_message);
        std::print(stderr, "Use --help para ver as opções disponíveis.\n");
        return 1;
    case cli::ParseStatus::Ok:
        break;
    }

    if (!result.config) {
        return 1;
    }

    auto &cfg = *result.config;
    if (cfg.workdir.empty()) {
        cfg.workdir =
            std::filesystem::absolute(std::filesystem::current_path()).string();
    }

    auto runtime = buildDefaultRuntime(cfg, ".cppllmcoder/agent.log", false);
    auto cockpit_consent =
        std::make_shared<CockpitConsentProvider>(cfg.auto_approve);
    runtime.consent = cockpit_consent;
    auto agent = std::make_shared<Agent>(cfg, runtime.tools, runtime.prompts,
                                         runtime.consent, runtime.logger,
                                         runtime.stats, runtime.done_signal);
    return runCockpitUi(CockpitAppDeps{
        .options = cfg,
        .runtime = std::move(runtime),
        .agent = std::move(agent),
        .consent = std::move(cockpit_consent),
    });
}
