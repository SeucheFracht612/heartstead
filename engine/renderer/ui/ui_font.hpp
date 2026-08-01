#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::renderer {

struct UiFontConfig {
    float em_size_pixels = 48.0F;
    std::uint32_t atlas_width = 2048;
    std::uint32_t maximum_atlas_height = 4096;
    std::uint32_t glyph_padding = 6;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct UiFontGlyph {
    std::uint32_t codepoint = 0;
    float advance = 0.0F;
    math::Vec2f plane_minimum{};
    math::Vec2f plane_maximum{};
    math::Vec2f uv_minimum{};
    math::Vec2f uv_maximum{};
    bool drawable = false;
};

struct UiGlyphPlacement {
    const UiFontGlyph* glyph = nullptr;
    math::Vec2f minimum_pixels{};
    math::Vec2f maximum_pixels{};
};

struct UiTextLayout {
    std::vector<UiGlyphPlacement> glyphs;
    math::Vec2f extent_pixels{};
    std::uint32_t replacement_count = 0;
};

class UiFont {
  public:
    [[nodiscard]] static core::Result<UiFont>
    build(std::span<const std::uint8_t> sfnt_bytes, UiFontConfig config = {});

    [[nodiscard]] core::Result<UiTextLayout>
    layout_utf8(std::string_view text, math::Vec2f origin_pixels, float em_size_pixels) const;
    [[nodiscard]] const UiFontGlyph* glyph(std::uint32_t codepoint) const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> atlas_sdf() const noexcept;
    [[nodiscard]] std::uint32_t atlas_width() const noexcept;
    [[nodiscard]] std::uint32_t atlas_height() const noexcept;
    [[nodiscard]] float em_size_pixels() const noexcept;
    [[nodiscard]] float line_height_pixels() const noexcept;
    [[nodiscard]] float ascent_pixels() const noexcept;

  private:
    UiFontConfig config_{};
    std::uint32_t atlas_height_ = 0;
    float ascent_pixels_ = 0.0F;
    float line_height_pixels_ = 0.0F;
    std::vector<std::uint8_t> atlas_sdf_;
    std::vector<UiFontGlyph> glyphs_;
    std::unordered_map<std::uint32_t, std::size_t> glyph_lookup_;
};

} // namespace heartstead::renderer
