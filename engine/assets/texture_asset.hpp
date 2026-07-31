#pragma once

#include "engine/assets/image_asset.hpp"
#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace heartstead::assets {

enum class TextureRole : std::uint8_t {
    color,
    normal,
    metallic_roughness,
    occlusion,
    emissive,
    mask,
    user_interface,
    data,
};

enum class TextureAssetColorSpace : std::uint8_t {
    linear,
    srgb,
};

enum class TextureAssetFormat : std::uint8_t {
    rgba8,
    bc5_rg,
    bc7_rgba,
};

enum class TextureCompressionMode : std::uint8_t {
    automatic,
    rgba8,
    bc5,
    bc7,
};

struct TextureCookSettings {
    TextureRole role = TextureRole::color;
    TextureAssetColorSpace color_space = TextureAssetColorSpace::srgb;
    TextureCompressionMode compression = TextureCompressionMode::automatic;
    bool generate_mips = true;
    bool preserve_alpha_coverage = false;
    float alpha_cutoff = 0.5F;

    friend bool operator==(const TextureCookSettings&, const TextureCookSettings&) = default;
};

struct TextureMipLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> bytes;

    friend bool operator==(const TextureMipLevel&, const TextureMipLevel&) = default;
};

struct TextureAsset {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureRole role = TextureRole::color;
    TextureAssetColorSpace color_space = TextureAssetColorSpace::srgb;
    TextureAssetFormat format = TextureAssetFormat::bc7_rgba;
    bool alpha_coverage_preserved = false;
    float alpha_cutoff = 0.5F;
    std::vector<TextureMipLevel> mips;

    [[nodiscard]] std::size_t gpu_memory_bytes() const noexcept;

    friend bool operator==(const TextureAsset&, const TextureAsset&) = default;
};

struct TextureAssetLimits {
    std::uint32_t maximum_dimension = 16'384;
    std::uint32_t maximum_mip_levels = 32;
    std::size_t maximum_payload_bytes = 512U * 1024U * 1024U;

    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] TextureCookSettings
infer_texture_cook_settings(const std::filesystem::path& source_path) noexcept;
[[nodiscard]] core::Result<TextureCookSettings>
load_texture_cook_settings(const std::filesystem::path& source_path);
[[nodiscard]] std::filesystem::path
texture_cook_sidecar_path(const std::filesystem::path& source_path);

[[nodiscard]] core::Result<TextureAsset> cook_texture_asset(const ImageAsset& source,
                                                            const TextureCookSettings& settings,
                                                            const TextureAssetLimits& limits = {});
[[nodiscard]] core::Status validate_texture_asset(const TextureAsset& asset,
                                                  const TextureAssetLimits& limits = {});
[[nodiscard]] core::Result<std::vector<std::uint8_t>>
encode_texture_asset(const TextureAsset& asset, const TextureAssetLimits& limits = {});
[[nodiscard]] core::Result<TextureAsset>
decode_texture_asset(std::span<const std::uint8_t> bytes, const TextureAssetLimits& limits = {});

[[nodiscard]] std::string_view texture_role_name(TextureRole role) noexcept;
[[nodiscard]] std::string_view
texture_color_space_name(TextureAssetColorSpace color_space) noexcept;
[[nodiscard]] std::string_view texture_asset_format_name(TextureAssetFormat format) noexcept;

} // namespace heartstead::assets
