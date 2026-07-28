#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/coords/world_coords.hpp"

namespace heartstead::world {

class ChunkDatabase;
class VoxelPalette;

[[nodiscard]] core::Result<double>
query_fluid_submersion(const ChunkDatabase& chunks, const VoxelPalette& palette,
                       BlockCoord local_origin, math::Bounds3d local_bounds);

} // namespace heartstead::world
