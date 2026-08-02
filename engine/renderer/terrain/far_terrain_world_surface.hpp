#pragma once

#include "engine/renderer/terrain/far_terrain_clipmap.hpp"
#include "engine/world/world_state.hpp"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace heartstead::renderer {

[[nodiscard]] FarTerrainSurfaceSample
sample_far_terrain_world_surface(const world::WorldState& world, double world_x, double world_z,
                                 FarTerrainDomain domain) noexcept;
[[nodiscard]] std::uint64_t
far_terrain_world_surface_revision(const world::WorldState& world) noexcept;

// Owner-thread cache of the authoritative block-world surface used to snapshot mid/far terrain.
// Synchronization only rescans changed horizontal chunk columns and returns their exact horizontal
// invalidation bounds. Samplers can then read the immutable owner-owned map without scanning the
// live world for every clipmap vertex.
class FarTerrainWorldSurfaceCache {
  public:
    [[nodiscard]] std::vector<math::Bounds3d> synchronize(const world::WorldState& world,
                                                          std::uint64_t revision);
    [[nodiscard]] FarTerrainSurfaceSample sample(double world_x, double world_z,
                                                 FarTerrainDomain domain) const noexcept;
    void clear() noexcept;

    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] std::size_t sample_count() const noexcept;

  private:
    using HorizontalChunkCoord = std::pair<std::int64_t, std::int64_t>;
    using ChunkSurfaceState = std::pair<std::uint64_t, std::uint64_t>;
    using SurfaceCoord = std::pair<std::int64_t, std::int64_t>;

    std::map<SurfaceCoord, FarTerrainSurfaceSample> samples_;
    std::map<world::ChunkCoord, ChunkSurfaceState> chunk_states_;
    std::uint64_t revision_ = 0;
};

} // namespace heartstead::renderer
