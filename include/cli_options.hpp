#pragma once

#include "cli/option_parser_decls.hpp"
#include "cli/option_parser_impl.hpp"
#include <array>
#include <span>
#include <string>
#include <string_view>
#include "options.hpp"

namespace app {

// Valores permitidos para demonstração de restrição (exemplo: modelos
// homologados)
inline constexpr std::array<std::string_view, 3> ALLOWED_MODELS = {
    "qwen2.5-coder:7b", "qwen2.5-coder:14b", "codestral:latest"};

inline constexpr std::array<cli::OptionSpec<Options>, 8> OPTION_SPECS = {{
    {
        .long_name = "verbose",
        .short_name = 'v',
        .takes_value = false,
        .value_name = "",
        .help = "Habilita logs detalhados.",
        .long_help = "Exibe logs detalhados de execução da VM Lua, queries SQL e chamadas de API do LLM.",
        .allowed_values = {}, // Span vazio = qualquer valor
        .apply = [](Options &cfg, std::string_view) { cfg.verbose = true; },
        .required = false
    },
    {
        .long_name = "db",
        .short_name = 'd',
        .takes_value = true,
        .value_name = "<path>",
        .help = "Caminho do banco SQLite.",
        .long_help = "Define onde o 'cérebro' do agente (memória de longo prazo e vetores) será armazenado.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view val) { cfg.db_path = std::string(val); },
        .required = false
    },
    {
        .long_name = "workdir",
        .short_name = 'w',
        .takes_value = true,
        .value_name = "<dir>",
        .help = "Diretório de análise.",
        .long_help = "O diretório raiz onde o agente terá permissão para ler e listar arquivos via FS tools.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view val) { cfg.workdir = std::string(val); },
        .required = false
    },
    {
        .long_name = "model",
        .short_name = 'm',
        .takes_value = true,
        .value_name = "<name>",
        .help = "Nome do modelo LLM.",
        .long_help = "Especifica qual modelo do Ollama deve ser utilizado para geração de código Lua.",
        .allowed_values = ALLOWED_MODELS, // Restringe aos modelos na array acima
        .apply = [](Options &cfg, std::string_view val) { cfg.model = std::string(val); },
        .required = false
    },
    {
        .long_name = "endpoint",
        .short_name = 'e',
        .takes_value = true,
        .value_name = "<url>",
        .help = "URL da API do Ollama.",
        .long_help = "Endpoint HTTP compatível com OpenAI (default: local Ollama em 11434).",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view val) { cfg.endpoint = std::string(val); },
        .required = false
    },
    {
        .long_name = "max-steps",
        .short_name = 's',
        .takes_value = true,
        .value_name = "<n>",
        .help = "Limite de iterações.",
        .long_help = "Número máximo de ciclos de 'Pensamento-Ação-Observação' antes de interromper o agente.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view val) { cfg.max_iterations = std::stoi(std::string(val)); },
        .required = false
    },
    {
        .long_name = "yes",
        .short_name = 'y',
        .takes_value = false,
        .value_name = "",
        .help = "Auto-aprovar scripts Lua.",
        .long_help = "Executa blocos <code> gerados pelo LLM imediatamente sem pedir confirmação do usuário.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view) { cfg.auto_approve = true; },
        .required = false
    },
    {
        .long_name = "version",
        .short_name = 'V',
        .takes_value = false,
        .value_name = "",
        .help = "Mostra versão.",
        .long_help = "Exibe a versão atual do cppllmcoder e informações de build.",
        .allowed_values = {},
        .apply = [](Options &cfg, std::string_view) { cfg.version = true; },
        .required = false
    }
}};

inline cli::OptionParser<Options> create_parser() {
    return cli::OptionParser<Options>(OPTION_SPECS)
        .with_description(
            "CPP-LLM-CODER: Agente terminal para análise técnica e reversa.")
        .with_examples("  cppllmcoder -w ./binaries -m qwen2.5-coder:7b\n"
                       "  cppllmcoder --db ./brain.db --yes\n");
}

} // namespace app
