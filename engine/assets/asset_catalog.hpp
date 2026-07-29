#pragma once

#include "engine/assets/virtual_file_system.hpp"
#include "engine/core/result.hpp"
#include "engine/modding/mod_diagnostic.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::assets {

inline constexpr std::size_t default_maximum_asset_source_bytes = 256U * 1024U * 1024U;

enum class AssetKind {
    texture,
    model,
    shader,
    material,
    sound,
    music,
    font,
    localization,
    ui,
    data,
    unknown,
};

enum class AssetSourceKind {
    mod,
    resource_pack,
    engine,
};

struct AssetRecord {
    std::string logical_id;
    AssetKind kind = AssetKind::unknown;
    VirtualPath virtual_path;
    AssetSourceKind source_kind = AssetSourceKind::mod;
    std::string source_id;
    std::uint32_t priority = 0;
    std::filesystem::path source_path;
    std::string content_hash;
    bool cooked = false;
    std::vector<VirtualPath> dependencies;
};

struct AssetCatalogBuildResult {
    std::vector<modding::ModDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

class AssetCatalog {
  public:
    [[nodiscard]] core::Status add(AssetRecord record);
    [[nodiscard]] core::Status set_dependencies(std::string_view logical_id,
                                                AssetSourceKind source_kind,
                                                std::string_view source_id,
                                                std::vector<VirtualPath> dependencies);

    [[nodiscard]] const AssetRecord* find_active(std::string_view logical_id) const noexcept;
    [[nodiscard]] std::vector<const AssetRecord*> records() const;
    [[nodiscard]] std::vector<const AssetRecord*> active_records() const;
    [[nodiscard]] std::vector<const AssetRecord*> records_for(std::string_view logical_id) const;
    [[nodiscard]] std::size_t record_count() const noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] std::size_t count_kind(AssetKind kind) const noexcept;

  private:
    std::vector<AssetRecord> records_;
    std::unordered_map<std::string, std::size_t> active_by_logical_id_;
};

class AssetCatalogBuilder {
  public:
    [[nodiscard]] static AssetCatalogBuildResult
    index_directory(AssetCatalog& catalog, const std::filesystem::path& root,
                    std::string namespace_id, AssetSourceKind source_kind, std::string source_id,
                    std::uint32_t priority,
                    std::size_t maximum_file_bytes = default_maximum_asset_source_bytes);
    [[nodiscard]] static AssetCatalogBuildResult
    index_virtual_namespace(AssetCatalog& catalog, const VirtualFileSystem& vfs,
                            std::string namespace_id, AssetSourceKind source_kind,
                            std::string source_id, std::uint32_t priority,
                            std::size_t maximum_file_bytes = default_maximum_asset_source_bytes);
    [[nodiscard]] static AssetCatalogBuildResult
    index_virtual_directory(AssetCatalog& catalog, const VirtualFileSystem& vfs,
                            std::string_view virtual_directory, AssetSourceKind source_kind,
                            std::string source_id, std::uint32_t priority,
                            std::size_t maximum_file_bytes = default_maximum_asset_source_bytes);
};

[[nodiscard]] std::string_view asset_kind_name(AssetKind kind) noexcept;
[[nodiscard]] std::string_view asset_source_kind_name(AssetSourceKind kind) noexcept;
[[nodiscard]] AssetKind infer_asset_kind(const std::filesystem::path& relative_path);
[[nodiscard]] std::string asset_logical_id(const VirtualPath& path);
[[nodiscard]] core::Result<std::filesystem::path> asset_logical_path(std::string_view logical_id);
[[nodiscard]] core::Status discover_asset_dependencies(AssetCatalog& catalog);
[[nodiscard]] core::Result<AssetCatalog>
select_asset_dependency_closure(const AssetCatalog& catalog,
                                std::span<const std::string> logical_ids);

} // namespace heartstead::assets
