#pragma once

#include "engine/assets/texture_asset.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/assets/render_asset_handles.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

enum class TextureColorSpace : std::uint8_t {
    linear,
    srgb,
};

struct TextureUploadDesc {
    std::string id;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t array_layers = 1;
    TextureColorSpace color_space = TextureColorSpace::srgb;
    bool generate_mipmaps = true;
    std::vector<std::byte> rgba8;
    // Production-cooked payloads provide a complete GPU-native mip chain here. This is mutually
    // exclusive with rgba8 and bypasses runtime mip generation.
    rhi::RenderImageFormat cooked_format = rhi::RenderImageFormat::rgba8_unorm;
    std::uint32_t cooked_mip_levels = 0;
    std::vector<std::byte> cooked_bytes;
};

struct TextureView {
    TextureHandle handle;
    std::string id;
    rhi::RenderResourceHandle image;
    std::uint64_t revision = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t array_layers = 0;
    std::uint32_t mip_levels = 0;
    TextureColorSpace color_space = TextureColorSpace::srgb;
    rhi::RenderImageFormat format = rhi::RenderImageFormat::rgba8_unorm;
    std::size_t resident_bytes = 0;
};

struct TextureManagerStats {
    std::size_t resident_texture_count = 0;
    std::uint64_t resident_texture_bytes = 0;
    std::uint64_t uploaded_texture_bytes = 0;
    std::uint64_t successful_reload_count = 0;
    std::uint64_t failed_reload_count = 0;
};

class TextureManager {
  public:
    explicit TextureManager(rhi::IRenderDevice& device);
    ~TextureManager();

    TextureManager(const TextureManager&) = delete;
    TextureManager& operator=(const TextureManager&) = delete;

    [[nodiscard]] core::Status initialize_fallbacks();
    [[nodiscard]] core::Result<TextureHandle> create_texture(TextureUploadDesc desc);
    [[nodiscard]] core::Status replace_texture(TextureHandle handle, TextureUploadDesc desc);
    [[nodiscard]] core::Status release_texture(TextureHandle handle);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] const TextureView* find(TextureHandle handle) const noexcept;
    [[nodiscard]] const TextureView* find(std::string_view id) const noexcept;
    [[nodiscard]] const TextureView& resolve_or_error(TextureHandle handle) const noexcept;

    [[nodiscard]] TextureHandle error_texture() const noexcept;
    [[nodiscard]] TextureHandle white_texture() const noexcept;
    [[nodiscard]] TextureHandle black_texture() const noexcept;
    [[nodiscard]] TextureHandle normal_texture() const noexcept;
    [[nodiscard]] const TextureManagerStats& stats() const noexcept;

  private:
    struct TextureRecord;
    struct PreparedTexture;

    [[nodiscard]] core::Result<PreparedTexture> prepare(TextureUploadDesc desc) const;
    [[nodiscard]] TextureRecord* find_record(TextureHandle handle) noexcept;
    [[nodiscard]] const TextureRecord* find_record(TextureHandle handle) const noexcept;
    void refresh_view(TextureRecord& record);
    void update_stats() noexcept;

    rhi::IRenderDevice& device_;
    std::vector<TextureRecord> textures_;
    TextureHandle error_texture_;
    TextureHandle white_texture_;
    TextureHandle black_texture_;
    TextureHandle normal_texture_;
    TextureManagerStats stats_{};
};

[[nodiscard]] core::Status validate_texture_upload_desc(const TextureUploadDesc& desc);
[[nodiscard]] std::uint32_t complete_mip_level_count(std::uint32_t width,
                                                     std::uint32_t height) noexcept;
[[nodiscard]] std::vector<std::byte>
generate_rgba8_mip_chain(std::uint32_t width, std::uint32_t height, std::uint32_t array_layers,
                         TextureColorSpace color_space, std::span<const std::byte> base_level);
[[nodiscard]] core::Result<TextureUploadDesc>
texture_upload_desc_from_asset(std::string id, const assets::TextureAsset& asset);

} // namespace heartstead::renderer
