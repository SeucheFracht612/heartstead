#include "engine/assets/asset_catalog.hpp"

#include "engine/assets/model_asset.hpp"
#include "engine/core/hash.hpp"
#include "engine/core/ids.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <span>
#include <system_error>
#include <unordered_set>

namespace heartstead::assets {

namespace {

[[nodiscard]] bool is_valid_logical_id(std::string_view logical_id) noexcept {
    return core::PrototypeId::parse(logical_id).has_value();
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool starts_with_segment(const std::filesystem::path& path,
                                       std::string_view segment) {
    const auto first = path.begin();
    return first != path.end() && first->generic_string() == segment;
}

[[nodiscard]] core::Result<std::string> hash_file(const std::filesystem::path& path,
                                                  std::size_t maximum_bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::string>::failure(
            "asset_catalog.hash_failed", "failed to read asset for hashing: " + path.string());
    }

    core::StableHash64 hasher;
    std::size_t total_bytes = 0;
    char buffer[4096]{};
    while (input) {
        input.read(buffer, sizeof(buffer));
        const auto read_count = input.gcount();
        if (read_count > 0) {
            const auto byte_count = static_cast<std::size_t>(read_count);
            if (total_bytes > maximum_bytes || byte_count > maximum_bytes - total_bytes) {
                return core::Result<std::string>::failure(
                    "asset_catalog.file_too_large",
                    "asset exceeds the catalog hashing byte limit: " + path.string());
            }
            hasher.add_bytes(std::span{
                reinterpret_cast<const std::uint8_t*>(buffer),
                byte_count,
            });
            total_bytes += byte_count;
        }
    }
    if (input.bad() || (!input.eof() && input.fail())) {
        return core::Result<std::string>::failure("asset_catalog.hash_failed",
                                                  "failed while hashing asset: " + path.string());
    }

    return core::Result<std::string>::success(hasher.hex());
}

} // namespace

bool AssetCatalogBuildResult::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const modding::ModDiagnostic& diagnostic) {
        return diagnostic.severity == modding::DiagnosticSeverity::error;
    });
}

core::Status AssetCatalog::add(AssetRecord record) {
    if (!is_valid_logical_id(record.logical_id)) {
        return core::Status::failure("asset_catalog.invalid_logical_id",
                                     "asset logical id must be a safe relative path");
    }
    if (!core::is_valid_namespace_id(record.virtual_path.namespace_id)) {
        return core::Status::failure("asset_catalog.invalid_namespace",
                                     "asset virtual path namespace is invalid");
    }
    if (record.logical_id != asset_logical_id(record.virtual_path)) {
        return core::Status::failure("asset_catalog.logical_path_mismatch",
                                     "asset logical id must equal its namespaced virtual path");
    }
    if (record.source_id.empty()) {
        return core::Status::failure("asset_catalog.missing_source",
                                     "asset record needs a source id");
    }
    if (!core::is_valid_namespace_id(record.source_id)) {
        return core::Status::failure("asset_catalog.invalid_source",
                                     "asset source id must be a safe namespace identifier");
    }

    const auto duplicate = std::ranges::find_if(records_, [&record](const AssetRecord& existing) {
        return existing.logical_id == record.logical_id &&
               existing.source_kind == record.source_kind && existing.source_id == record.source_id;
    });
    if (duplicate != records_.end()) {
        return core::Status::failure("asset_catalog.duplicate_asset",
                                     "duplicate asset logical id in same source: " +
                                         record.logical_id);
    }

    const auto existing = active_by_logical_id_.find(record.logical_id);
    if (existing != active_by_logical_id_.end()) {
        const auto& active = records_[existing->second];
        if (active.priority == record.priority && active.source_id != record.source_id) {
            return core::Status::failure(
                "asset_catalog.ambiguous_priority",
                "asset override from different sources requires an explicit priority: " +
                    record.logical_id);
        }
    }

    records_.push_back(std::move(record));
    const auto new_index = records_.size() - 1;
    const auto active = active_by_logical_id_.find(records_[new_index].logical_id);
    if (active == active_by_logical_id_.end() ||
        records_[new_index].priority >= records_[active->second].priority) {
        active_by_logical_id_[records_[new_index].logical_id] = new_index;
    }

    return core::Status::ok();
}

core::Status AssetCatalog::set_dependencies(std::string_view logical_id,
                                            AssetSourceKind source_kind, std::string_view source_id,
                                            std::vector<VirtualPath> dependencies) {
    const auto record = std::ranges::find_if(records_, [&](const AssetRecord& candidate) {
        return candidate.logical_id == logical_id && candidate.source_kind == source_kind &&
               candidate.source_id == source_id;
    });
    if (record == records_.end()) {
        return core::Status::failure(
            "asset_catalog.missing_dependency_owner",
            "cannot assign dependencies to a missing asset catalog record");
    }
    std::ranges::sort(dependencies, {}, &VirtualPath::to_string);
    const auto duplicate = std::ranges::adjacent_find(
        dependencies, [](const VirtualPath& left, const VirtualPath& right) {
            return left.to_string() == right.to_string();
        });
    if (duplicate != dependencies.end()) {
        return core::Status::failure("asset_catalog.duplicate_dependency",
                                     "asset dependencies must be unique within a catalog record");
    }
    record->dependencies = std::move(dependencies);
    return core::Status::ok();
}

const AssetRecord* AssetCatalog::find_active(std::string_view logical_id) const noexcept {
    const auto found = active_by_logical_id_.find(std::string(logical_id));
    if (found == active_by_logical_id_.end()) {
        return nullptr;
    }
    return &records_[found->second];
}

std::vector<const AssetRecord*> AssetCatalog::records() const {
    std::vector<const AssetRecord*> result;
    result.reserve(records_.size());
    for (const auto& record : records_) {
        result.push_back(&record);
    }
    return result;
}

std::vector<const AssetRecord*> AssetCatalog::active_records() const {
    std::vector<const AssetRecord*> result;
    result.reserve(active_by_logical_id_.size());
    for (const auto& record : records_) {
        if (find_active(record.logical_id) == &record) {
            result.push_back(&record);
        }
    }
    return result;
}

std::vector<const AssetRecord*> AssetCatalog::records_for(std::string_view logical_id) const {
    std::vector<const AssetRecord*> result;
    for (const auto& record : records_) {
        if (record.logical_id == logical_id) {
            result.push_back(&record);
        }
    }
    return result;
}

std::size_t AssetCatalog::record_count() const noexcept {
    return records_.size();
}

std::size_t AssetCatalog::active_count() const noexcept {
    return active_by_logical_id_.size();
}

std::size_t AssetCatalog::count_kind(AssetKind kind) const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        records_, [kind](const AssetRecord& record) { return record.kind == kind; }));
}

AssetCatalogBuildResult
AssetCatalogBuilder::index_directory(AssetCatalog& catalog, const std::filesystem::path& root,
                                     std::string namespace_id, AssetSourceKind source_kind,
                                     std::string source_id, std::uint32_t priority,
                                     std::size_t maximum_file_bytes) {
    AssetCatalogBuildResult result;

    if (!core::is_valid_namespace_id(namespace_id)) {
        result.diagnostics.push_back(modding::ModDiagnostic{modding::DiagnosticSeverity::error,
                                                            root, "asset_catalog.invalid_namespace",
                                                            "asset namespace is invalid"});
        return result;
    }
    if (!std::filesystem::is_directory(root)) {
        result.diagnostics.push_back(modding::ModDiagnostic{modding::DiagnosticSeverity::warning,
                                                            root, "asset_catalog.root_missing",
                                                            "asset directory does not exist"});
        return result;
    }

    VirtualFileSystem vfs;
    auto mounted = vfs.mount(namespace_id, root);
    if (!mounted) {
        result.diagnostics.push_back(modding::ModDiagnostic{modding::DiagnosticSeverity::error,
                                                            root, mounted.error().code,
                                                            mounted.error().message});
        return result;
    }

    return index_virtual_namespace(catalog, vfs, std::move(namespace_id), source_kind,
                                   std::move(source_id), priority, maximum_file_bytes);
}

AssetCatalogBuildResult
AssetCatalogBuilder::index_virtual_namespace(AssetCatalog& catalog, const VirtualFileSystem& vfs,
                                             std::string namespace_id, AssetSourceKind source_kind,
                                             std::string source_id, std::uint32_t priority,
                                             std::size_t maximum_file_bytes) {
    AssetCatalogBuildResult result;

    if (maximum_file_bytes == 0) {
        result.diagnostics.push_back(modding::ModDiagnostic{
            modding::DiagnosticSeverity::error, std::filesystem::path{namespace_id},
            "asset_catalog.invalid_file_limit", "asset catalog file byte limit must be non-zero"});
        return result;
    }

    auto entries = vfs.list_namespace_files(namespace_id);
    if (!entries) {
        result.diagnostics.push_back(modding::ModDiagnostic{
            modding::DiagnosticSeverity::error, std::filesystem::path{namespace_id},
            entries.error().code, entries.error().message});
        return result;
    }

    auto staged_catalog = catalog;
    for (const auto& entry : entries.value()) {
        const auto logical_id = asset_logical_id(entry.virtual_path);
        auto hash = hash_file(entry.resolved_path, maximum_file_bytes);
        if (!hash) {
            result.diagnostics.push_back(
                modding::ModDiagnostic{modding::DiagnosticSeverity::error, entry.resolved_path,
                                       hash.error().code, hash.error().message});
            continue;
        }

        auto status = staged_catalog.add(AssetRecord{
            logical_id,
            infer_asset_kind(entry.virtual_path.relative_path),
            entry.virtual_path,
            source_kind,
            source_id,
            priority,
            entry.resolved_path,
            hash.value(),
            false,
            {},
        });
        if (!status) {
            result.diagnostics.push_back(
                modding::ModDiagnostic{modding::DiagnosticSeverity::error, entry.resolved_path,
                                       status.error().code, status.error().message});
        }
    }

    if (!result.has_errors()) {
        catalog = std::move(staged_catalog);
    }

    return result;
}

AssetCatalogBuildResult AssetCatalogBuilder::index_virtual_directory(
    AssetCatalog& catalog, const VirtualFileSystem& vfs, std::string_view virtual_directory,
    AssetSourceKind source_kind, std::string source_id, std::uint32_t priority,
    std::size_t maximum_file_bytes) {
    AssetCatalogBuildResult result;

    if (maximum_file_bytes == 0) {
        result.diagnostics.push_back(modding::ModDiagnostic{
            modding::DiagnosticSeverity::error,
            std::filesystem::path{std::string(virtual_directory)},
            "asset_catalog.invalid_file_limit", "asset catalog file byte limit must be non-zero"});
        return result;
    }

    auto entries = vfs.list_files(virtual_directory);
    if (!entries) {
        result.diagnostics.push_back(
            modding::ModDiagnostic{modding::DiagnosticSeverity::error,
                                   std::filesystem::path{std::string(virtual_directory)},
                                   entries.error().code, entries.error().message});
        return result;
    }

    auto staged_catalog = catalog;
    for (const auto& entry : entries.value()) {
        const auto logical_id = asset_logical_id(entry.virtual_path);
        auto hash = hash_file(entry.resolved_path, maximum_file_bytes);
        if (!hash) {
            result.diagnostics.push_back(
                modding::ModDiagnostic{modding::DiagnosticSeverity::error, entry.resolved_path,
                                       hash.error().code, hash.error().message});
            continue;
        }

        auto status = staged_catalog.add(AssetRecord{
            logical_id,
            infer_asset_kind(entry.virtual_path.relative_path),
            entry.virtual_path,
            source_kind,
            source_id,
            priority,
            entry.resolved_path,
            hash.value(),
            false,
            {},
        });
        if (!status) {
            result.diagnostics.push_back(
                modding::ModDiagnostic{modding::DiagnosticSeverity::error, entry.resolved_path,
                                       status.error().code, status.error().message});
        }
    }

    if (!result.has_errors()) {
        catalog = std::move(staged_catalog);
    }

    return result;
}

std::string asset_logical_id(const VirtualPath& path) {
    return path.namespace_id + ":" + path.relative_path.generic_string();
}

core::Result<std::filesystem::path> asset_logical_path(std::string_view logical_id) {
    auto parsed = core::PrototypeId::parse(logical_id);
    if (!parsed)
        return core::Result<std::filesystem::path>::failure(
            "asset_catalog.invalid_logical_id", "asset logical id is not namespace:path");
    return core::Result<std::filesystem::path>::success(
        std::filesystem::path(parsed->namespace_id()) / parsed->local_id());
}

std::string_view asset_kind_name(AssetKind kind) noexcept {
    switch (kind) {
    case AssetKind::texture:
        return "texture";
    case AssetKind::model:
        return "model";
    case AssetKind::shader:
        return "shader";
    case AssetKind::material:
        return "material";
    case AssetKind::sound:
        return "sound";
    case AssetKind::music:
        return "music";
    case AssetKind::font:
        return "font";
    case AssetKind::localization:
        return "localization";
    case AssetKind::ui:
        return "ui";
    case AssetKind::data:
        return "data";
    case AssetKind::unknown:
        return "unknown";
    }
    return "unknown";
}

std::string_view asset_source_kind_name(AssetSourceKind kind) noexcept {
    switch (kind) {
    case AssetSourceKind::mod:
        return "mod";
    case AssetSourceKind::resource_pack:
        return "resource_pack";
    case AssetSourceKind::engine:
        return "engine";
    }
    return "unknown";
}

AssetKind infer_asset_kind(const std::filesystem::path& relative_path) {
    const auto extension = lower_ascii(relative_path.extension().generic_string());
    if (extension == ".bin") {
        return AssetKind::data;
    }
    if (starts_with_segment(relative_path, "textures") || extension == ".png" ||
        extension == ".ktx2" || extension == ".jpg" || extension == ".jpeg") {
        return AssetKind::texture;
    }
    if (starts_with_segment(relative_path, "models") || extension == ".glb" ||
        extension == ".gltf") {
        return AssetKind::model;
    }
    if (starts_with_segment(relative_path, "shaders") || extension == ".slang" ||
        extension == ".hlsl" || extension == ".spv") {
        return AssetKind::shader;
    }
    if (starts_with_segment(relative_path, "materials")) {
        return AssetKind::material;
    }
    if (starts_with_segment(relative_path, "music")) {
        return AssetKind::music;
    }
    if (starts_with_segment(relative_path, "sounds")) {
        return AssetKind::sound;
    }
    if (extension == ".ogg" || extension == ".flac") {
        return AssetKind::music;
    }
    if (extension == ".wav") {
        return AssetKind::sound;
    }
    if (starts_with_segment(relative_path, "fonts") || extension == ".ttf" || extension == ".otf") {
        return AssetKind::font;
    }
    if (starts_with_segment(relative_path, "locale")) {
        return AssetKind::localization;
    }
    if (starts_with_segment(relative_path, "ui")) {
        return AssetKind::ui;
    }
    if (starts_with_segment(relative_path, "data") || extension == ".json" ||
        extension == ".toml") {
        return AssetKind::data;
    }
    return AssetKind::unknown;
}

core::Status discover_asset_dependencies(AssetCatalog& catalog) {
    struct Discovered {
        std::string logical_id;
        AssetSourceKind source_kind = AssetSourceKind::mod;
        std::string source_id;
        std::vector<VirtualPath> dependencies;
    };
    std::vector<Discovered> discovered;
    for (const auto* record : catalog.records()) {
        if (record->kind != AssetKind::model) {
            continue;
        }
        const auto extension = lower_ascii(record->source_path.extension().generic_string());
        if (extension != ".gltf" && extension != ".glb") {
            continue;
        }
        auto external = discover_gltf_external_dependencies(record->source_path);
        if (!external) {
            return core::Status::failure(external.error().code,
                                         "failed to discover dependencies for " +
                                             record->logical_id + ": " + external.error().message);
        }
        Discovered entry{record->logical_id, record->source_kind, record->source_id, {}};
        entry.dependencies.reserve(external.value().size());
        for (const auto& relative : external.value()) {
            const auto combined =
                (record->virtual_path.relative_path.parent_path() / relative).lexically_normal();
            auto virtual_path = VirtualPath::parse(record->virtual_path.namespace_id + ":" +
                                                   combined.generic_string());
            if (!virtual_path) {
                return core::Status::failure(
                    "asset_catalog.invalid_discovered_dependency",
                    "model dependency escapes its asset namespace or has an invalid path: " +
                        record->logical_id + " -> " + relative.generic_string());
            }
            entry.dependencies.push_back(std::move(virtual_path).value());
        }
        discovered.push_back(std::move(entry));
    }
    for (auto& entry : discovered) {
        auto status = catalog.set_dependencies(entry.logical_id, entry.source_kind, entry.source_id,
                                               std::move(entry.dependencies));
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Result<AssetCatalog>
select_asset_dependency_closure(const AssetCatalog& catalog,
                                std::span<const std::string> logical_ids) {
    AssetCatalog filtered;
    std::vector<std::string> pending(logical_ids.begin(), logical_ids.end());
    std::unordered_set<std::string> included;
    while (!pending.empty()) {
        auto logical_id = std::move(pending.back());
        pending.pop_back();
        if (!included.insert(logical_id).second) {
            continue;
        }
        const auto* record = catalog.find_active(logical_id);
        if (record == nullptr) {
            return core::Result<AssetCatalog>::failure(
                "asset_catalog.missing_selected_dependency",
                "selected asset or dependency is not active: " + logical_id);
        }
        auto status = filtered.add(*record);
        if (!status) {
            return core::Result<AssetCatalog>::failure(status.error().code, status.error().message);
        }
        for (const auto& dependency : record->dependencies) {
            pending.push_back(asset_logical_id(dependency));
        }
    }
    return core::Result<AssetCatalog>::success(std::move(filtered));
}

} // namespace heartstead::assets
