#include "engine/assets/image_asset.hpp"

#include <limits>
#include <string>

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

} // namespace heartstead::assets
