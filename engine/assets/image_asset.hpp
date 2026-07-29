#pragma once

#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::assets {

struct ImageAssetLimits {
    std::uint32_t maximum_dimension = 8'192;
    std::size_t maximum_decoded_bytes = 128U * 1024U * 1024U;

    [[nodiscard]] core::Status validate() const;
};

struct ImageAsset {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;

    friend bool operator==(const ImageAsset&, const ImageAsset&) = default;
};

[[nodiscard]] core::Result<ImageAsset> decode_png_or_jpeg(std::span<const std::uint8_t> encoded,
                                                          const ImageAssetLimits& limits = {});
[[nodiscard]] core::Result<ImageAsset> decode_ktx2(std::span<const std::uint8_t> encoded,
                                                   const ImageAssetLimits& limits = {},
                                                   bool require_gltf_basisu = false);

} // namespace heartstead::assets
