#pragma once
// =============================================================================
// CLI Options - Definição das opções de linha de comando
// =============================================================================

#include <array>
#include <string>

// Forward include do option_parser (está em include/)
#include "cli/option_parser_decls.hpp"
#include "cli/option_parser_impl.hpp"

namespace app {

// =============================================================================
// Configuração parseada da linha de comando
// =============================================================================

struct Options {
    bool dev_mode = false;  // Forçar modo desenvolvimento
    bool prod_mode = false; // Forçar modo produção
    bool verbose = false;   // Log verbose
    bool version = false;   // Mostrar versão
    int width = 0;          // Largura da janela (0 = usar padrão)
    int height = 0;         // Altura da janela (0 = usar padrão)
    std::string url;        // URL customizada para navegação
};

// =============================================================================
// Especificações das opções
// =============================================================================

inline constexpr std::array<cli::OptionSpec<Options>, 7> OPTION_SPECS = {{
    {
        .long_name = "dev",
        .short_name = 'd',
        .takes_value = false,
        .value_name = "",
        .help = "Force development mode (use Vite dev server)",
        .long_help = "Forces the application to run in development mode,\n"
                     "connecting to the Vite dev server for hot reload.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view) { cfg.dev_mode = true; },
        .required = false,
    },
    {
        .long_name = "prod",
        .short_name = 'p',
        .takes_value = false,
        .value_name = "",
        .help = "Force production mode (use embedded HTML)",
        .long_help = "Forces the application to run in production mode,\n"
                     "using the embedded HTML instead of dev server.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view) { cfg.prod_mode = true; },
        .required = false,
    },
    {
        .long_name = "verbose",
        .short_name = 'v',
        .takes_value = false,
        .value_name = "",
        .help = "Enable verbose logging",
        .long_help = "Enables detailed logging output for debugging.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view) { cfg.verbose = true; },
        .required = false,
    },
    {
        .long_name = "version",
        .short_name = 'V',
        .takes_value = false,
        .value_name = "",
        .help = "Show version information",
        .long_help = "Displays the application version and exits.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view) { cfg.version = true; },
        .required = false,
    },
    {
        .long_name = "width",
        .short_name = 'W',
        .takes_value = true,
        .value_name = "<pixels>",
        .help = "Set window width",
        .long_help = "Sets the initial window width in pixels.",
        .allowed_values = {},
        .apply =
            [](Options &cfg, std::string_view val) {
                cfg.width = std::stoi(std::string(val));
            },
        .required = false,
    },
    {
        .long_name = "height",
        .short_name = 'H',
        .takes_value = true,
        .value_name = "<pixels>",
        .help = "Set window height",
        .long_help = "Sets the initial window height in pixels.",
        .allowed_values = {},
        .apply =
            [](Options &cfg, std::string_view val) {
                cfg.height = std::stoi(std::string(val));
            },
        .required = false,
    },
    {
        .long_name = "url",
        .short_name = 'u',
        .takes_value = true,
        .value_name = "<url>",
        .help = "Navigate to custom URL",
        .long_help = "Navigate to a custom URL instead of the default.\n"
                     "Useful for development with external servers.",
        .allowed_values = {},
        .apply = [](Options &cfg,
                    std::string_view val) { cfg.url = std::string(val); },
        .required = false,
    },
}};

// =============================================================================
// Criar parser configurado
// =============================================================================

inline cli::OptionParser<Options> create_parser() {
    return cli::OptionParser<Options>(OPTION_SPECS)
        .with_description(
            "WebView-based desktop application with Vite frontend.")
        .with_examples(
            "  app                    # Run with auto-detected mode\n"
            "  app --dev              # Force development mode\n"
            "  app --prod             # Force production mode\n"
            "  app --url http://localhost:3000  # Use custom URL\n"
            "  app -W 1920 -H 1080    # Custom window size\n");
}

} // namespace app
