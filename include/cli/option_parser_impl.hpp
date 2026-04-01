// option_parser_impl.hpp - Template implementations for option_parser.hpp

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <ranges>

#include "option_parser_decls.hpp"
#include <expected>

namespace cli {

// ============================================================================
// OptionParser implementation
// ============================================================================

template <typename Config>
ParseResult<Config> OptionParser<Config>::parse(int argc, char *argv[]) const {
    std::vector<std::string_view> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return parse(std::span{args});
}

template <typename Config>
ParseResult<Config>
OptionParser<Config>::parse(std::span<const std::string_view> args) const {
    return parse_impl(args);
}

template <typename Config>
const OptionSpec<Config> *
OptionParser<Config>::find_option(std::string_view long_name) const {
    for (const auto &spec : specs_) {
        if (spec.long_name == long_name) {
            return &spec;
        }
    }
    return nullptr;
}

template <typename Config>
const OptionSpec<Config> *
OptionParser<Config>::find_option(char short_name) const {
    if (short_name == '\0') {
        return nullptr;
    }
    for (const auto &spec : specs_) {
        if (spec.short_name == short_name) {
            return &spec;
        }
    }
    return nullptr;
}

template <typename Config>
const OptionSpec<Config> *
OptionParser<Config>::find_option_token(std::string_view token) const {
    using namespace std::literals;

    if (token.size() >= 3 && token.starts_with("--"sv)) {
        return find_option(token.substr(2));
    }
    if (token.size() == 2 && token[0] == '-') {
        return find_option(token[1]);
    }
    return nullptr;
}

template <typename Config>
std::expected<void, std::string>
OptionParser<Config>::validate_value(const OptionSpec<Config> *spec,
                                     std::string_view value) const {
    if (spec->allowed_values.empty()) {
        return {}; // Any value is allowed
    }

    bool found =
        std::ranges::any_of(spec->allowed_values,
                            [value](std::string_view v) { return v == value; });

    if (!found) {
        std::string allowed;
        for (auto v : spec->allowed_values) {
            if (!allowed.empty()) {
                allowed += ", ";
            }
            allowed += v;
        }
        return make_unexpected(
            std::format("Invalid value '{}' for '--{}'. Allowed: {}", value,
                        spec->long_name, allowed));
    }

    return {};
}

template <typename Config>
ParseResult<Config>
OptionParser<Config>::parse_impl(std::span<const std::string_view> args) const {

    using namespace std::literals;

    ParseResult<Config> result;
    Config config{};

    // Detecta contexto de bash completion (COMP_LINE está definido)
    if (std::getenv("COMP_LINE") != nullptr) {
        CompletionHandler::handle_completion(*this);
        result.status = ParseStatus::ShowCompletion;
        return result;
    }

    // Sem argumentos = usa config padrão (não mostra help automaticamente)
    if (args.size() <= 1) {
        result.config = std::move(config);
        result.status = ParseStatus::Ok;
        return result;
    }

    for (size_t i = 1; i < args.size(); ++i) {
        std::string_view arg = args[i];

        // Handle --help / -h
        if (arg == "--help"sv || arg == "-h"sv) {
            result.status = ParseStatus::ShowHelp;
            return result;
        }

        // Handle --help-verbose
        if (arg == "--help-verbose"sv) {
            result.status = ParseStatus::ShowHelpVerbose;
            return result;
        }

        // Handle --version / -V
        if (arg == "--version"sv || arg == "-V"sv) {
            result.status = ParseStatus::ShowVersion;
            return result;
        }

        // Handle --
        if (arg == "--"sv) {
            break; // Stop processing options
        }

        // Handle long options (--option)
        if (arg.starts_with("--"sv)) {
            auto name = arg.substr(2);
            auto *spec = find_option(name);

            if (!spec) {
                result.status = ParseStatus::Error;
                result.error_message =
                    std::format("Unknown option '--{}'", name);
                return result;
            }

            std::string_view value;
            if (spec->takes_value) {
                if (i + 1 >= args.size()) {
                    result.status = ParseStatus::Error;
                    result.error_message =
                        std::format("Option '--{}' requires a value", name);
                    return result;
                }
                value = args[++i];

                if (auto err = validate_value(spec, value); !err) {
                    result.status = ParseStatus::Error;
                    result.error_message = err.error();
                    return result;
                }
            }

            try {
                spec->apply(config, value);
            } catch (const std::exception &ex) {
                result.status = ParseStatus::Error;
                result.error_message = ex.what();
                return result;
            }
        }
        // Handle short options (-o)
        else if (arg.starts_with('-') && arg.size() > 1) {
            for (size_t pos = 1; pos < arg.size(); ++pos) {
                char short_opt = arg[pos];
                auto *spec = find_option(short_opt);

                if (!spec) {
                    result.status = ParseStatus::Error;
                    result.error_message =
                        std::format("Unknown option '-{}'", short_opt);
                    return result;
                }

                std::string_view value;
                if (spec->takes_value) {
                    // Value can be attached (-ovalue) or separate (-o value)
                    if (pos + 1 < arg.size()) {
                        value = arg.substr(pos + 1);
                        pos = arg.size(); // Consume rest of arg
                    } else {
                        if (i + 1 >= args.size()) {
                            result.status = ParseStatus::Error;
                            result.error_message = std::format(
                                "Option '-{}' requires a value", short_opt);
                            return result;
                        }
                        value = args[++i];
                    }

                    if (auto err = validate_value(spec, value); !err) {
                        result.status = ParseStatus::Error;
                        result.error_message = err.error();
                        return result;
                    }
                }

                try {
                    spec->apply(config, value);
                } catch (const std::exception &ex) {
                    result.status = ParseStatus::Error;
                    result.error_message = ex.what();
                    return result;
                }
            }
        } else {
            // Positional argument (not supported in current design)
            result.status = ParseStatus::Error;
            result.error_message =
                std::format("Unexpected positional argument '{}'", arg);
            return result;
        }
    }

    result.config = std::move(config);
    result.status = ParseStatus::Ok;
    return result;
}

template <typename Config>
std::string
OptionParser<Config>::generate_help(std::string_view program_name) const {
    std::string help;
    help.reserve(2048);

    help += std::format("Usage: {} [OPTIONS]\n\n", program_name);
    help += "Options:\n";

    for (const auto &opt : specs_) {
        format_option_help(help, opt);
    }

    help += "\n  -h, --help              Show this help message\n";
    help += "      --help-verbose      Show detailed help with examples\n";

    // Add database sources section if provided
    if (!database_sources_.empty()) {
        help += "\nDATABASE SOURCES:\n";
        help += database_sources_;
        if (!database_sources_.ends_with('\n')) {
            help += '\n';
        }
    }

    // Add examples section if provided
    if (!examples_.empty()) {
        help += "\nEXAMPLES:\n";
        help += examples_;
        if (!examples_.ends_with('\n')) {
            help += '\n';
        }
    }

    return help;
}

template <typename Config>
void OptionParser<Config>::format_option_help(
    std::string &out, const OptionSpec<Config> &opt) const {
    out += "  ";

    // Short option
    if (opt.short_name != '\0') {
        out += '-';
        out += opt.short_name;
        if (!opt.long_name.empty()) {
            out += ", ";
        }
    } else {
        out += "    ";
    }

    // Long option
    if (!opt.long_name.empty()) {
        out += "--";
        out += opt.long_name;
    }

    // Value placeholder
    if (opt.takes_value && !opt.value_name.empty()) {
        out += ' ';
        out += opt.value_name;
    }

    // Padding to align descriptions (assuming max ~30 chars for option part)
    size_t current_len = out.size() - out.rfind('\n') - 1;
    if (current_len < 30) {
        out.append(30 - current_len, ' ');
    } else {
        out += "  ";
    }

    // Help text
    out += opt.help;

    // Show allowed values
    if (!opt.allowed_values.empty()) {
        out += " (";
        bool first = true;
        for (auto v : opt.allowed_values) {
            if (!first) {
                out += ", ";
            }
            out += v;
            first = false;
        }
        out += ')';
    }

    if (opt.required) {
        out += " [required]";
    }

    out += '\n';
}

template <typename Config>
std::string OptionParser<Config>::generate_help_verbose(
    std::string_view program_name) const {
    std::string help;
    help.reserve(4096);

    // NAME section
    help += "NAME\n";
    help += std::format("    {} - ", program_name);
    if (!description_.empty()) {
        help += description_;
    } else {
        help += "Command-line tool";
    }
    help += "\n\n";

    // SYNOPSIS section
    help += "SYNOPSIS\n";
    help += std::format("    {} [OPTIONS]\n\n", program_name);

    // DESCRIPTION section (if provided)
    if (!description_.empty()) {
        help += "DESCRIPTION\n";
        // Indent description text
        for (char c : description_) {
            if (help.back() == '\n') {
                help += "    ";
            }
            help += c;
        }
        if (!description_.ends_with('\n')) {
            help += '\n';
        }
        help += '\n';
    }

    // OPTIONS section (detailed)
    help += "OPTIONS\n";
    for (const auto &opt : specs_) {
        format_option_help_verbose(help, opt);
    }

    // Add help options
    help += "    -h, --help\n";
    help += "        Show concise help message with examples.\n\n";
    help += "    --help-verbose\n";
    help += "        Show this detailed help message (man-page style).\n\n";

    // DATABASE SOURCES section
    if (!database_sources_.empty()) {
        help += "DATABASE SOURCES\n";
        // Indent database sources text
        for (char c : database_sources_) {
            if (help.back() == '\n') {
                help += "    ";
            }
            help += c;
        }
        if (!database_sources_.ends_with('\n')) {
            help += '\n';
        }
        help += '\n';
    }

    // EXAMPLES section
    if (!examples_.empty()) {
        help += "EXAMPLES\n";
        // Indent examples text
        for (char c : examples_) {
            if (help.back() == '\n') {
                help += "    ";
            }
            help += c;
        }
        if (!examples_.ends_with('\n')) {
            help += '\n';
        }
    }

    return help;
}

template <typename Config>
void OptionParser<Config>::format_option_help_verbose(
    std::string &out, const OptionSpec<Config> &opt) const {
    // Option signature line
    out += "    ";

    if (opt.short_name != '\0') {
        out += '-';
        out += opt.short_name;
        if (!opt.long_name.empty()) {
            out += ", ";
        }
    }

    if (!opt.long_name.empty()) {
        out += "--";
        out += opt.long_name;
    }

    if (opt.takes_value && !opt.value_name.empty()) {
        out += ' ';
        out += opt.value_name;
    }

    out += '\n';

    // Detailed description (indented)
    std::string_view desc = opt.long_help.empty() ? opt.help : opt.long_help;

    out += "        ";
    for (char c : desc) {
        out += c;
        if (c == '\n') {
            out += "        "; // Indent continuation lines
        }
    }

    if (!desc.ends_with('\n')) {
        out += '\n';
    }

    // Show allowed values on separate line if present
    if (!opt.allowed_values.empty()) {
        out += "        \n";
        out += "        Allowed values: ";
        bool first = true;
        for (auto v : opt.allowed_values) {
            if (!first) {
                out += ", ";
            }
            out += v;
            first = false;
        }
        out += '\n';
    }

    // Show required marker
    if (opt.required) {
        out += "        \n";
        out += "        This option is required.\n";
    }

    out += '\n';
}

// ============================================================================
// CompletionHandler implementation
// ============================================================================

inline std::vector<std::string_view>
CompletionHandler::split_words(const std::string &line) {
    std::vector<std::string_view> words;
    size_t i = 0;

    while (i < line.size()) {
        // Skip whitespace
        while (i < line.size() &&
               std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }

        size_t start = i;

        // Collect non-whitespace
        while (i < line.size() &&
               !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }

        if (start < i) {
            words.emplace_back(&line[start], i - start);
        }
    }

    return words;
}

template <typename Config>
void CompletionHandler::suggest_options(const OptionParser<Config> &parser,
                                        std::string_view prefix) {
    for (const auto &spec : parser.specs()) {
        // Check long option
        if (!spec.long_name.empty()) {
            std::string option =
                std::string("--") + std::string(spec.long_name);
            if (prefix.empty() || option.starts_with(prefix)) {
                std::cout << option << '\n';
            }
        }
        // Check short option
        if (spec.short_name != '\0') {
            std::string option = std::string("-") + spec.short_name;
            if (prefix.empty() || option.starts_with(prefix)) {
                std::cout << option << '\n';
            }
        }
    }
}

template <typename Config>
int CompletionHandler::handle_completion(const OptionParser<Config> &parser) {
    const char *line_env = std::getenv("COMP_LINE");
    const char *point_env = std::getenv("COMP_POINT");

    if (!line_env || !point_env) {
        return 0; // Not in completion context
    }

    std::string line(line_env);
    int point = std::atoi(point_env);

    if (point < 0 || point > static_cast<int>(line.size())) {
        point = static_cast<int>(line.size());
    }

    line.resize(static_cast<size_t>(point));

    bool ends_with_space =
        !line.empty() && std::isspace(static_cast<unsigned char>(line.back()));

    auto words = split_words(line);
    if (words.empty()) {
        return 0;
    }

    using namespace std::literals;

    std::string_view cur;
    std::string_view prev;

    if (ends_with_space) {
        if (!words.empty()) {
            prev = words.back();
        }
        cur = {};
    } else {
        cur = words.back();
        if (words.size() >= 2) {
            prev = words[words.size() - 2];
        }
    }

    // Suggest values for previous option if it takes values
    if (!prev.empty()) {
        if (auto *spec = parser.find_option_token(prev)) {
            if (spec->takes_value && !spec->allowed_values.empty()) {
                suggest_values(spec->allowed_values, cur);
                return 0;
            }
        }
    }

    // Suggest options if current word starts with '-' or is empty
    if (cur.starts_with('-') || cur.empty()) {
        suggest_options(parser, cur);
        return 0;
    }

    return 0;
}

inline void
CompletionHandler::suggest_values(std::span<const std::string_view> values,
                                  std::string_view prefix) {
    for (auto value : values) {
        if (prefix.empty() || value.starts_with(prefix)) {
            std::printf("%.*s\n", static_cast<int>(value.size()), value.data());
        }
    }
}

} // namespace cli
