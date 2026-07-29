#include "engine/modding/flat_manifest.hpp"

#include "engine/core/file_io.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace heartstead::modding {

namespace {

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                              value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' ||
                              value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] bool is_key_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.';
}

[[nodiscard]] bool is_valid_key(std::string_view key) noexcept {
    return !key.empty() && key.front() != '.' && key.back() != '.' &&
           key.find("..") == std::string_view::npos && std::ranges::all_of(key, is_key_character);
}

void add_diagnostic(const std::filesystem::path& file, std::vector<ModDiagnostic>& diagnostics,
                    const FlatManifestParseOptions& options, std::string_view suffix,
                    std::string message) {
    diagnostics.push_back(ModDiagnostic{
        DiagnosticSeverity::error,
        file,
        std::string(options.diagnostic_prefix) + "." + std::string(suffix),
        std::move(message),
    });
}

struct LineSyntax {
    std::size_t content_end = 0;
    std::optional<std::size_t> separator;
    bool valid = true;
};

[[nodiscard]] LineSyntax inspect_line(std::string_view line) noexcept {
    LineSyntax syntax{line.size(), std::nullopt, true};
    char quote = '\0';
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const auto character = line[index];
        if (quote == '"' && escaped) {
            escaped = false;
            continue;
        }
        if (quote == '"' && character == '\\') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (character == quote) {
                quote = '\0';
            }
            continue;
        }
        if (character == '"' || character == '\'') {
            quote = character;
        } else if (character == '#') {
            syntax.content_end = index;
            break;
        } else if (character == '=' && !syntax.separator.has_value()) {
            syntax.separator = index;
        }
    }
    syntax.valid = quote == '\0' && !escaped;
    return syntax;
}

[[nodiscard]] std::optional<std::string> parse_quoted_value(std::string_view value) {
    if (value.empty()) {
        return std::nullopt;
    }
    const auto quote = value.front();
    if (quote != '"' && quote != '\'') {
        if (value.find('"') != std::string_view::npos ||
            value.find('\'') != std::string_view::npos) {
            return std::nullopt;
        }
        return std::string(value);
    }
    if (value.size() < 2) {
        return std::nullopt;
    }

    std::string decoded;
    decoded.reserve(value.size() - 2U);
    bool escaped = false;
    std::size_t closing_index = std::string_view::npos;
    for (std::size_t index = 1; index < value.size(); ++index) {
        const auto character = value[index];
        if (quote == '\'') {
            if (character == quote) {
                closing_index = index;
                break;
            }
            decoded.push_back(character);
            continue;
        }
        if (escaped) {
            switch (character) {
            case 'b':
                decoded.push_back('\b');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            case 'n':
                decoded.push_back('\n');
                break;
            case 'f':
                decoded.push_back('\f');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case '"':
                decoded.push_back('"');
                break;
            case '\\':
                decoded.push_back('\\');
                break;
            default:
                return std::nullopt;
            }
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
        } else if (character == quote) {
            closing_index = index;
            break;
        } else {
            decoded.push_back(character);
        }
    }
    if (escaped || closing_index == std::string_view::npos ||
        !trim(value.substr(closing_index + 1U)).empty()) {
        return std::nullopt;
    }
    return decoded;
}

} // namespace

std::map<std::string, std::string> parse_flat_manifest(const std::filesystem::path& file,
                                                       std::vector<ModDiagnostic>& diagnostics,
                                                       FlatManifestParseOptions options) {
    if (options.diagnostic_prefix.empty() || options.maximum_bytes == 0 ||
        options.maximum_line_bytes == 0 || options.maximum_fields == 0) {
        add_diagnostic(file, diagnostics, options, "invalid_limits",
                       "flat manifest parser limits and diagnostic prefix must be non-zero");
        return {};
    }

    auto source = core::read_text_file(file, {.maximum_bytes = options.maximum_bytes});
    if (!source) {
        add_diagnostic(file, diagnostics, options,
                       source.error().code == "core.file_too_large" ? "too_large" : "unreadable",
                       source.error().message);
        return {};
    }
    return parse_flat_manifest_text(source.value(), file, diagnostics, options);
}

std::map<std::string, std::string>
parse_flat_manifest_text(std::string_view text, const std::filesystem::path& source,
                         std::vector<ModDiagnostic>& diagnostics,
                         FlatManifestParseOptions options) {
    if (options.diagnostic_prefix.empty() || options.maximum_bytes == 0 ||
        options.maximum_line_bytes == 0 || options.maximum_fields == 0) {
        add_diagnostic(source, diagnostics, options, "invalid_limits",
                       "flat manifest parser limits and diagnostic prefix must be non-zero");
        return {};
    }
    if (text.size() > options.maximum_bytes) {
        add_diagnostic(source, diagnostics, options, "too_large",
                       "flat manifest source exceeds its byte limit");
        return {};
    }
    std::map<std::string, std::string> values;
    std::size_t line_number = 0;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        ++line_number;
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > options.maximum_line_bytes) {
            add_diagnostic(source, diagnostics, options, "too_large",
                           "manifest line exceeds its byte limit at line " +
                               std::to_string(line_number));
            return {};
        }

        const auto syntax = inspect_line(line);
        if (!syntax.valid) {
            add_diagnostic(source, diagnostics, options, "syntax",
                           "unterminated quote or escape at line " + std::to_string(line_number));
        } else {
            const auto content = line.substr(0, syntax.content_end);
            if (!trim(content).empty()) {
                if (!syntax.separator.has_value() || *syntax.separator >= syntax.content_end) {
                    add_diagnostic(source, diagnostics, options, "syntax",
                                   "expected key = value at line " + std::to_string(line_number));
                } else {
                    const auto key = trim(content.substr(0, *syntax.separator));
                    const auto encoded_value = trim(content.substr(*syntax.separator + 1U));
                    const auto value = parse_quoted_value(encoded_value);
                    if (!is_valid_key(key) || encoded_value.empty() || !value.has_value()) {
                        add_diagnostic(source, diagnostics, options, "syntax",
                                       "invalid key or value at line " +
                                           std::to_string(line_number));
                    } else if (values.contains(std::string(key))) {
                        add_diagnostic(source, diagnostics, options, "duplicate_key",
                                       "manifest key is duplicated at line " +
                                           std::to_string(line_number) + ": " + std::string(key));
                    } else if (values.size() >= options.maximum_fields) {
                        add_diagnostic(source, diagnostics, options, "too_large",
                                       "manifest exceeds its field limit");
                        return {};
                    } else {
                        values.emplace(std::string(key), std::move(*value));
                    }
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1U;
    }
    return values;
}

} // namespace heartstead::modding
