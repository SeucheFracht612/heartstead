#include "engine/ui/map_view.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace heartstead::ui {

namespace {

[[nodiscard]] bool valid_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(color, [](float value) {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    });
}

[[nodiscard]] std::uint64_t coordinate_hash(std::int64_t x, std::int64_t z,
                                            std::string_view layer) noexcept {
    auto value = static_cast<std::uint64_t>(x) * 0x9e3779b97f4a7c15ULL;
    value ^= static_cast<std::uint64_t>(z) + 0xbf58476d1ce4e5b9ULL + (value << 6U) +
             (value >> 2U);
    for (const auto character : layer) {
        value ^= static_cast<unsigned char>(character);
        value *= 0x100000001b3ULL;
    }
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::array<float, 4> cell_color(player_profiles::MapCellCoord cell,
                                               std::string_view layer) noexcept {
    const auto noise = static_cast<float>(coordinate_hash(cell.x, cell.z, layer) & 0xffU) / 255.0F;
    if (layer == "underground") {
        return {0.22F + noise * 0.06F, 0.18F + noise * 0.05F, 0.14F + noise * 0.04F, 1.0F};
    }
    if (layer == "aerial") {
        return {0.35F + noise * 0.08F, 0.57F + noise * 0.08F, 0.68F + noise * 0.10F, 1.0F};
    }
    return {0.22F + noise * 0.08F, 0.39F + noise * 0.10F, 0.20F + noise * 0.06F, 1.0F};
}

void append_quad(std::vector<renderer::UiVertex>& vertices,
                 std::vector<std::uint32_t>& indices, math::Vec2f minimum,
                 math::Vec2f maximum, std::array<float, 4> color) {
    const auto first = static_cast<std::uint32_t>(vertices.size());
    vertices.insert(vertices.end(),
                    {renderer::UiVertex{minimum, {}, color},
                     renderer::UiVertex{{maximum.x, minimum.y}, {1.0F, 0.0F}, color},
                     renderer::UiVertex{maximum, {1.0F, 1.0F}, color},
                     renderer::UiVertex{{minimum.x, maximum.y}, {0.0F, 1.0F}, color}});
    indices.insert(indices.end(), {first, first + 1U, first + 2U, first, first + 2U, first + 3U});
}

} // namespace

core::Result<MapViewStats>
MapViewRenderer::paint(renderer::UiRenderer& renderer, renderer::rhi::RenderExtent extent,
                       const player_profiles::MapDiscovery& discovery, const MapViewDesc& desc,
                       std::span<const MapMarker> markers) {
    if (!extent.is_valid() || !desc.minimum_pixels.is_finite() ||
        !desc.maximum_pixels.is_finite() || desc.maximum_pixels.x <= desc.minimum_pixels.x ||
        desc.maximum_pixels.y <= desc.minimum_pixels.y || desc.layer_id.empty() ||
        desc.layer_id.size() > 64U || desc.cell_radius < 2U || desc.cell_radius > 48U ||
        !std::isfinite(desc.rotation_radians)) {
        return core::Result<MapViewStats>::failure(
            "map_view.invalid_desc", "map bounds, layer, radius, rotation, or extent are invalid");
    }
    if (desc.minimum_pixels.x < 0.0F || desc.minimum_pixels.y < 0.0F ||
        desc.maximum_pixels.x > static_cast<float>(extent.width) ||
        desc.maximum_pixels.y > static_cast<float>(extent.height)) {
        return core::Result<MapViewStats>::failure(
            "map_view.outside_extent", "map view must be contained by the framebuffer extent");
    }
    for (const auto& marker : markers) {
        if (marker.layer_id.empty() || marker.layer_id.size() > 64U ||
            !valid_color(marker.color)) {
            return core::Result<MapViewStats>::failure(
                "map_view.invalid_marker", "map marker layer or color is invalid");
        }
    }

    vertices_.clear();
    indices_.clear();
    MapViewStats stats;
    append_quad(vertices_, indices_, desc.minimum_pixels, desc.maximum_pixels,
                desc.kind == MapViewKind::full_map
                    ? std::array{0.035F, 0.048F, 0.038F, 0.97F}
                    : std::array{0.025F, 0.035F, 0.028F, 0.88F});
    ++stats.submitted_quads;

    const auto diameter = desc.cell_radius * 2U + 1U;
    const auto width = desc.maximum_pixels.x - desc.minimum_pixels.x;
    const auto height = desc.maximum_pixels.y - desc.minimum_pixels.y;
    const auto cell_size = std::min(width, height) / static_cast<float>(diameter);
    const auto content_size = cell_size * static_cast<float>(diameter);
    const math::Vec2f content_origin{
        desc.minimum_pixels.x + (width - content_size) * 0.5F,
        desc.minimum_pixels.y + (height - content_size) * 0.5F};
    for (std::int64_t z = -static_cast<std::int64_t>(desc.cell_radius);
         z <= static_cast<std::int64_t>(desc.cell_radius); ++z) {
        for (std::int64_t x = -static_cast<std::int64_t>(desc.cell_radius);
             x <= static_cast<std::int64_t>(desc.cell_radius); ++x) {
            ++stats.tested_cells;
            const player_profiles::MapCellCoord cell{desc.center.x + x, desc.center.z + z};
            if (!discovery.is_discovered(desc.layer_id, cell)) {
                continue;
            }
            ++stats.discovered_cells;
            const auto screen_x = static_cast<float>(x + desc.cell_radius);
            const auto screen_z = static_cast<float>(z + desc.cell_radius);
            const math::Vec2f minimum{content_origin.x + screen_x * cell_size,
                                      content_origin.y + screen_z * cell_size};
            append_quad(vertices_, indices_, minimum,
                        {minimum.x + cell_size + 0.35F, minimum.y + cell_size + 0.35F},
                        cell_color(cell, desc.layer_id));
            ++stats.submitted_quads;
        }
    }

    for (const auto& marker : markers) {
        if (marker.layer_id != desc.layer_id) {
            continue;
        }
        const auto relative_x = marker.cell.x - desc.center.x;
        const auto relative_z = marker.cell.z - desc.center.z;
        if (std::abs(relative_x) > desc.cell_radius || std::abs(relative_z) > desc.cell_radius) {
            continue;
        }
        const auto size = std::max(4.0F, cell_size * 1.75F);
        const math::Vec2f center{
            content_origin.x + (static_cast<float>(relative_x + desc.cell_radius) + 0.5F) * cell_size,
            content_origin.y + (static_cast<float>(relative_z + desc.cell_radius) + 0.5F) * cell_size};
        append_quad(vertices_, indices_, {center.x - size * 0.5F, center.y - size * 0.5F},
                    {center.x + size * 0.5F, center.y + size * 0.5F}, marker.color);
        ++stats.visible_markers;
        ++stats.submitted_quads;
    }

    const renderer::UiScissorRect scissor{
        static_cast<std::uint32_t>(desc.minimum_pixels.x),
        static_cast<std::uint32_t>(desc.minimum_pixels.y),
        static_cast<std::uint32_t>(std::ceil(width)),
        static_cast<std::uint32_t>(std::ceil(height))};
    auto submitted = renderer.submit_triangles(
        {vertices_, indices_, 0, true, scissor, false});
    if (!submitted) {
        return core::Result<MapViewStats>::failure(submitted.error().code,
                                                   submitted.error().message);
    }
    stats.draw_calls = 1;
    return core::Result<MapViewStats>::success(stats);
}

std::string_view map_layer_name(std::string_view layer_id) noexcept {
    if (layer_id == "surface") return "Surface";
    if (layer_id == "underground") return "Underground";
    if (layer_id == "aerial") return "Aerial";
    return layer_id;
}

} // namespace heartstead::ui
