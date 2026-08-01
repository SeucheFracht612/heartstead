#include "engine/renderer/ui/ui_font.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <future>
#include <thread>
#include <utility>

namespace heartstead::renderer {

namespace {

struct PendingGlyph {
    UiFontGlyph glyph;
    std::vector<std::uint8_t> sdf;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] std::vector<std::uint32_t> production_codepoints() {
    std::vector<std::uint32_t> result;
    const auto append_range = [&result](std::uint32_t first, std::uint32_t last) {
        for (auto value = first; value <= last; ++value) {
            result.push_back(value);
        }
    };
    append_range(0x20, 0x7e);
    append_range(0xa0, 0x024f);
    append_range(0x0370, 0x03ff);
    append_range(0x0400, 0x052f);
    append_range(0x2000, 0x206f);
    append_range(0x20a0, 0x20cf);
    result.push_back(0xfffd);
    return result;
}

[[nodiscard]] std::uint32_t next_power_of_two(std::uint32_t value) noexcept {
    if (value <= 1U) {
        return 1U;
    }
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
}

[[nodiscard]] bool is_continuation(unsigned char value) noexcept {
    return (value & 0xc0U) == 0x80U;
}

[[nodiscard]] std::uint32_t decode_utf8(std::string_view text, std::size_t& cursor,
                                        bool& replaced) noexcept {
    const auto first = static_cast<unsigned char>(text[cursor]);
    const auto fail = [&]() {
        ++cursor;
        replaced = true;
        return 0xfffdU;
    };
    if (first < 0x80U) {
        ++cursor;
        return first;
    }
    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xe0U) == 0xc0U) {
        length = 2;
        value = first & 0x1fU;
        minimum = 0x80U;
    } else if ((first & 0xf0U) == 0xe0U) {
        length = 3;
        value = first & 0x0fU;
        minimum = 0x800U;
    } else if ((first & 0xf8U) == 0xf0U) {
        length = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return fail();
    }
    if (cursor + length > text.size()) {
        return fail();
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto byte = static_cast<unsigned char>(text[cursor + index]);
        if (!is_continuation(byte)) {
            return fail();
        }
        value = (value << 6U) | (byte & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
        return fail();
    }
    cursor += length;
    return value;
}

} // namespace

core::Status UiFontConfig::validate() const noexcept {
    if (!std::isfinite(em_size_pixels) || em_size_pixels < 16.0F || em_size_pixels > 128.0F ||
        atlas_width < 256U || atlas_width > 4096U ||
        (atlas_width & (atlas_width - 1U)) != 0U || maximum_atlas_height < 256U ||
        maximum_atlas_height > 4096U ||
        (maximum_atlas_height & (maximum_atlas_height - 1U)) != 0U ||
        glyph_padding < 2U || glyph_padding > 16U) {
        return core::Status::failure(
            "ui_font.invalid_config",
            "SDF font size, power-of-two atlas limits, or glyph padding are invalid");
    }
    return core::Status::ok();
}

core::Result<UiFont> UiFont::build(std::span<const std::uint8_t> sfnt_bytes,
                                   UiFontConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<UiFont>::failure(status.error().code, status.error().message);
    }
    if (sfnt_bytes.empty() || sfnt_bytes.size() > static_cast<std::size_t>(INT_MAX)) {
        return core::Result<UiFont>::failure("ui_font.invalid_source",
                                             "font source is empty or exceeds parser limits");
    }
    stbtt_fontinfo info{};
    const auto offset = stbtt_GetFontOffsetForIndex(sfnt_bytes.data(), 0);
    if (offset < 0 || stbtt_InitFont(&info, sfnt_bytes.data(), offset) == 0) {
        return core::Result<UiFont>::failure("ui_font.invalid_sfnt",
                                             "font source is not a valid TrueType/OpenType face");
    }

    UiFont result;
    result.config_ = config;
    const auto scale = stbtt_ScaleForPixelHeight(&info, config.em_size_pixels);
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &line_gap);
    result.ascent_pixels_ = static_cast<float>(ascent) * scale;
    result.line_height_pixels_ = static_cast<float>(ascent - descent + line_gap) * scale;

    const auto codepoints = production_codepoints();
    std::vector<PendingGlyph> rasterized(codepoints.size());
    const auto rasterize_range = [&](std::size_t first, std::size_t last) {
        for (auto index = first; index < last; ++index) {
            const auto codepoint = codepoints[index];
            if (stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint)) == 0 &&
                codepoint != 0xfffdU) {
                continue;
            }
            auto& item = rasterized[index];
            item.glyph.codepoint = codepoint;
            int advance = 0;
            int bearing = 0;
            stbtt_GetCodepointHMetrics(&info, static_cast<int>(codepoint), &advance, &bearing);
            item.glyph.advance = static_cast<float>(advance) * scale;
            int width = 0;
            int height = 0;
            int x_offset = 0;
            int y_offset = 0;
            auto* sdf = stbtt_GetCodepointSDF(
                &info, scale, static_cast<int>(codepoint),
                static_cast<int>(config.glyph_padding), 128U, 32.0F, &width, &height, &x_offset,
                &y_offset);
            if (sdf != nullptr && width > 0 && height > 0) {
                item.width = static_cast<std::uint32_t>(width);
                item.height = static_cast<std::uint32_t>(height);
                item.sdf.assign(sdf, sdf + static_cast<std::ptrdiff_t>(width * height));
                stbtt_FreeSDF(sdf, info.userdata);
                item.glyph.plane_minimum = {static_cast<float>(x_offset),
                                            static_cast<float>(y_offset)};
                item.glyph.plane_maximum = {static_cast<float>(x_offset + width),
                                            static_cast<float>(y_offset + height)};
                item.glyph.drawable = true;
            }
        }
    };
    const auto hardware_workers = std::max(1U, std::thread::hardware_concurrency());
    const auto worker_count =
        std::min<std::size_t>({8U, static_cast<std::size_t>(hardware_workers), codepoints.size()});
    const auto items_per_worker = (codepoints.size() + worker_count - 1U) / worker_count;
    std::vector<std::future<void>> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        const auto first = worker * items_per_worker;
        const auto last = std::min(codepoints.size(), first + items_per_worker);
        if (first == last) {
            break;
        }
        workers.push_back(std::async(std::launch::async, rasterize_range, first, last));
    }
    for (auto& worker : workers) {
        worker.get();
    }
    std::vector<PendingGlyph> pending;
    pending.reserve(rasterized.size());
    for (auto& item : rasterized) {
        if (item.glyph.codepoint != 0U) {
            pending.push_back(std::move(item));
        }
    }
    if (pending.empty()) {
        return core::Result<UiFont>::failure("ui_font.no_glyphs",
                                             "font contains no supported production glyphs");
    }

    struct Placement {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };
    std::vector<Placement> placements(pending.size());
    std::uint32_t cursor_x = 0;
    std::uint32_t cursor_y = 0;
    std::uint32_t row_height = 0;
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto& item = pending[index];
        if (!item.glyph.drawable) {
            continue;
        }
        if (item.width > config.atlas_width) {
            return core::Result<UiFont>::failure("ui_font.glyph_too_large",
                                                 "a glyph exceeds the configured atlas width");
        }
        if (cursor_x + item.width > config.atlas_width) {
            cursor_x = 0;
            cursor_y += row_height;
            row_height = 0;
        }
        placements[index] = {cursor_x, cursor_y};
        cursor_x += item.width;
        row_height = std::max(row_height, item.height);
    }
    result.atlas_height_ = next_power_of_two(std::max(cursor_y + row_height, 1U));
    if (result.atlas_height_ > config.maximum_atlas_height) {
        return core::Result<UiFont>::failure("ui_font.atlas_exhausted",
                                             "production glyph set does not fit the font atlas");
    }
    result.atlas_sdf_.assign(
        static_cast<std::size_t>(config.atlas_width) * result.atlas_height_, 0U);
    result.glyphs_.reserve(pending.size());
    result.glyph_lookup_.reserve(pending.size());
    for (std::size_t index = 0; index < pending.size(); ++index) {
        auto glyph = pending[index].glyph;
        if (glyph.drawable) {
            const auto placement = placements[index];
            for (std::uint32_t row = 0; row < pending[index].height; ++row) {
                const auto source = static_cast<std::size_t>(row) * pending[index].width;
                const auto destination =
                    static_cast<std::size_t>(placement.y + row) * config.atlas_width + placement.x;
                std::copy_n(pending[index].sdf.begin() + static_cast<std::ptrdiff_t>(source),
                            pending[index].width,
                            result.atlas_sdf_.begin() + static_cast<std::ptrdiff_t>(destination));
            }
            glyph.uv_minimum = {
                static_cast<float>(placement.x) / static_cast<float>(config.atlas_width),
                static_cast<float>(placement.y) / static_cast<float>(result.atlas_height_)};
            glyph.uv_maximum = {
                static_cast<float>(placement.x + pending[index].width) /
                    static_cast<float>(config.atlas_width),
                static_cast<float>(placement.y + pending[index].height) /
                    static_cast<float>(result.atlas_height_)};
        }
        result.glyph_lookup_.emplace(glyph.codepoint, result.glyphs_.size());
        result.glyphs_.push_back(glyph);
    }
    return core::Result<UiFont>::success(std::move(result));
}

core::Result<UiTextLayout> UiFont::layout_utf8(std::string_view text,
                                               math::Vec2f origin_pixels,
                                               float em_size_pixels) const {
    if (text.empty() || !origin_pixels.is_finite() || !std::isfinite(em_size_pixels) ||
        em_size_pixels <= 0.0F) {
        return core::Result<UiTextLayout>::failure(
            "ui_font.invalid_layout", "text layout requires content, a finite origin, and size");
    }
    UiTextLayout result;
    result.glyphs.reserve(text.size());
    const auto scale = em_size_pixels / config_.em_size_pixels;
    auto cursor = origin_pixels;
    const auto line_start_x = cursor.x;
    float maximum_x = cursor.x;
    std::size_t byte = 0;
    while (byte < text.size()) {
        bool replaced = false;
        auto codepoint = decode_utf8(text, byte, replaced);
        if (codepoint == '\r') {
            continue;
        }
        if (codepoint == '\n') {
            maximum_x = std::max(maximum_x, cursor.x);
            cursor.x = line_start_x;
            cursor.y += line_height_pixels_ * scale;
            continue;
        }
        auto* selected = glyph(codepoint);
        if (selected == nullptr) {
            selected = glyph(0xfffdU);
            replaced = true;
        }
        if (replaced) {
            ++result.replacement_count;
        }
        if (selected == nullptr) {
            return core::Result<UiTextLayout>::failure(
                "ui_font.missing_replacement_glyph",
                "font does not provide the required Unicode replacement glyph");
        }
        if (selected->drawable) {
            const auto baseline = cursor.y + ascent_pixels_ * scale;
            result.glyphs.push_back(
                {selected,
                 {cursor.x + selected->plane_minimum.x * scale,
                  baseline + selected->plane_minimum.y * scale},
                 {cursor.x + selected->plane_maximum.x * scale,
                  baseline + selected->plane_maximum.y * scale}});
        }
        cursor.x += selected->advance * scale;
    }
    maximum_x = std::max(maximum_x, cursor.x);
    result.extent_pixels = {maximum_x - line_start_x,
                            cursor.y - origin_pixels.y + line_height_pixels_ * scale};
    return core::Result<UiTextLayout>::success(std::move(result));
}

const UiFontGlyph* UiFont::glyph(std::uint32_t codepoint) const noexcept {
    const auto found = glyph_lookup_.find(codepoint);
    return found == glyph_lookup_.end() ? nullptr : &glyphs_[found->second];
}

std::span<const std::uint8_t> UiFont::atlas_sdf() const noexcept { return atlas_sdf_; }
std::uint32_t UiFont::atlas_width() const noexcept { return config_.atlas_width; }
std::uint32_t UiFont::atlas_height() const noexcept { return atlas_height_; }
float UiFont::em_size_pixels() const noexcept { return config_.em_size_pixels; }
float UiFont::line_height_pixels() const noexcept { return line_height_pixels_; }
float UiFont::ascent_pixels() const noexcept { return ascent_pixels_; }

} // namespace heartstead::renderer
