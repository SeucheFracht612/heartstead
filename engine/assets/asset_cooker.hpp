#pragma once

#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/cooked_asset_manifest.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace heartstead::assets {

enum class AssetCookBackend {
    development_passthrough,
    production_converters,
};

struct AssetCookBackendInfo {
    AssetCookBackend backend = AssetCookBackend::development_passthrough;
    std::string_view name;
    bool available = false;
    std::string_view status;
};

struct AssetCookPipelineInfo {
    AssetKind kind = AssetKind::unknown;
    AssetCookBackend backend = AssetCookBackend::development_passthrough;
    std::string_view name;
    bool available = false;
    bool converts_source_format = false;
    std::string_view status;
};

struct AssetCookConfig {
    AssetCookBackend backend = AssetCookBackend::development_passthrough;
    std::filesystem::path output_root;
    std::filesystem::path manifest_relative_path = "asset_manifest.txt";
    CookedAssetBuildConfig manifest_config;
    std::size_t maximum_source_bytes = default_maximum_asset_source_bytes;
    bool incremental = true;
    bool prune_stale_outputs = true;
};

struct AssetCookResult {
    AssetCookBackend backend = AssetCookBackend::development_passthrough;
    CookedAssetManifest manifest;
    std::filesystem::path manifest_path;
    std::size_t cooked_file_count = 0;
    std::size_t reused_file_count = 0;
    std::size_t invalidated_file_count = 0;
    std::size_t removed_file_count = 0;
    std::uintmax_t cooked_payload_bytes = 0;
};

class AssetCooker {
  public:
    [[nodiscard]] static core::Result<AssetCookResult> cook(const AssetCatalog& catalog,
                                                            AssetCookConfig config);
};

[[nodiscard]] core::Status validate_asset_cook_config(const AssetCookConfig& config);
[[nodiscard]] AssetCookBackendInfo asset_cook_backend_info(AssetCookBackend backend) noexcept;
[[nodiscard]] AssetCookPipelineInfo asset_cook_pipeline_info(AssetKind kind,
                                                             AssetCookBackend backend) noexcept;
[[nodiscard]] std::string_view asset_cook_backend_name(AssetCookBackend backend) noexcept;
[[nodiscard]] std::string_view asset_cook_backend_name(AssetKind kind) noexcept;
[[nodiscard]] std::string_view asset_cook_pipeline_name(
    AssetKind kind, AssetCookBackend backend = AssetCookBackend::development_passthrough) noexcept;

} // namespace heartstead::assets
