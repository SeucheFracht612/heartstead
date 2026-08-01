#pragma once

#include "engine/core/result.hpp"
#include "engine/world/coords/world_coords.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::dirty {

using DirtyRegionClock = std::chrono::steady_clock;

enum class DirtyRegionKind {
    chunk_mesh,
    chunk_collision,
    chunk_lighting,
    room_graph,
    road_network,
    cart_access_network,
    storage_access_network,
    power_network,
    ward_network,
    smoke_ventilation_network,
    water_network,
    logistics_network,
};

using DirtyRegionCoord = world::BlockCoord;

struct DirtyRegionBounds {
    DirtyRegionCoord min;
    DirtyRegionCoord max;

    [[nodiscard]] static DirtyRegionBounds single(DirtyRegionCoord coord) noexcept;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] bool overlaps_or_touches(const DirtyRegionBounds& other) const noexcept;
    [[nodiscard]] DirtyRegionBounds merged_with(const DirtyRegionBounds& other) const noexcept;
    [[nodiscard]] core::Result<DirtyRegionBounds> expanded(std::uint32_t padding) const;
};

struct DirtyRegion {
    DirtyRegionKind kind = DirtyRegionKind::room_graph;
    DirtyRegionBounds bounds;
    std::string reason;
    std::uint64_t sequence = 0;
    DirtyRegionClock::time_point marked_at{};
};

class DirtyRegionTracker {
  public:
    [[nodiscard]] core::Status mark(DirtyRegionKind kind, DirtyRegionBounds bounds,
                                    std::string reason = {});
    [[nodiscard]] core::Status mark_single(DirtyRegionKind kind, DirtyRegionCoord coord,
                                           std::string reason = {});

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t count(DirtyRegionKind kind) const noexcept;
    [[nodiscard]] const std::vector<DirtyRegion>& regions() const noexcept;

    [[nodiscard]] std::vector<DirtyRegion> consume_kind(DirtyRegionKind kind);
    [[nodiscard]] std::vector<DirtyRegion> consume_all();
    void clear() noexcept;

  private:
    std::vector<DirtyRegion> regions_;
    std::uint64_t next_sequence_ = 1;
};

[[nodiscard]] std::string_view dirty_region_kind_name(DirtyRegionKind kind) noexcept;

} // namespace heartstead::dirty
