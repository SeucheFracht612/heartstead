#include "engine/assets/cooked_asset_manifest.hpp"

#include "engine/core/hash.hpp"
#include "engine/core/ids.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace heartstead::assets {

namespace {

constexpr std::string_view magic_v1 = "heartstead.cooked_assets.v1";
constexpr std::string_view magic_v2 = "heartstead.cooked_assets.v2";
constexpr std::size_t max_manifest_bytes = 64U * 1024U * 1024U;
constexpr std::size_t max_manifest_line_bytes = 1024U * 1024U;
constexpr std::size_t max_manifest_records = 1'000'000;
constexpr std::size_t max_record_dependencies = 4096;

[[nodiscard]] bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

[[nodiscard]] char hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return static_cast<char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<char>(10 + value - 'a');
    }
    return static_cast<char>(10 + value - 'A');
}

[[nodiscard]] std::string percent_escape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto value : input) {
        const auto byte = static_cast<unsigned char>(value);
        if (value == '%' || value == '|' || value == '=' || value == ',' || value == '\n' ||
            value == '\r') {
            result.push_back('%');
            result.push_back(hex[(byte >> 4u) & 0x0Fu]);
            result.push_back(hex[byte & 0x0Fu]);
        } else {
            result.push_back(value);
        }
    }

    return result;
}

[[nodiscard]] core::Result<std::string> percent_unescape(std::string_view input) {
    std::string result;
    result.reserve(input.size());

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '%') {
            result.push_back(input[index]);
            continue;
        }

        if (index + 2 >= input.size() || !is_hex_digit(input[index + 1]) ||
            !is_hex_digit(input[index + 2])) {
            return core::Result<std::string>::failure(
                "cooked_asset_manifest.invalid_escape",
                "cooked asset manifest contains an invalid escape");
        }

        const auto high = hex_value(input[index + 1]);
        const auto low = hex_value(input[index + 2]);
        result.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }

    return core::Result<std::string>::success(std::move(result));
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(start));
            break;
        }
        result.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field_name) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure(
            "cooked_asset_manifest.invalid_number",
            "invalid numeric cooked asset manifest field: " + std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint32_t> parse_u32(std::string_view value,
                                                    std::string_view field_name) {
    auto parsed = parse_u64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint32_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure(
            "cooked_asset_manifest.number_out_of_range",
            "numeric cooked asset manifest field is too large: " + std::string(field_name));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

[[nodiscard]] bool is_valid_relative_path(const std::filesystem::path& path) {
    return is_safe_asset_relative_path(path) && core::is_valid_local_id(path.generic_string());
}

[[nodiscard]] bool is_valid_stable_hash(std::string_view value) noexcept {
    return value.size() == 16 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

[[nodiscard]] core::Result<AssetKind> parse_asset_kind(std::string_view value) {
    for (const auto kind :
         {AssetKind::texture, AssetKind::model, AssetKind::shader, AssetKind::material,
          AssetKind::sound, AssetKind::music, AssetKind::font, AssetKind::localization,
          AssetKind::ui, AssetKind::data, AssetKind::unknown}) {
        if (value == asset_kind_name(kind)) {
            return core::Result<AssetKind>::success(kind);
        }
    }
    return core::Result<AssetKind>::failure("cooked_asset_manifest.unknown_asset_kind",
                                            "unknown cooked asset kind: " + std::string(value));
}

[[nodiscard]] core::Result<AssetSourceKind> parse_source_kind(std::string_view value) {
    for (const auto kind :
         {AssetSourceKind::mod, AssetSourceKind::resource_pack, AssetSourceKind::engine}) {
        if (value == asset_source_kind_name(kind)) {
            return core::Result<AssetSourceKind>::success(kind);
        }
    }
    return core::Result<AssetSourceKind>::failure("cooked_asset_manifest.unknown_source_kind",
                                                  "unknown cooked asset source kind: " +
                                                      std::string(value));
}

[[nodiscard]] std::filesystem::path make_cooked_path(const AssetRecord& record,
                                                     const CookedAssetBuildConfig& config) {
    std::filesystem::path path;
    path /= record.source_id;
    path /= config.profile;
    path /= std::string(asset_kind_name(record.kind));
    auto logical_path = asset_logical_path(record.logical_id);
    if (!logical_path)
        return {};
    path /= logical_path.value();
    path += ".cooked";
    return path;
}

[[nodiscard]] CookedAssetRecord make_cooked_record(const AssetCatalog& catalog,
                                                   const AssetRecord& asset,
                                                   const CookedAssetBuildConfig& config) {
    const auto* active = catalog.find_active(asset.logical_id);
    const auto cooked_path = make_cooked_path(asset, config);
    const auto cooked_hash =
        core::stable_hash64_hex(asset.logical_id + "|" + asset.content_hash + "|" + config.profile +
                                "|" + std::to_string(config.pipeline_version));

    return CookedAssetRecord{
        asset.logical_id,  asset.kind,         asset.virtual_path,      asset.source_kind,
        asset.source_id,   asset.content_hash, "0000000000000000",      "unassigned",
        cooked_path,       cooked_hash,        config.pipeline_version, active == &asset,
        asset.dependencies};
}

[[nodiscard]] core::Result<std::string> dependency_closure_hash(const AssetCatalog& catalog,
                                                                const AssetRecord& root) {
    core::StableHash64 hasher;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> visiting;
    const auto visit = [&](const auto& self, const AssetRecord& record) -> core::Status {
        if (visited.contains(record.logical_id)) {
            return core::Status::ok();
        }
        if (!visiting.insert(record.logical_id).second) {
            return core::Status::failure("cooked_asset_manifest.dependency_cycle",
                                         "asset dependency graph contains a cycle at " +
                                             record.logical_id);
        }
        auto dependencies = record.dependencies;
        std::ranges::sort(dependencies, {}, &VirtualPath::to_string);
        for (const auto& dependency : dependencies) {
            const auto logical_id = asset_logical_id(dependency);
            const auto* child = catalog.find_active(logical_id);
            if (child == nullptr) {
                return core::Status::failure(
                    "cooked_asset_manifest.missing_dependency",
                    "asset dependency closure contains an unresolved asset: " + record.logical_id +
                        " -> " + logical_id);
            }
            hasher.add_string(logical_id);
            hasher.add_string(child->content_hash);
            auto status = self(self, *child);
            if (!status) {
                return status;
            }
        }
        visiting.erase(record.logical_id);
        visited.insert(record.logical_id);
        return core::Status::ok();
    };
    auto status = visit(visit, root);
    if (!status) {
        return core::Result<std::string>::failure(status.error().code, status.error().message);
    }
    return core::Result<std::string>::success(hasher.hex());
}

[[nodiscard]] std::string encode_dependencies(const std::vector<VirtualPath>& dependencies) {
    std::ostringstream output;
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << percent_escape(dependencies[index].to_string());
    }
    return output.str();
}

[[nodiscard]] core::Result<std::vector<VirtualPath>> decode_dependencies(std::string_view encoded) {
    std::vector<VirtualPath> dependencies;
    if (encoded.empty()) {
        return core::Result<std::vector<VirtualPath>>::success(std::move(dependencies));
    }

    if (static_cast<std::size_t>(std::ranges::count(encoded, ',')) >= max_record_dependencies) {
        return core::Result<std::vector<VirtualPath>>::failure(
            "cooked_asset_manifest.too_many_dependencies",
            "cooked asset record has too many dependencies");
    }
    for (const auto part : split(encoded, ',')) {
        auto unescaped = percent_unescape(part);
        if (!unescaped) {
            return core::Result<std::vector<VirtualPath>>::failure(unescaped.error().code,
                                                                   unescaped.error().message);
        }
        auto parsed = VirtualPath::parse(unescaped.value());
        if (!parsed) {
            return core::Result<std::vector<VirtualPath>>::failure(parsed.error().code,
                                                                   parsed.error().message);
        }
        dependencies.push_back(std::move(parsed).value());
    }

    return core::Result<std::vector<VirtualPath>>::success(std::move(dependencies));
}

[[nodiscard]] core::Result<CookedAssetRecord> parse_record(std::string_view value,
                                                           std::uint32_t schema_version) {
    const auto parts = split(value, '|');
    const auto expected_fields = schema_version >= 2U ? 13U : 11U;
    if (parts.size() != expected_fields) {
        return core::Result<CookedAssetRecord>::failure(
            "cooked_asset_manifest.invalid_record",
            "cooked asset record has the wrong number of pipe-separated fields");
    }

    auto logical_id = percent_unescape(parts[0]);
    auto kind = parse_asset_kind(parts[1]);
    auto source_virtual_path_text = percent_unescape(parts[2]);
    auto source_kind = parse_source_kind(parts[3]);
    auto source_id = percent_unescape(parts[4]);
    auto source_hash = percent_unescape(parts[5]);
    const auto cooked_path_field = schema_version >= 2U ? 8U : 6U;
    const auto cooked_hash_field = cooked_path_field + 1U;
    const auto pipeline_version_field = cooked_path_field + 2U;
    const auto active_field = cooked_path_field + 3U;
    const auto dependency_field = cooked_path_field + 4U;
    auto dependency_hash = schema_version >= 2U
                               ? percent_unescape(parts[6])
                               : core::Result<std::string>::success("0000000000000000");
    auto pipeline_id = schema_version >= 2U ? percent_unescape(parts[7])
                                            : core::Result<std::string>::success("legacy_v1");
    auto cooked_relative_path = percent_unescape(parts[cooked_path_field]);
    auto cooked_hash = percent_unescape(parts[cooked_hash_field]);
    auto pipeline_version = parse_u32(parts[pipeline_version_field], "pipeline_version");
    auto dependencies = decode_dependencies(parts[dependency_field]);

    if (!logical_id || !kind || !source_virtual_path_text || !source_kind || !source_id ||
        !source_hash || !dependency_hash || !pipeline_id || !cooked_relative_path || !cooked_hash ||
        !pipeline_version || !dependencies) {
        return core::Result<CookedAssetRecord>::failure(
            "cooked_asset_manifest.invalid_record", "cooked asset record contains invalid fields");
    }

    auto source_virtual_path = VirtualPath::parse(source_virtual_path_text.value());
    if (!source_virtual_path) {
        return core::Result<CookedAssetRecord>::failure(source_virtual_path.error().code,
                                                        source_virtual_path.error().message);
    }

    if (parts[active_field] != "0" && parts[active_field] != "1") {
        return core::Result<CookedAssetRecord>::failure("cooked_asset_manifest.invalid_active_flag",
                                                        "cooked asset active flag must be 0 or 1");
    }

    return core::Result<CookedAssetRecord>::success(CookedAssetRecord{
        std::move(logical_id).value(),
        kind.value(),
        std::move(source_virtual_path).value(),
        source_kind.value(),
        std::move(source_id).value(),
        std::move(source_hash).value(),
        std::move(dependency_hash).value(),
        std::move(pipeline_id).value(),
        std::filesystem::path(std::move(cooked_relative_path).value()),
        std::move(cooked_hash).value(),
        pipeline_version.value(),
        parts[active_field] == "1",
        std::move(dependencies).value(),
    });
}

} // namespace

bool CookedAssetDependencyReport::has_errors() const noexcept {
    return !issues.empty();
}

core::Status CookedAssetManifest::validate() const {
    if (schema_version == 0) {
        return core::Status::failure("cooked_asset_manifest.invalid_schema",
                                     "cooked asset manifest schema version must be non-zero");
    }
    if (!core::is_valid_namespace_id(profile)) {
        return core::Status::failure("cooked_asset_manifest.invalid_profile",
                                     "cooked asset profile must be a safe identifier");
    }

    std::unordered_set<std::string> record_identities;
    std::unordered_set<std::string> cooked_paths;
    std::unordered_map<std::string, std::size_t> active_counts;
    std::unordered_set<std::string> logical_ids;

    for (const auto& record : records) {
        if (!core::PrototypeId::parse(record.logical_id)) {
            return core::Status::failure("cooked_asset_manifest.invalid_logical_id",
                                         "cooked asset logical id is invalid");
        }
        if (!core::is_valid_namespace_id(record.source_virtual_path.namespace_id) ||
            !is_valid_relative_path(record.source_virtual_path.relative_path)) {
            return core::Status::failure("cooked_asset_manifest.invalid_source_path",
                                         "cooked asset source virtual path is invalid");
        }
        if (record.logical_id != asset_logical_id(record.source_virtual_path)) {
            return core::Status::failure(
                "cooked_asset_manifest.logical_path_mismatch",
                "cooked asset logical id must equal its source virtual path");
        }
        if (!core::is_valid_namespace_id(record.source_id)) {
            return core::Status::failure("cooked_asset_manifest.invalid_source_id",
                                         "cooked asset source id is invalid");
        }
        if (!is_valid_stable_hash(record.source_hash) ||
            !is_valid_stable_hash(record.dependency_hash)) {
            return core::Status::failure(
                "cooked_asset_manifest.invalid_source_hash",
                "cooked asset source and dependency hashes must be lowercase 64-bit hexadecimal "
                "digests");
        }
        if (record.pipeline_id.empty() || record.pipeline_id.size() > 256U ||
            record.pipeline_id.find_first_of("\r\n|") != std::string::npos) {
            return core::Status::failure(
                "cooked_asset_manifest.invalid_pipeline_id",
                "cooked asset pipeline identity must be a bounded single-line value");
        }
        if (!is_valid_relative_path(record.cooked_relative_path)) {
            return core::Status::failure("cooked_asset_manifest.invalid_cooked_path",
                                         "cooked asset output path is invalid");
        }
        if (!is_valid_stable_hash(record.cooked_hash)) {
            return core::Status::failure(
                "cooked_asset_manifest.invalid_cooked_hash",
                "cooked asset hash must be a lowercase 64-bit hexadecimal digest");
        }
        if (record.pipeline_version == 0) {
            return core::Status::failure("cooked_asset_manifest.invalid_pipeline_version",
                                         "cooked asset pipeline version must be non-zero");
        }
        for (const auto& dependency : record.dependencies) {
            if (!core::is_valid_namespace_id(dependency.namespace_id) ||
                !is_valid_relative_path(dependency.relative_path)) {
                return core::Status::failure("cooked_asset_manifest.invalid_dependency",
                                             "cooked asset dependency virtual path is invalid");
            }
        }

        const auto identity = record.logical_id + "|" +
                              std::string(asset_source_kind_name(record.source_kind)) + "|" +
                              record.source_id;
        if (!record_identities.insert(identity).second) {
            return core::Status::failure(
                "cooked_asset_manifest.duplicate_record",
                "cooked asset manifest contains a duplicate logical/source record");
        }
        if (!cooked_paths.insert(record.cooked_relative_path.generic_string()).second) {
            return core::Status::failure("cooked_asset_manifest.duplicate_cooked_path",
                                         "cooked asset manifest contains a duplicate output path");
        }
        logical_ids.insert(record.logical_id);
        if (record.active) {
            ++active_counts[record.logical_id];
        }
    }

    for (const auto& logical_id : logical_ids) {
        const auto active_count = active_counts[logical_id];
        if (active_count != 1) {
            return core::Status::failure(
                active_count == 0 ? "cooked_asset_manifest.missing_active_record"
                                  : "cooked_asset_manifest.multiple_active_records",
                "cooked asset manifest must contain exactly one active record per logical id");
        }
    }

    return core::Status::ok();
}

const CookedAssetRecord* CookedAssetManifest::find(std::string_view logical_id) const noexcept {
    const auto found = std::ranges::find_if(records, [logical_id](const CookedAssetRecord& record) {
        return record.logical_id == logical_id;
    });
    return found == records.end() ? nullptr : &*found;
}

const CookedAssetRecord*
CookedAssetManifest::find_active(std::string_view logical_id) const noexcept {
    const auto found = std::ranges::find_if(records, [logical_id](const CookedAssetRecord& record) {
        return record.logical_id == logical_id && record.active;
    });
    return found == records.end() ? nullptr : &*found;
}

std::vector<const CookedAssetRecord*>
CookedAssetManifest::records_for(std::string_view logical_id) const {
    std::vector<const CookedAssetRecord*> result;
    for (const auto& record : records) {
        if (record.logical_id == logical_id) {
            result.push_back(&record);
        }
    }
    return result;
}

std::size_t CookedAssetManifest::count_kind(AssetKind kind) const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        records, [kind](const CookedAssetRecord& record) { return record.kind == kind; }));
}

std::size_t CookedAssetManifest::active_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        records, [](const CookedAssetRecord& record) { return record.active; }));
}

CookedAssetDependencyReport CookedAssetManifest::dependency_report() const {
    CookedAssetDependencyReport report;
    for (const auto& record : records) {
        if (!record.active) {
            continue;
        }

        for (const auto& dependency : record.dependencies) {
            const auto dependency_logical_id = asset_logical_id(dependency);
            if (find_active(dependency_logical_id) != nullptr) {
                continue;
            }

            report.issues.push_back(CookedAssetDependencyIssue{
                record.logical_id,
                dependency,
                "cooked_asset_manifest.missing_dependency",
                "cooked asset has an unresolved active dependency: " + record.logical_id + " -> " +
                    dependency.to_string(),
            });
        }
    }
    return report;
}

core::Status CookedAssetManifest::validate_dependencies() const {
    auto report = dependency_report();
    if (report.issues.empty()) {
        return core::Status::ok();
    }
    const auto& issue = report.issues.front();
    return core::Status::failure(issue.code, issue.message);
}

core::Result<CookedAssetManifest> CookedAssetManifestBuilder::build(const AssetCatalog& catalog,
                                                                    CookedAssetBuildConfig config) {
    if (!core::is_valid_namespace_id(config.profile)) {
        return core::Result<CookedAssetManifest>::failure(
            "cooked_asset_manifest.invalid_profile",
            "cooked asset profile must be a safe identifier");
    }
    if (config.pipeline_version == 0) {
        return core::Result<CookedAssetManifest>::failure(
            "cooked_asset_manifest.invalid_pipeline_version",
            "cooked asset pipeline version must be non-zero");
    }

    auto source_records = config.active_assets_only ? catalog.active_records() : catalog.records();
    std::ranges::sort(source_records, [](const AssetRecord* left, const AssetRecord* right) {
        if (left->logical_id != right->logical_id) {
            return left->logical_id < right->logical_id;
        }
        if (left->source_id != right->source_id) {
            return left->source_id < right->source_id;
        }
        return left->priority < right->priority;
    });

    CookedAssetManifest manifest;
    manifest.profile = config.profile;
    for (const auto* asset : source_records) {
        auto record = make_cooked_record(catalog, *asset, config);
        auto dependency_hash = dependency_closure_hash(catalog, *asset);
        if (!dependency_hash) {
            return core::Result<CookedAssetManifest>::failure(dependency_hash.error().code,
                                                              dependency_hash.error().message);
        }
        record.dependency_hash = std::move(dependency_hash).value();
        manifest.records.push_back(std::move(record));
    }

    auto status = manifest.validate();
    if (!status) {
        return core::Result<CookedAssetManifest>::failure(status.error().code,
                                                          status.error().message);
    }
    status = manifest.validate_dependencies();
    if (!status) {
        return core::Result<CookedAssetManifest>::failure(status.error().code,
                                                          status.error().message);
    }

    return core::Result<CookedAssetManifest>::success(std::move(manifest));
}

std::string CookedAssetManifestTextCodec::encode(const CookedAssetManifest& manifest) {
    std::ostringstream output;
    const auto use_v2 = manifest.schema_version >= 2U;
    output << (use_v2 ? magic_v2 : magic_v1) << '\n';
    output << "schema_version=" << manifest.schema_version << '\n';
    output << "profile=" << percent_escape(manifest.profile) << '\n';

    for (const auto& record : manifest.records) {
        output << "record=" << percent_escape(record.logical_id) << '|'
               << asset_kind_name(record.kind) << '|'
               << percent_escape(record.source_virtual_path.to_string()) << '|'
               << asset_source_kind_name(record.source_kind) << '|'
               << percent_escape(record.source_id) << '|' << percent_escape(record.source_hash)
               << '|';
        if (use_v2) {
            output << percent_escape(record.dependency_hash) << '|'
                   << percent_escape(record.pipeline_id) << '|';
        }
        output << percent_escape(record.cooked_relative_path.generic_string()) << '|'
               << percent_escape(record.cooked_hash) << '|' << record.pipeline_version << '|'
               << (record.active ? '1' : '0') << '|' << encode_dependencies(record.dependencies)
               << '\n';
    }

    output << "end\n";
    return output.str();
}

core::Result<CookedAssetManifest> CookedAssetManifestTextCodec::decode(std::string_view text) {
    if (text.size() > max_manifest_bytes) {
        return core::Result<CookedAssetManifest>::failure(
            "cooked_asset_manifest.too_large", "cooked asset manifest exceeds its size limit");
    }
    CookedAssetManifest manifest;
    bool saw_magic = false;
    bool saw_end = false;
    bool saw_schema = false;
    bool saw_profile = false;
    std::uint32_t magic_version = 0;
    std::size_t consumed_bytes = 0;

    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > max_manifest_line_bytes) {
            return core::Result<CookedAssetManifest>::failure(
                "cooked_asset_manifest.too_large",
                "cooked asset manifest line exceeds its size limit");
        }

        if (!saw_magic) {
            magic_version = line == magic_v2 ? 2U : line == magic_v1 ? 1U : 0U;
            if (magic_version == 0U) {
                return core::Result<CookedAssetManifest>::failure(
                    "cooked_asset_manifest.invalid_magic",
                    "cooked asset manifest does not start with the expected magic");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
            consumed_bytes = line_end == std::string_view::npos ? text.size() : line_end + 1;
            break;
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos) {
                return core::Result<CookedAssetManifest>::failure(
                    "cooked_asset_manifest.invalid_line",
                    "cooked asset manifest line is missing key/value separator");
            }

            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);

            if (key == "schema_version") {
                if (saw_schema)
                    return core::Result<CookedAssetManifest>::failure(
                        "cooked_asset_manifest.duplicate_schema",
                        "cooked asset manifest schema is duplicated");
                auto parsed = parse_u32(value, key);
                if (!parsed) {
                    return core::Result<CookedAssetManifest>::failure(parsed.error().code,
                                                                      parsed.error().message);
                }
                manifest.schema_version = parsed.value();
                saw_schema = true;
            } else if (key == "profile") {
                if (saw_profile)
                    return core::Result<CookedAssetManifest>::failure(
                        "cooked_asset_manifest.duplicate_profile",
                        "cooked asset manifest profile is duplicated");
                auto parsed = percent_unescape(value);
                if (!parsed) {
                    return core::Result<CookedAssetManifest>::failure(parsed.error().code,
                                                                      parsed.error().message);
                }
                manifest.profile = std::move(parsed).value();
                saw_profile = true;
            } else if (key == "record") {
                if (manifest.records.size() >= max_manifest_records) {
                    return core::Result<CookedAssetManifest>::failure(
                        "cooked_asset_manifest.too_large",
                        "cooked asset manifest has too many records");
                }
                if (!saw_schema) {
                    return core::Result<CookedAssetManifest>::failure(
                        "cooked_asset_manifest.missing_schema",
                        "schema_version must precede cooked asset records");
                }
                auto parsed = parse_record(value, manifest.schema_version);
                if (!parsed) {
                    return core::Result<CookedAssetManifest>::failure(parsed.error().code,
                                                                      parsed.error().message);
                }
                manifest.records.push_back(std::move(parsed).value());
            } else {
                return core::Result<CookedAssetManifest>::failure(
                    "cooked_asset_manifest.unknown_key",
                    "unknown cooked asset manifest key: " + std::string(key));
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!saw_magic || !saw_end || !saw_schema || !saw_profile) {
        return core::Result<CookedAssetManifest>::failure(
            "cooked_asset_manifest.incomplete_manifest",
            "cooked asset manifest is missing required fields");
    }
    if (manifest.schema_version != magic_version || manifest.schema_version > 2U) {
        return core::Result<CookedAssetManifest>::failure(
            "cooked_asset_manifest.unsupported_schema",
            "cooked asset manifest magic and schema version are unsupported or inconsistent");
    }
    if (consumed_bytes != text.size()) {
        return core::Result<CookedAssetManifest>::failure(
            "cooked_asset_manifest.trailing_data",
            "cooked asset manifest contains data after its end marker");
    }

    auto status = manifest.validate();
    if (!status) {
        return core::Result<CookedAssetManifest>::failure(status.error().code,
                                                          status.error().message);
    }

    return core::Result<CookedAssetManifest>::success(std::move(manifest));
}

} // namespace heartstead::assets
