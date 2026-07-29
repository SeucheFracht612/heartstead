#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/assets/texture_manager.hpp"
#include "engine/renderer/materials/material_runtime_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace heartstead::renderer {

struct SurfaceTextureArrayConfig {
    std::uint32_t layer_width = 512;
    std::uint32_t layer_height = 512;
    std::uint32_t maximum_layers = 256;

    [[nodiscard]] core::Status validate() const;
};

struct SurfaceTextureArrayFallbacks {
    std::uint32_t error = 0;
    std::uint32_t white = 1;
    std::uint32_t normal = 2;
    std::uint32_t black = 3;
};

class SurfaceTextureArray {
  public:
    explicit SurfaceTextureArray(TextureManager& textures);

    [[nodiscard]] core::Status initialize(SurfaceTextureArrayConfig config = {});
    [[nodiscard]] core::Result<std::uint32_t> add(std::string id, std::uint32_t width,
                                                  std::uint32_t height,
                                                  std::span<const std::uint8_t> rgba8);
    [[nodiscard]] core::Status synchronize();
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] TextureHandle texture() const noexcept;
    [[nodiscard]] const TextureView* texture_view() const noexcept;
    [[nodiscard]] SurfaceTextureArrayFallbacks fallbacks() const noexcept;
    [[nodiscard]] std::size_t layer_count() const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept;

  private:
    TextureManager* textures_ = nullptr;
    SurfaceTextureArrayConfig config_{};
    TerrainTextureArrayBuilder layers_{1, 1};
    TextureHandle texture_;
    std::uint64_t revision_ = 0;
    std::uint64_t synchronized_revision_ = 0;
    bool initialized_ = false;
};

[[nodiscard]] std::vector<std::byte> resize_surface_rgba8(std::uint32_t source_width,
                                                          std::uint32_t source_height,
                                                          std::span<const std::uint8_t> source,
                                                          std::uint32_t target_width,
                                                          std::uint32_t target_height);

} // namespace heartstead::renderer
