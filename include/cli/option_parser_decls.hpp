// option_parser.hpp - Generic command-line option parsing framework
// Provides a declarative, table-driven approach to CLI argument parsing
// with automatic help generation and bash completion support.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <expected>

namespace cli {

/// Status of parsing operation
enum class ParseStatus {
    Ok,              ///< Parsing succeeded
    ShowHelp,        ///< --help was requested
    ShowHelpVerbose, ///< --help-verbose was requested
    ShowVersion,     ///< --version was requested
    ShowCompletion,  ///< Bash completion was requested (COMP_LINE set)
    Error            ///< Parsing failed
};

/// Result of parsing operation
template <typename Config> struct ParseResult {
    std::optional<Config> config;
    ParseStatus status = ParseStatus::Error;
    std::string error_message;
    std::string output; ///< Output to print (help text, completions, etc.)
};

/// Specification for a single command-line option
template <typename Config> struct OptionSpec {
    std::string_view long_name; ///< Long option name (without --)
    char short_name;            ///< Short option character (or '\0')
    bool takes_value;           ///< Whether option expects a value
    std::string_view
        value_name;             ///< Value placeholder for help (e.g., "<file>")
    std::string_view help;      ///< Short help text for concise mode
    std::string_view long_help; ///< Detailed help for verbose mode (optional)

    /// Allowed values (empty span = any value accepted)
    std::span<const std::string_view> allowed_values;

    /// Function to apply this option to config
    void (*apply)(Config &, std::string_view);

    /// Whether this option is required
    bool required = false;
};

/// Helper to create std::array from variadic arguments
template <typename... T> constexpr auto make_array(T &&...t) {
    using Common = std::common_type_t<std::remove_cvref_t<T>...>;
    return std::array<Common, sizeof...(T)>{std::forward<T>(t)...};
}

/// Generic option parser
template <typename Config> class OptionParser {
  public:
    /// Construct parser with option specifications
    explicit OptionParser(std::span<const OptionSpec<Config>> specs)
        : specs_(specs) {}

    /// Set program description for help text
    OptionParser &with_description(std::string_view desc) {
        description_ = desc;
        return *this;
    }

    /// Set examples section for help text
    OptionParser &with_examples(std::string_view examples) {
        examples_ = examples;
        return *this;
    }

    /// Set database sources documentation
    OptionParser &with_database_sources(std::string_view db_sources) {
        database_sources_ = db_sources;
        return *this;
    }

    /// Parse command-line arguments
    [[nodiscard]] ParseResult<Config> parse(int argc, char *argv[]) const;

    /// Parse from string_view span (useful for testing)
    [[nodiscard]] ParseResult<Config>
    parse(std::span<const std::string_view> args) const;

    /// Generate concise help text with examples
    [[nodiscard]] std::string
    generate_help(std::string_view program_name) const;

    /// Generate detailed help text (man-page style)
    [[nodiscard]] std::string
    generate_help_verbose(std::string_view program_name) const;

    /// Find option by long name
    [[nodiscard]] const OptionSpec<Config> *
    find_option(std::string_view long_name) const;

    /// Find option by short name
    [[nodiscard]] const OptionSpec<Config> *find_option(char short_name) const;

    /// Find option by token (e.g., "--left" or "-l")
    [[nodiscard]] const OptionSpec<Config> *
    find_option_token(std::string_view token) const;

    /// Get all option specifications (for completion handlers)
    [[nodiscard]] std::span<const OptionSpec<Config>> specs() const noexcept {
        return specs_;
    }

  private:
    std::span<const OptionSpec<Config>> specs_;
    std::string_view description_;
    std::string_view examples_;
    std::string_view database_sources_;

    [[nodiscard]] ParseResult<Config>
    parse_impl(std::span<const std::string_view> args) const;

    [[nodiscard]] std::expected<void, std::string>
    validate_value(const OptionSpec<Config> *spec,
                   std::string_view value) const;

    void format_option_help(std::string &out,
                            const OptionSpec<Config> &opt) const;
    void format_option_help_verbose(std::string &out,
                                    const OptionSpec<Config> &opt) const;
};

/// Bash completion handler
class CompletionHandler {
  public:
    /// Handle bash completion request
    /// Returns 0 on success
    template <typename Config>
    static int handle_completion(const OptionParser<Config> &parser);

  private:
    /// Split command line into words (simple whitespace splitting)
    static std::vector<std::string_view> split_words(const std::string &line);

    /// Suggest option flags
    template <typename Config>
    static void suggest_options(const OptionParser<Config> &parser,
                                std::string_view prefix);

    /// Suggest values for an option
    static void suggest_values(std::span<const std::string_view> values,
                               std::string_view prefix);
};

} // namespace cli
