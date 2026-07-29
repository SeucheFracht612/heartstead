#include "engine/assets/image_asset.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>

#include <ktx.h>

#define STBI_FAILURE_USERMSG
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace heartstead::assets {

core::Status ImageAssetLimits::validate() const {
    if (maximum_dimension == 0 || maximum_decoded_bytes < 4) {
        return core::Status::failure("image_asset.invalid_limits",
                                     "image limits must allow a non-empty RGBA8 image");
    }
    return core::Status::ok();
}

core::Result<ImageAsset> decode_png_or_jpeg(std::span<const std::uint8_t> encoded,
                                            const ImageAssetLimits& limits) {
    auto status = limits.validate();
    if (!status) {
        return core::Result<ImageAsset>::failure(status.error().code, status.error().message);
    }
    if (encoded.empty() ||
        encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return core::Result<ImageAsset>::failure(
            "image_asset.invalid_source_size",
            "PNG or JPEG source is empty or exceeds the decoder input limit");
    }

    int width = 0;
    int height = 0;
    int source_channels = 0;
    const auto* source = encoded.data();
    const auto source_size = static_cast<int>(encoded.size());
    if (stbi_info_from_memory(source, source_size, &width, &height, &source_channels) == 0) {
        const auto* reason = stbi_failure_reason();
        return core::Result<ImageAsset>::failure(
            "image_asset.invalid_source",
            "source is not a supported PNG or JPEG image" +
                (reason == nullptr ? std::string{} : ": " + std::string(reason)));
    }
    if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) > limits.maximum_dimension ||
        static_cast<std::uint32_t>(height) > limits.maximum_dimension) {
        return core::Result<ImageAsset>::failure(
            "image_asset.dimension_limit",
            "decoded image dimensions are empty or exceed the configured limit");
    }

    const auto pixel_count = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    const auto byte_count = pixel_count * 4U;
    if (byte_count > limits.maximum_decoded_bytes ||
        byte_count > std::numeric_limits<std::size_t>::max()) {
        return core::Result<ImageAsset>::failure(
            "image_asset.decoded_size_limit",
            "decoded RGBA8 image exceeds the configured byte limit");
    }

    auto* decoded = stbi_load_from_memory(source, source_size, &width, &height, &source_channels,
                                          STBI_rgb_alpha);
    if (decoded == nullptr) {
        const auto* reason = stbi_failure_reason();
        return core::Result<ImageAsset>::failure(
            "image_asset.decode_failed",
            "failed to decode PNG or JPEG source" +
                (reason == nullptr ? std::string{} : ": " + std::string(reason)));
    }

    ImageAsset image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba8.assign(decoded, decoded + static_cast<std::size_t>(byte_count));
    stbi_image_free(decoded);
    return core::Result<ImageAsset>::success(std::move(image));
}

core::Result<ImageAsset> decode_ktx2(std::span<const std::uint8_t> encoded,
                                     const ImageAssetLimits& limits, bool require_gltf_basisu) {
    auto status = limits.validate();
    if (!status) {
        return core::Result<ImageAsset>::failure(status.error().code, status.error().message);
    }
    if (encoded.empty()) {
        return core::Result<ImageAsset>::failure("image_asset.invalid_source_size",
                                                 "KTX2 source is empty");
    }

    ktxTexture2* raw_texture = nullptr;
    const auto create_flags = require_gltf_basisu ? KTX_TEXTURE_CREATE_CHECK_GLTF_BASISU_BIT
                                                  : KTX_TEXTURE_CREATE_NO_FLAGS;
    const auto create_result =
        ktxTexture2_CreateFromMemory(encoded.data(), encoded.size(), create_flags, &raw_texture);
    if (create_result != KTX_SUCCESS || raw_texture == nullptr) {
        return core::Result<ImageAsset>::failure("image_asset.invalid_ktx2",
                                                 "source is not a supported KTX2 texture: " +
                                                     std::string(ktxErrorString(create_result)));
    }
    const auto destroy = [](ktxTexture2* texture) { ktxTexture2_Destroy(texture); };
    std::unique_ptr<ktxTexture2, decltype(destroy)> texture(raw_texture, destroy);
    const auto* base = reinterpret_cast<const ktxTexture*>(texture.get());
    if (base->numDimensions != 2 || base->isArray || base->numLayers != 1 || base->numFaces != 1 ||
        base->baseDepth != 1 || base->baseWidth == 0 || base->baseHeight == 0 ||
        base->numLevels == 0 || base->numLevels > 32 ||
        base->baseWidth > limits.maximum_dimension || base->baseHeight > limits.maximum_dimension) {
        return core::Result<ImageAsset>::failure(
            "image_asset.unsupported_ktx2_layout",
            "KTX2 model textures must be bounded, non-array two-dimensional images");
    }
    const auto byte_count = static_cast<std::uint64_t>(base->baseWidth) * base->baseHeight * 4U;
    if (byte_count > limits.maximum_decoded_bytes ||
        byte_count > std::numeric_limits<std::size_t>::max()) {
        return core::Result<ImageAsset>::failure(
            "image_asset.decoded_size_limit",
            "decoded KTX2 RGBA8 image exceeds the configured byte limit");
    }
    std::uint64_t all_level_bytes = 0;
    auto level_width = base->baseWidth;
    auto level_height = base->baseHeight;
    for (std::uint32_t level = 0; level < base->numLevels; ++level) {
        const auto level_bytes = static_cast<std::uint64_t>(level_width) * level_height * 4U;
        if (level_bytes > limits.maximum_decoded_bytes - all_level_bytes) {
            return core::Result<ImageAsset>::failure(
                "image_asset.decoded_size_limit",
                "transcoded KTX2 mip data exceeds the configured byte limit");
        }
        all_level_bytes += level_bytes;
        level_width = std::max(level_width / 2U, 1U);
        level_height = std::max(level_height / 2U, 1U);
    }
    const auto load_result = ktxTexture2_LoadImageData(texture.get(), nullptr, 0);
    if (load_result != KTX_SUCCESS) {
        return core::Result<ImageAsset>::failure("image_asset.ktx2_load_failed",
                                                 "failed to load bounded KTX2 image data: " +
                                                     std::string(ktxErrorString(load_result)));
    }
    if (ktxTexture2_NeedsTranscoding(texture.get())) {
        const auto transcode_result = ktxTexture2_TranscodeBasis(texture.get(), KTX_TTF_RGBA32, 0);
        if (transcode_result != KTX_SUCCESS) {
            return core::Result<ImageAsset>::failure(
                "image_asset.ktx2_transcode_failed",
                "failed to transcode Basis Universal KTX2 image: " +
                    std::string(ktxErrorString(transcode_result)));
        }
    } else {
        // Vulkan format values 37 and 43 are RGBA8 UNORM and RGBA8 SRGB.
        // Other KTX2 payload layouts cannot be copied into an RGBA8 asset
        // without an explicit format conversion.
        constexpr ktx_uint32_t rgba8_unorm = 37;
        constexpr ktx_uint32_t rgba8_srgb = 43;
        if (texture->vkFormat != rgba8_unorm && texture->vkFormat != rgba8_srgb) {
            return core::Result<ImageAsset>::failure(
                "image_asset.unsupported_ktx2_format",
                "KTX2 image is neither Basis Universal nor an RGBA8 texture");
        }
    }
    ktx_size_t image_offset = 0;
    const auto offset_result = ktxTexture2_GetImageOffset(texture.get(), 0, 0, 0, &image_offset);
    const auto* decoded_base = reinterpret_cast<const ktxTexture*>(texture.get());
    if (offset_result != KTX_SUCCESS || decoded_base->pData == nullptr ||
        image_offset > decoded_base->dataSize ||
        byte_count > decoded_base->dataSize - image_offset) {
        return core::Result<ImageAsset>::failure(
            "image_asset.invalid_ktx2_payload",
            "KTX2 base mip does not contain a complete RGBA8 image");
    }

    ImageAsset image;
    image.width = base->baseWidth;
    image.height = base->baseHeight;
    const auto* pixels = decoded_base->pData + image_offset;
    image.rgba8.assign(pixels, pixels + static_cast<std::size_t>(byte_count));
    return core::Result<ImageAsset>::success(std::move(image));
}

} // namespace heartstead::assets
