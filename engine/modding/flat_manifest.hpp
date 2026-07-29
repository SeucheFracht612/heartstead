#pragma once

#include "engine/modding/mod_diagnostic.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::modding {

struct FlatManifestParseOptions {
    std::string_view diagnostic_prefix;
    std::size_t maximum_bytes = 1024U * 1024U;
    std::size_t maximum_line_bytes = 64U * 1024U;
    std::size_t maximum_fields = 256;
};

// Parses the deliberately small flat key/value subset used by engine manifests. It supports
// comments outside quoted strings, single-quoted literals, and common double-quoted escapes.
// Tables, arrays, multiline strings, and duplicate keys are intentionally rejected.
[[nodiscard]] std::map<std::string, std::string>
parse_flat_manifest(const std::filesystem::path& file, std::vector<ModDiagnostic>& diagnostics,
                    FlatManifestParseOptions options);

[[nodiscard]] std::map<std::string, std::string>
parse_flat_manifest_text(std::string_view text, const std::filesystem::path& source,
                         std::vector<ModDiagnostic>& diagnostics,
                         FlatManifestParseOptions options);

} // namespace heartstead::modding
