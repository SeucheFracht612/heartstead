#include "engine/renderer/assets/surface_texture_array.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace heartstead::renderer {

namespace {

[[nodiscard]] std::vector<std::byte> solid_layer(std::uint32_t width, std::uint32_t height,
                                                 std::array<std::uint8_t, 4> color) {
    std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4U) {
        for (std::size_t channel = 0; channel < color.size(); ++channel) {
            pixels[offset + channel] = static_cast<std::byte>(color[channel]);
        }
    }
    return pixels;
}

[[nodiscard]] std::vector<std::byte> error_layer(std::uint32_t width, std::uint32_t height) {
    auto pixels = solid_layer(width, height, {255, 0, 255, 255});
    constexpr std::uint32_t checker_size = 32;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (((x / checker_size) + (y / checker_size)) % 2U == 0) {
                continue;
            }
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[offset] = std::byte{16};
            pixels[offset + 2U] = std::byte{16};
        }
    }
    return pixels;
}

[[nodiscard]] float srgb_to_linear(float value) noexcept {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb(float value) noexcept {
    return value <= 0.0031308F ? value * 12.92F
                               : 1.055F * std::pow(std::max(value, 0.0F), 1.0F / 2.4F) - 0.055F;
}

} // namespace

core::Status SurfaceTextureArrayConfig::validate() const {
    if (layer_width == 0 || layer_height == 0 || maximum_layers < 4 || layer_width > 4096 ||
        layer_height > 4096) {
        return core::Status::failure(
            "surface_texture_array.invalid_config",
            "surface texture layers must be 1-4096 pixels and allow four fallback layers");
    }
    const auto bytes = static_cast<std::uint64_t>(layer_width) * layer_height * maximum_layers * 4U;
    if (bytes > 512ULL * 1024ULL * 1024ULL) {
        return core::Status::failure(
            "surface_texture_array.excessive_capacity",
            "configured surface texture array exceeds the 512 MiB base-level limit");
    }
    return core::Status::ok();
}

SurfaceTextureArray::SurfaceTextureArray(TextureManager& textures) : textures_(&textures) {}

core::Status SurfaceTextureArray::initialize(SurfaceTextureArrayConfig config) {
    if (initialized_) {
        return core::Status::failure("surface_texture_array.already_initialized",
                                     "surface texture array cannot be initialized twice");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    config_ = config;
    layers_ = TerrainTextureArrayBuilder(config.layer_width, config.layer_height);
    const auto add_fallback = [&](std::string id, std::vector<std::byte> pixels) -> core::Status {
        auto layer = layers_.add_layer(std::move(id), pixels);
        if (!layer) {
            return core::Status::failure(layer.error().code, layer.error().message);
        }
        return core::Status::ok();
    };
    status = add_fallback("__surface_error", error_layer(config.layer_width, config.layer_height));
    if (!status) {
        return status;
    }
    status = add_fallback("__surface_white", solid_layer(config.layer_width, config.layer_height,
                                                         {255, 255, 255, 255}));
    if (!status) {
        return status;
    }
    status = add_fallback("__surface_normal", solid_layer(config.layer_width, config.layer_height,
                                                          {128, 128, 255, 255}));
    if (!status) {
        return status;
    }
    status = add_fallback("__surface_black",
                          solid_layer(config.layer_width, config.layer_height, {0, 0, 0, 255}));
    if (!status) {
        return status;
    }
    revision_ = 1;
    initialized_ = true;
    status = synchronize();
    if (!status) {
        (void)shutdown();
    }
    return status;
}

core::Result<std::uint32_t> SurfaceTextureArray::add(std::string id, std::uint32_t width,
                                                     std::uint32_t height,
                                                     std::span<const std::uint8_t> rgba8) {
    if (!initialized_) {
        return core::Result<std::uint32_t>::failure(
            "surface_texture_array.not_initialized",
            "surface texture array must be initialized before adding an image");
    }
    if (id.empty() || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height * 4U != rgba8.size()) {
        return core::Result<std::uint32_t>::failure(
            "surface_texture_array.invalid_image",
            "surface image requires an id, nonzero extent, and matching RGBA8 pixels");
    }
    std::uint32_t existing_layer = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t index = 0; index < layers_.layer_count(); ++index) {
        if (const auto* layer_id = layers_.layer_id(index);
            layer_id != nullptr && *layer_id == id) {
            existing_layer = index;
            break;
        }
    }
    if (existing_layer == std::numeric_limits<std::uint32_t>::max() &&
        layers_.layer_count() >= config_.maximum_layers) {
        return core::Result<std::uint32_t>::failure(
            "surface_texture_array.full",
            "surface texture array has reached its configured layer limit");
    }
    auto resized =
        resize_surface_rgba8(width, height, rgba8, config_.layer_width, config_.layer_height);
    auto layer = layers_.add_layer(std::move(id), resized);
    if (!layer) {
        return layer;
    }
    if (existing_layer == std::numeric_limits<std::uint32_t>::max()) {
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
            return core::Result<std::uint32_t>::failure("surface_texture_array.revision_overflow",
                                                        "surface texture array revision overflow");
        }
        ++revision_;
    }
    return layer;
}

core::Status SurfaceTextureArray::synchronize() {
    if (!initialized_) {
        return core::Status::failure("surface_texture_array.not_initialized",
                                     "surface texture array must be initialized first");
    }
    if (synchronized_revision_ == revision_) {
        return core::Status::ok();
    }
    auto upload = layers_.build("__surface_texture_array", TextureColorSpace::srgb, true);
    if (!upload) {
        return core::Status::failure(upload.error().code, upload.error().message);
    }
    core::Status status = core::Status::ok();
    if (!texture_.is_valid()) {
        auto created = textures_->create_texture(std::move(upload).value());
        if (!created) {
            return core::Status::failure(created.error().code, created.error().message);
        }
        texture_ = created.value();
    } else {
        status = textures_->replace_texture(texture_, std::move(upload).value());
        if (!status) {
            return status;
        }
    }
    synchronized_revision_ = revision_;
    return core::Status::ok();
}

core::Status SurfaceTextureArray::shutdown() {
    auto status = core::Status::ok();
    if (texture_.is_valid()) {
        status = textures_->release_texture(texture_);
    }
    texture_ = {};
    revision_ = 0;
    synchronized_revision_ = 0;
    initialized_ = false;
    layers_ = TerrainTextureArrayBuilder(1, 1);
    return status;
}

TextureHandle SurfaceTextureArray::texture() const noexcept {
    return texture_;
}

const TextureView* SurfaceTextureArray::texture_view() const noexcept {
    return textures_ == nullptr ? nullptr : textures_->find(texture_);
}

SurfaceTextureArrayFallbacks SurfaceTextureArray::fallbacks() const noexcept {
    return {};
}

std::size_t SurfaceTextureArray::layer_count() const noexcept {
    return layers_.layer_count();
}

std::uint64_t SurfaceTextureArray::revision() const noexcept {
    return revision_;
}

std::vector<std::byte> resize_surface_rgba8(std::uint32_t source_width, std::uint32_t source_height,
                                            std::span<const std::uint8_t> source,
                                            std::uint32_t target_width,
                                            std::uint32_t target_height) {
    std::vector<std::byte> result(static_cast<std::size_t>(target_width) * target_height * 4U);
    for (std::uint32_t target_y = 0; target_y < target_height; ++target_y) {
        const auto source_y = (static_cast<float>(target_y) + 0.5F) *
                                  static_cast<float>(source_height) /
                                  static_cast<float>(target_height) -
                              0.5F;
        const auto y0 = static_cast<std::uint32_t>(
            std::clamp(std::floor(source_y), 0.0F, static_cast<float>(source_height - 1U)));
        const auto y1 = std::min(y0 + 1U, source_height - 1U);
        const auto fy = std::clamp(source_y - std::floor(source_y), 0.0F, 1.0F);
        for (std::uint32_t target_x = 0; target_x < target_width; ++target_x) {
            const auto source_x = (static_cast<float>(target_x) + 0.5F) *
                                      static_cast<float>(source_width) /
                                      static_cast<float>(target_width) -
                                  0.5F;
            const auto x0 = static_cast<std::uint32_t>(
                std::clamp(std::floor(source_x), 0.0F, static_cast<float>(source_width - 1U)));
            const auto x1 = std::min(x0 + 1U, source_width - 1U);
            const auto fx = std::clamp(source_x - std::floor(source_x), 0.0F, 1.0F);
            const auto sample = [&](std::uint32_t x, std::uint32_t y,
                                    std::size_t channel) noexcept {
                const auto offset = (static_cast<std::size_t>(y) * source_width + x) * 4U + channel;
                auto value = static_cast<float>(source[offset]) / 255.0F;
                return channel < 3U ? srgb_to_linear(value) : value;
            };
            const auto output = (static_cast<std::size_t>(target_y) * target_width + target_x) * 4U;
            for (std::size_t channel = 0; channel < 4U; ++channel) {
                const auto top = std::lerp(sample(x0, y0, channel), sample(x1, y0, channel), fx);
                const auto bottom = std::lerp(sample(x0, y1, channel), sample(x1, y1, channel), fx);
                auto value = std::lerp(top, bottom, fy);
                if (channel < 3U) {
                    value = linear_to_srgb(value);
                }
                result[output + channel] = static_cast<std::byte>(
                    static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F)));
            }
        }
    }
    return result;
}

} // namespace heartstead::renderer
