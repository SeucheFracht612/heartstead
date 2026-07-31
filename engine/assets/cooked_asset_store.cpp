#include "engine/assets/cooked_asset_store.hpp"

#include "engine/assets/asset_cooker.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/hash.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace heartstead::assets {

namespace {

constexpr std::string_view payload_magic = "heartstead.cooked_asset_payload.v1";
constexpr std::string_view payload_separator = "\n---\n";
constexpr std::string_view metadata_prefix = "meta.";
constexpr std::size_t max_payload_header_bytes = 1024U * 1024U;
constexpr std::size_t max_payload_header_line_bytes = 64U * 1024U;
constexpr std::size_t max_payload_header_fields = 256;

[[nodiscard]] core::Result<std::vector<std::uint8_t>>
read_file_bytes(const std::filesystem::path& path, std::size_t maximum_bytes) {
    auto bytes = core::read_binary_file(path, {.maximum_bytes = maximum_bytes});
    if (!bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            bytes.error().code == "core.file_too_large" ? "cooked_asset_store.file_too_large"
                                                        : "cooked_asset_store.read_failed",
            bytes.error().message);
    }
    return bytes;
}

[[nodiscard]] core::Result<std::string> read_text_file(const std::filesystem::path& path,
                                                       std::size_t maximum_bytes) {
    auto text = core::read_text_file(path, {.maximum_bytes = maximum_bytes});
    if (!text) {
        return core::Result<std::string>::failure(text.error().code == "core.file_too_large"
                                                      ? "cooked_asset_store.file_too_large"
                                                      : "cooked_asset_store.read_failed",
                                                  text.error().message);
    }
    return text;
}

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value,
                                                    std::string_view field_name) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure("cooked_asset_store.invalid_number",
                                                    "invalid numeric cooked asset payload field: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint64_t>::success(parsed);
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
    return core::Result<AssetKind>::failure("cooked_asset_store.unknown_asset_kind",
                                            "unknown cooked asset payload kind");
}

[[nodiscard]] core::Result<std::map<std::string, std::string>>
parse_payload_header(std::string_view header) {
    std::size_t line_start = 0;
    bool saw_magic = false;
    std::map<std::string, std::string> fields;

    while (line_start <= header.size()) {
        const auto line_end = header.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? header.substr(line_start)
                        : header.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.size() > max_payload_header_line_bytes) {
            return core::Result<std::map<std::string, std::string>>::failure(
                "cooked_asset_store.header_too_large",
                "cooked asset payload header line exceeds its safety limit");
        }

        if (!saw_magic) {
            if (line != payload_magic) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    "cooked_asset_store.invalid_magic",
                    "cooked asset payload does not start with expected magic");
            }
            saw_magic = true;
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos || separator == 0) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    "cooked_asset_store.invalid_header",
                    "cooked asset payload header line must use key=value syntax");
            }
            auto key = std::string(line.substr(0, separator));
            auto value = std::string(line.substr(separator + 1));
            if (!fields.contains(key) && fields.size() >= max_payload_header_fields) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    "cooked_asset_store.header_too_large",
                    "cooked asset payload header has too many fields");
            }
            const auto [_, inserted] = fields.emplace(std::move(key), std::move(value));
            if (!inserted) {
                return core::Result<std::map<std::string, std::string>>::failure(
                    "cooked_asset_store.duplicate_key",
                    "cooked asset payload header key is duplicated");
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!saw_magic) {
        return core::Result<std::map<std::string, std::string>>::failure(
            "cooked_asset_store.invalid_magic", "cooked asset payload is missing magic");
    }
    return core::Result<std::map<std::string, std::string>>::success(std::move(fields));
}

[[nodiscard]] core::Result<std::string_view>
require_field(const std::map<std::string, std::string>& fields, std::string_view key) {
    const auto found = fields.find(std::string(key));
    if (found == fields.end() || found->second.empty()) {
        return core::Result<std::string_view>::failure(
            "cooked_asset_store.missing_key",
            "cooked asset payload is missing required header key: " + std::string(key));
    }
    return core::Result<std::string_view>::success(found->second);
}

[[nodiscard]] core::Status require_field_value(const std::map<std::string, std::string>& fields,
                                               std::string_view key, std::string_view expected) {
    auto field = require_field(fields, key);
    if (!field) {
        return core::Status::failure(field.error().code, field.error().message);
    }
    if (field.value() != expected) {
        return core::Status::failure("cooked_asset_store.header_mismatch",
                                     "cooked asset payload header does not match manifest field: " +
                                         std::string(key));
    }
    return core::Status::ok();
}

[[nodiscard]] std::string_view expected_backend_for_profile(AssetKind kind,
                                                            std::string_view profile) noexcept {
    const auto backend = profile == "production" ? AssetCookBackend::production_converters
                                                 : AssetCookBackend::development_passthrough;
    return asset_cook_pipeline_name(kind, backend);
}

[[nodiscard]] std::map<std::string, std::string>
payload_metadata_from_fields(const std::map<std::string, std::string>& fields) {
    std::map<std::string, std::string> metadata;
    for (const auto& [key, value] : fields) {
        if (key.starts_with(metadata_prefix) && key.size() > metadata_prefix.size()) {
            metadata.emplace(key.substr(metadata_prefix.size()), value);
        }
    }
    return metadata;
}

} // namespace

core::Result<CookedAssetPayload>
CookedAssetPayloadCodec::decode(std::span<const std::uint8_t> bytes,
                                const CookedAssetRecord& expected,
                                std::string_view expected_profile, std::size_t maximum_bytes) {
    if (maximum_bytes == 0 || bytes.size() > maximum_bytes) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.file_too_large", "cooked asset payload exceeds its safety limit");
    }
    if (core::stable_hash64_hex(bytes) != expected.cooked_hash) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.cooked_hash_mismatch",
            "cooked asset payload hash does not match manifest record");
    }

    const auto text = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto separator = text.find(payload_separator);
    if (separator == std::string_view::npos) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.missing_separator",
            "cooked asset payload is missing header separator");
    }
    if (separator > max_payload_header_bytes) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.header_too_large",
            "cooked asset payload header exceeds its safety limit");
    }

    auto fields = parse_payload_header(text.substr(0, separator));
    if (!fields) {
        return core::Result<CookedAssetPayload>::failure(fields.error().code,
                                                         fields.error().message);
    }

    auto status = require_field_value(fields.value(), "logical_id", expected.logical_id);
    if (!status) {
        return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                         status.error().message);
    }
    status = require_field_value(fields.value(), "kind", asset_kind_name(expected.kind));
    if (!status) {
        return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                         status.error().message);
    }
    const auto expected_source_virtual_path = expected.source_virtual_path.to_string();
    status =
        require_field_value(fields.value(), "source_virtual_path", expected_source_virtual_path);
    if (!status) {
        return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                         status.error().message);
    }
    status = require_field_value(fields.value(), "source_hash", expected.source_hash);
    if (!status) {
        return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                         status.error().message);
    }
    if (expected.pipeline_id != "legacy_v1") {
        status = require_field_value(fields.value(), "dependency_hash", expected.dependency_hash);
        if (!status) {
            return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                             status.error().message);
        }
        status = require_field_value(fields.value(), "pipeline_id", expected.pipeline_id);
        if (!status) {
            return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                             status.error().message);
        }
    }
    const auto expected_pipeline_version = std::to_string(expected.pipeline_version);
    status = require_field_value(fields.value(), "pipeline_version", expected_pipeline_version);
    if (!status) {
        return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                         status.error().message);
    }
    status = require_field_value(fields.value(), "profile", expected_profile);
    if (!status) {
        return core::Result<CookedAssetPayload>::failure(status.error().code,
                                                         status.error().message);
    }

    auto backend = require_field(fields.value(), "backend");
    auto source_bytes = require_field(fields.value(), "source_bytes");
    if (!backend || !source_bytes) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.missing_key",
            "cooked asset payload is missing required header keys");
    }
    const auto expected_backend =
        expected.pipeline_id == "legacy_v1"
            ? expected_backend_for_profile(expected.kind, expected_profile)
            : std::string_view(expected.pipeline_id);
    if (backend.value() != expected_backend) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.backend_mismatch",
            "cooked asset payload backend does not match expected asset kind and profile");
    }

    auto parsed_source_bytes = parse_u64(source_bytes.value(), "source_bytes");
    if (!parsed_source_bytes) {
        return core::Result<CookedAssetPayload>::failure(parsed_source_bytes.error().code,
                                                         parsed_source_bytes.error().message);
    }

    const auto payload_offset = separator + payload_separator.size();
    const auto payload_size = bytes.size() - payload_offset;
    if (parsed_source_bytes.value() != static_cast<std::uint64_t>(payload_size)) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.payload_size_mismatch",
            "cooked asset payload size does not match source_bytes header");
    }

    auto kind = parse_asset_kind(require_field(fields.value(), "kind").value());
    if (!kind) {
        return core::Result<CookedAssetPayload>::failure(kind.error().code, kind.error().message);
    }

    CookedAssetPayload payload;
    payload.logical_id = expected.logical_id;
    payload.kind = kind.value();
    payload.backend = std::string(backend.value());
    payload.profile = std::string(expected_profile);
    payload.source_path = expected.source_virtual_path.relative_path;
    payload.metadata = payload_metadata_from_fields(fields.value());
    payload.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(payload_offset), bytes.end());
    return core::Result<CookedAssetPayload>::success(std::move(payload));
}

core::Result<CookedAssetStore> CookedAssetStore::load(std::filesystem::path root,
                                                      std::filesystem::path manifest_relative_path,
                                                      CookedAssetStoreLoadOptions options) {
    if (root.empty()) {
        return core::Result<CookedAssetStore>::failure("cooked_asset_store.invalid_root",
                                                       "cooked asset store root is required");
    }
    if (!is_safe_asset_relative_path(manifest_relative_path)) {
        return core::Result<CookedAssetStore>::failure(
            "cooked_asset_store.invalid_manifest_path",
            "cooked asset manifest path must be a safe relative path");
    }
    if (options.maximum_manifest_bytes == 0 || options.maximum_payload_bytes == 0) {
        return core::Result<CookedAssetStore>::failure(
            "cooked_asset_store.invalid_read_limit",
            "cooked asset store read limits must be non-zero");
    }

    auto canonical_root = canonical_asset_root(root);
    if (!canonical_root) {
        return core::Result<CookedAssetStore>::failure(canonical_root.error().code,
                                                       canonical_root.error().message);
    }
    auto manifest_path = resolve_asset_path(canonical_root.value(), manifest_relative_path);
    if (!manifest_path) {
        return core::Result<CookedAssetStore>::failure(manifest_path.error().code,
                                                       manifest_path.error().message);
    }
    auto manifest_text = read_text_file(manifest_path.value(), options.maximum_manifest_bytes);
    if (!manifest_text) {
        return core::Result<CookedAssetStore>::failure(manifest_text.error().code,
                                                       manifest_text.error().message);
    }
    auto manifest = CookedAssetManifestTextCodec::decode(manifest_text.value());
    if (!manifest) {
        return core::Result<CookedAssetStore>::failure(manifest.error().code,
                                                       manifest.error().message);
    }

    CookedAssetStore store;
    store.root_ = std::move(canonical_root).value();
    store.manifest_ = std::move(manifest).value();
    store.maximum_payload_bytes_ = options.maximum_payload_bytes;
    return core::Result<CookedAssetStore>::success(std::move(store));
}

const CookedAssetManifest& CookedAssetStore::manifest() const noexcept {
    return manifest_;
}

const std::filesystem::path& CookedAssetStore::root() const noexcept {
    return root_;
}

core::Result<CookedAssetPayload> CookedAssetStore::load_payload(std::string_view logical_id) const {
    const auto* record = manifest_.find_active(logical_id);
    if (record == nullptr) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.asset_not_found",
            "cooked asset manifest does not contain logical id: " + std::string(logical_id));
    }
    return load_payload(*record);
}

core::Result<CookedAssetPayload>
CookedAssetStore::load_payload(const CookedAssetRecord& record) const {
    if (!is_safe_asset_relative_path(record.cooked_relative_path)) {
        return core::Result<CookedAssetPayload>::failure(
            "cooked_asset_store.invalid_cooked_path",
            "cooked asset record path must be a safe relative path");
    }

    auto payload_path = resolve_asset_path(root_, record.cooked_relative_path);
    if (!payload_path) {
        return core::Result<CookedAssetPayload>::failure(payload_path.error().code,
                                                         payload_path.error().message);
    }
    auto bytes = read_file_bytes(payload_path.value(), maximum_payload_bytes_);
    if (!bytes) {
        return core::Result<CookedAssetPayload>::failure(bytes.error().code, bytes.error().message);
    }
    return CookedAssetPayloadCodec::decode(bytes.value(), record, manifest_.profile,
                                           maximum_payload_bytes_);
}

} // namespace heartstead::assets
