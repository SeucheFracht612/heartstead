#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace heartstead::renderer {

// Matches ChunkMeshFaceDirection and the terrain shader's face-table order.
enum class VoxelMaterialFace : std::uint8_t {
    west,
    east,
    bottom,
    top,
    north,
    south,
};

inline constexpr std::size_t voxel_material_face_count = 6;

[[nodiscard]] constexpr std::size_t voxel_material_face_index(VoxelMaterialFace face) noexcept {
    return static_cast<std::size_t>(face);
}

[[nodiscard]] constexpr std::string_view voxel_material_face_name(VoxelMaterialFace face) noexcept {
    switch (face) {
    case VoxelMaterialFace::west:
        return "west";
    case VoxelMaterialFace::east:
        return "east";
    case VoxelMaterialFace::bottom:
        return "bottom";
    case VoxelMaterialFace::top:
        return "top";
    case VoxelMaterialFace::north:
        return "north";
    case VoxelMaterialFace::south:
        return "south";
    }
    return "unknown";
}

} // namespace heartstead::renderer
