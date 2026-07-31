#include "engine/renderer/assets/texture_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] float srgb_to_linear(float value) noexcept {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb(float value) noexcept {
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint8_t byte_value(std::byte value) noexcept {
    return std::to_integer<std::uint8_t>(value);
}

[[nodiscard]] std::byte averaged_channel(const std::array<std::uint8_t, 4>& values,
                                         bool srgb) noexcept {
    float sum = 0.0F;
    for (const auto value : values) {
        auto normalized = static_cast<float>(value) / 255.0F;
        sum += srgb ? srgb_to_linear(normalized) : normalized;
    }
    auto result = sum * 0.25F;
    if (srgb) {
        result = linear_to_srgb(result);
    }
    result = std::clamp(result, 0.0F, 1.0F);
    return static_cast<std::byte>(static_cast<std::uint8_t>(std::lround(result * 255.0F)));
}

[[nodiscard]] TextureUploadDesc solid_texture(std::string id, std::array<std::uint8_t, 4> color) {
    TextureUploadDesc result;
    result.id = std::move(id);
    result.width = 2;
    result.height = 2;
    result.generate_mipmaps = true;
    result.rgba8.resize(2U * 2U * 4U);
    for (std::size_t offset = 0; offset < result.rgba8.size(); offset += 4) {
        for (std::size_t channel = 0; channel < color.size(); ++channel) {
            result.rgba8[offset + channel] = static_cast<std::byte>(color[channel]);
        }
    }
    return result;
}

[[nodiscard]] TextureUploadDesc error_checkerboard() {
    TextureUploadDesc result;
    result.id = "__error_checkerboard";
    result.width = 4;
    result.height = 4;
    result.generate_mipmaps = true;
    result.rgba8.resize(4U * 4U * 4U);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const bool magenta = ((x / 2U) + (y / 2U)) % 2U == 0;
            const auto offset = static_cast<std::size_t>(y * 4U + x) * 4U;
            result.rgba8[offset] = static_cast<std::byte>(magenta ? 255U : 16U);
            result.rgba8[offset + 1] = static_cast<std::byte>(0U);
            result.rgba8[offset + 2] = static_cast<std::byte>(magenta ? 255U : 16U);
            result.rgba8[offset + 3] = static_cast<std::byte>(255U);
        }
    }
    return result;
}

} // namespace

struct TextureManager::PreparedTexture {
    TextureUploadDesc source;
    rhi::RenderResourceHandle image;
    std::uint32_t mip_levels = 1;
    std::size_t resident_bytes = 0;
};

struct TextureManager::TextureRecord {
    std::uint32_t generation = 1;
    bool occupied = false;
    std::uint64_t revision = 0;
    PreparedTexture texture;
    TextureView view;
};

TextureManager::TextureManager(rhi::IRenderDevice& device) : device_(device) {}

TextureManager::~TextureManager() {
    (void)shutdown();
}

core::Status TextureManager::initialize_fallbacks() {
    if (error_texture_.is_valid()) {
        return core::Status::ok();
    }
    auto error = create_texture(error_checkerboard());
    if (!error) {
        return core::Status::failure(error.error().code, error.error().message);
    }
    error_texture_ = error.value();
    auto white = create_texture(solid_texture("__white", {255, 255, 255, 255}));
    if (!white) {
        (void)shutdown();
        return core::Status::failure(white.error().code, white.error().message);
    }
    white_texture_ = white.value();
    auto black = create_texture(solid_texture("__black", {0, 0, 0, 255}));
    if (!black) {
        (void)shutdown();
        return core::Status::failure(black.error().code, black.error().message);
    }
    black_texture_ = black.value();
    auto normal = create_texture(solid_texture("__normal", {128, 128, 255, 255}));
    if (!normal) {
        (void)shutdown();
        return core::Status::failure(normal.error().code, normal.error().message);
    }
    normal_texture_ = normal.value();
    return core::Status::ok();
}

core::Result<TextureHandle> TextureManager::create_texture(TextureUploadDesc desc) {
    if (find(desc.id) != nullptr) {
        return core::Result<TextureHandle>::failure("texture_manager.duplicate_texture",
                                                    "texture id is already resident: " + desc.id);
    }
    auto prepared = prepare(std::move(desc));
    if (!prepared) {
        return core::Result<TextureHandle>::failure(prepared.error().code,
                                                    prepared.error().message);
    }
    std::size_t slot = textures_.size();
    for (std::size_t index = 0; index < textures_.size(); ++index) {
        if (!textures_[index].occupied) {
            slot = index;
            break;
        }
    }
    if (slot == textures_.size()) {
        textures_.emplace_back();
    }
    auto& record = textures_[slot];
    record.occupied = true;
    record.revision = 1;
    record.texture = std::move(prepared).value();
    record.view.handle = {static_cast<std::uint32_t>(slot + 1), record.generation};
    refresh_view(record);
    stats_.uploaded_texture_bytes += record.texture.resident_bytes;
    update_stats();
    return core::Result<TextureHandle>::success(record.view.handle);
}

core::Status TextureManager::replace_texture(TextureHandle handle, TextureUploadDesc desc) {
    auto* record = find_record(handle);
    if (record == nullptr) {
        return core::Status::failure("texture_manager.stale_texture_handle",
                                     "texture replacement references a stale handle");
    }
    if (record->texture.source.id != desc.id) {
        return core::Status::failure("texture_manager.reload_id_mismatch",
                                     "texture replacement id must match the resident texture");
    }
    auto replacement = prepare(std::move(desc));
    if (!replacement) {
        ++stats_.failed_reload_count;
        return core::Status::failure(replacement.error().code, replacement.error().message);
    }
    const auto old_image = record->texture.image;
    record->texture = std::move(replacement).value();
    if (record->revision == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++record->revision;
    ++stats_.successful_reload_count;
    stats_.uploaded_texture_bytes += record->texture.resident_bytes;
    refresh_view(*record);
    update_stats();
    return device_.release_resource(old_image);
}

core::Status TextureManager::release_texture(TextureHandle handle) {
    if (handle == error_texture_ || handle == white_texture_ || handle == black_texture_ ||
        handle == normal_texture_) {
        return core::Status::failure("texture_manager.fallback_release_rejected",
                                     "fallback textures remain resident until manager shutdown");
    }
    auto* record = find_record(handle);
    if (record == nullptr) {
        return core::Status::failure("texture_manager.stale_texture_handle",
                                     "texture release references a stale handle");
    }
    auto status = device_.release_resource(record->texture.image);
    record->occupied = false;
    record->revision = 0;
    record->texture = {};
    record->view = {};
    if (record->generation == std::numeric_limits<std::uint32_t>::max()) {
        std::terminate();
    }
    ++record->generation;
    update_stats();
    return status;
}

core::Status TextureManager::shutdown() {
    core::Status first_failure = core::Status::ok();
    for (auto& record : textures_) {
        if (!record.occupied) {
            continue;
        }
        auto status = device_.release_resource(record.texture.image);
        if (!status && first_failure) {
            first_failure = status;
        }
        record = {};
    }
    textures_.clear();
    error_texture_ = {};
    white_texture_ = {};
    black_texture_ = {};
    normal_texture_ = {};
    update_stats();
    return first_failure;
}

const TextureView* TextureManager::find(TextureHandle handle) const noexcept {
    const auto* record = find_record(handle);
    return record == nullptr ? nullptr : &record->view;
}

const TextureView* TextureManager::find(std::string_view id) const noexcept {
    const auto found = std::ranges::find_if(textures_, [id](const TextureRecord& record) {
        return record.occupied && record.texture.source.id == id;
    });
    return found == textures_.end() ? nullptr : &found->view;
}

const TextureView& TextureManager::resolve_or_error(TextureHandle handle) const noexcept {
    if (const auto* texture = find(handle); texture != nullptr) {
        return *texture;
    }
    if (const auto* fallback = find(error_texture_); fallback != nullptr) {
        return *fallback;
    }
    static const TextureView unavailable{};
    return unavailable;
}

TextureHandle TextureManager::error_texture() const noexcept {
    return error_texture_;
}

TextureHandle TextureManager::white_texture() const noexcept {
    return white_texture_;
}

TextureHandle TextureManager::black_texture() const noexcept {
    return black_texture_;
}

TextureHandle TextureManager::normal_texture() const noexcept {
    return normal_texture_;
}

const TextureManagerStats& TextureManager::stats() const noexcept {
    return stats_;
}

core::Result<TextureManager::PreparedTexture>
TextureManager::prepare(TextureUploadDesc desc) const {
    auto status = validate_texture_upload_desc(desc);
    if (!status) {
        return core::Result<PreparedTexture>::failure(status.error().code, status.error().message);
    }
    PreparedTexture result;
    result.source = std::move(desc);
    const auto is_cooked = !result.source.cooked_bytes.empty();
    result.mip_levels = is_cooked ? result.source.cooked_mip_levels
                        : result.source.generate_mipmaps
                            ? complete_mip_level_count(result.source.width, result.source.height)
                            : 1;
    auto upload_bytes =
        is_cooked ? result.source.cooked_bytes
        : result.source.generate_mipmaps
            ? generate_rgba8_mip_chain(result.source.width, result.source.height,
                                       result.source.array_layers, result.source.color_space,
                                       result.source.rgba8)
            : result.source.rgba8;
    rhi::RenderImageDesc image_desc;
    image_desc.format = is_cooked ? result.source.cooked_format
                        : result.source.color_space == TextureColorSpace::srgb
                            ? rhi::RenderImageFormat::rgba8_srgb
                            : rhi::RenderImageFormat::rgba8_unorm;
    image_desc.width = result.source.width;
    image_desc.height = result.source.height;
    image_desc.debug_name = result.source.id;
    image_desc.array_layers = result.source.array_layers;
    image_desc.mip_levels = result.mip_levels;
    auto uploaded = device_.upload_image(image_desc, upload_bytes);
    if (!uploaded) {
        return core::Result<PreparedTexture>::failure(uploaded.error().code,
                                                      uploaded.error().message);
    }
    result.image = uploaded.value().handle;
    result.resident_bytes = upload_bytes.size();
    return core::Result<PreparedTexture>::success(std::move(result));
}

TextureManager::TextureRecord* TextureManager::find_record(TextureHandle handle) noexcept {
    if (!handle.is_valid() || handle.index > textures_.size()) {
        return nullptr;
    }
    auto& record = textures_[handle.index - 1];
    return record.occupied && record.generation == handle.generation ? &record : nullptr;
}

const TextureManager::TextureRecord*
TextureManager::find_record(TextureHandle handle) const noexcept {
    if (!handle.is_valid() || handle.index > textures_.size()) {
        return nullptr;
    }
    const auto& record = textures_[handle.index - 1];
    return record.occupied && record.generation == handle.generation ? &record : nullptr;
}

void TextureManager::refresh_view(TextureRecord& record) {
    record.view.id = record.texture.source.id;
    record.view.image = record.texture.image;
    record.view.revision = record.revision;
    record.view.width = record.texture.source.width;
    record.view.height = record.texture.source.height;
    record.view.array_layers = record.texture.source.array_layers;
    record.view.mip_levels = record.texture.mip_levels;
    record.view.color_space = record.texture.source.color_space;
    record.view.format = record.texture.source.cooked_bytes.empty()
                             ? record.texture.source.color_space == TextureColorSpace::srgb
                                   ? rhi::RenderImageFormat::rgba8_srgb
                                   : rhi::RenderImageFormat::rgba8_unorm
                             : record.texture.source.cooked_format;
    record.view.resident_bytes = record.texture.resident_bytes;
}

void TextureManager::update_stats() noexcept {
    stats_.resident_texture_count = 0;
    stats_.resident_texture_bytes = 0;
    for (const auto& record : textures_) {
        if (!record.occupied) {
            continue;
        }
        ++stats_.resident_texture_count;
        stats_.resident_texture_bytes += record.texture.resident_bytes;
    }
}

core::Status validate_texture_upload_desc(const TextureUploadDesc& desc) {
    if (desc.id.empty()) {
        return core::Status::failure("texture_manager.missing_id", "texture id must not be empty");
    }
    if (desc.width == 0 || desc.height == 0 || desc.array_layers == 0) {
        return core::Status::failure("texture_manager.invalid_extent",
                                     "texture dimensions and array layers must be nonzero");
    }
    if (!desc.cooked_bytes.empty()) {
        if (!desc.rgba8.empty() || desc.generate_mipmaps || desc.cooked_mip_levels == 0) {
            return core::Status::failure(
                "texture_manager.invalid_cooked_texture",
                "cooked texture uploads require only a complete supplied mip chain");
        }
        rhi::RenderImageDesc image_desc;
        image_desc.format = desc.cooked_format;
        image_desc.width = desc.width;
        image_desc.height = desc.height;
        image_desc.array_layers = desc.array_layers;
        image_desc.mip_levels = desc.cooked_mip_levels;
        return rhi::validate_render_image_upload(image_desc, desc.cooked_bytes);
    }
    if (desc.cooked_mip_levels != 0) {
        return core::Status::failure("texture_manager.invalid_cooked_texture",
                                     "cooked mip counts require supplied cooked texture bytes");
    }
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    auto expected = static_cast<std::size_t>(desc.width);
    if (desc.height > maximum / expected) {
        return core::Status::failure("texture_manager.extent_overflow",
                                     "texture base-level byte size overflows size_t");
    }
    expected *= desc.height;
    if (desc.array_layers > maximum / expected ||
        static_cast<std::size_t>(desc.array_layers) * expected > maximum / 4U) {
        return core::Status::failure("texture_manager.extent_overflow",
                                     "texture base-level byte size overflows size_t");
    }
    expected *= static_cast<std::size_t>(desc.array_layers) * 4U;
    if (desc.rgba8.size() != expected) {
        return core::Status::failure("texture_manager.base_level_size_mismatch",
                                     "RGBA8 texture bytes must match its base extent and layers");
    }
    return core::Status::ok();
}

core::Result<TextureUploadDesc> texture_upload_desc_from_asset(std::string id,
                                                               const assets::TextureAsset& asset) {
    auto status = assets::validate_texture_asset(asset);
    if (!status) {
        return core::Result<TextureUploadDesc>::failure(status.error().code,
                                                        status.error().message);
    }
    if (id.empty()) {
        return core::Result<TextureUploadDesc>::failure("texture_manager.missing_id",
                                                        "texture id must not be empty");
    }
    TextureUploadDesc result;
    result.id = std::move(id);
    result.width = asset.width;
    result.height = asset.height;
    result.color_space = asset.color_space == assets::TextureAssetColorSpace::srgb
                             ? TextureColorSpace::srgb
                             : TextureColorSpace::linear;
    result.generate_mipmaps = false;
    result.cooked_mip_levels = static_cast<std::uint32_t>(asset.mips.size());
    switch (asset.format) {
    case assets::TextureAssetFormat::rgba8:
        result.cooked_format = asset.color_space == assets::TextureAssetColorSpace::srgb
                                   ? rhi::RenderImageFormat::rgba8_srgb
                                   : rhi::RenderImageFormat::rgba8_unorm;
        break;
    case assets::TextureAssetFormat::bc5_rg:
        result.cooked_format = rhi::RenderImageFormat::bc5_rg_unorm;
        break;
    case assets::TextureAssetFormat::bc7_rgba:
        result.cooked_format = asset.color_space == assets::TextureAssetColorSpace::srgb
                                   ? rhi::RenderImageFormat::bc7_rgba_srgb
                                   : rhi::RenderImageFormat::bc7_rgba_unorm;
        break;
    }
    result.cooked_bytes.reserve(asset.gpu_memory_bytes());
    for (const auto& mip : asset.mips) {
        result.cooked_bytes.insert(
            result.cooked_bytes.end(), reinterpret_cast<const std::byte*>(mip.bytes.data()),
            reinterpret_cast<const std::byte*>(mip.bytes.data() + mip.bytes.size()));
    }
    status = validate_texture_upload_desc(result);
    if (!status) {
        return core::Result<TextureUploadDesc>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<TextureUploadDesc>::success(std::move(result));
}

std::uint32_t complete_mip_level_count(std::uint32_t width, std::uint32_t height) noexcept {
    std::uint32_t levels = 1;
    for (auto dimension = std::max(width, height); dimension > 1; dimension /= 2) {
        ++levels;
    }
    return levels;
}

std::vector<std::byte> generate_rgba8_mip_chain(std::uint32_t width, std::uint32_t height,
                                                std::uint32_t array_layers,
                                                TextureColorSpace color_space,
                                                std::span<const std::byte> base_level) {
    std::vector<std::byte> result(base_level.begin(), base_level.end());
    std::vector<std::byte> previous(base_level.begin(), base_level.end());
    auto previous_width = width;
    auto previous_height = height;
    for (std::uint32_t mip = 1; mip < complete_mip_level_count(width, height); ++mip) {
        const auto mip_width = std::max(1U, previous_width / 2U);
        const auto mip_height = std::max(1U, previous_height / 2U);
        std::vector<std::byte> next(static_cast<std::size_t>(mip_width) * mip_height *
                                    array_layers * 4U);
        for (std::uint32_t layer = 0; layer < array_layers; ++layer) {
            for (std::uint32_t y = 0; y < mip_height; ++y) {
                for (std::uint32_t x = 0; x < mip_width; ++x) {
                    const auto destination =
                        ((static_cast<std::size_t>(layer) * mip_height + y) * mip_width + x) * 4U;
                    for (std::uint32_t channel = 0; channel < 4; ++channel) {
                        std::array<std::uint8_t, 4> samples{};
                        std::size_t sample_index = 0;
                        for (std::uint32_t dy = 0; dy < 2; ++dy) {
                            for (std::uint32_t dx = 0; dx < 2; ++dx) {
                                const auto source_x = std::min(previous_width - 1U, x * 2U + dx);
                                const auto source_y = std::min(previous_height - 1U, y * 2U + dy);
                                const auto source =
                                    ((static_cast<std::size_t>(layer) * previous_height +
                                      source_y) *
                                         previous_width +
                                     source_x) *
                                        4U +
                                    channel;
                                samples[sample_index++] = byte_value(previous[source]);
                            }
                        }
                        const bool srgb_channel =
                            color_space == TextureColorSpace::srgb && channel < 3;
                        next[destination + channel] = averaged_channel(samples, srgb_channel);
                    }
                }
            }
        }
        result.insert(result.end(), next.begin(), next.end());
        previous = std::move(next);
        previous_width = mip_width;
        previous_height = mip_height;
    }
    return result;
}

} // namespace heartstead::renderer
