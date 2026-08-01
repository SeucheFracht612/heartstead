#pragma once

#include "engine/core/result.hpp"
#include "engine/player_profiles/map_discovery.hpp"
#include "engine/renderer/ui/ui_renderer.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace heartstead::ui {

enum class MapViewKind : std::uint8_t { minimap, full_map };
enum class MapMarkerKind : std::uint8_t { player, settlement, resource, danger, custom };

struct MapMarker {
    std::uint64_t stable_id = 0;
    player_profiles::MapCellCoord cell{};
    std::string layer_id = "surface";
    MapMarkerKind kind = MapMarkerKind::custom;
    std::array<float, 4> color{0.96F, 0.82F, 0.28F, 1.0F};
    std::string label;
};

struct MapViewDesc {
    MapViewKind kind = MapViewKind::minimap;
    math::Vec2f minimum_pixels{};
    math::Vec2f maximum_pixels{};
    player_profiles::MapCellCoord center{};
    std::string_view layer_id = "surface";
    std::uint32_t cell_radius = 12;
    float rotation_radians = 0.0F;
};

struct MapViewStats {
    std::uint32_t tested_cells = 0;
    std::uint32_t discovered_cells = 0;
    std::uint32_t visible_markers = 0;
    std::uint32_t submitted_quads = 0;
    std::uint32_t draw_calls = 0;
};

class MapViewRenderer {
  public:
    [[nodiscard]] core::Result<MapViewStats>
    paint(renderer::UiRenderer& renderer, renderer::rhi::RenderExtent extent,
          const player_profiles::MapDiscovery& discovery, const MapViewDesc& desc,
          std::span<const MapMarker> markers = {});

  private:
    std::vector<renderer::UiVertex> vertices_;
    std::vector<std::uint32_t> indices_;
};

[[nodiscard]] std::string_view map_layer_name(std::string_view layer_id) noexcept;

} // namespace heartstead::ui
