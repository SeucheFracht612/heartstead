#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/blocks/block_model.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace heartstead::world {

class ChunkDatabase;
class VoxelPalette;
struct BlockRenderTableSnapshot;
struct ChunkNeighborhoodSnapshot;
enum class MeshingRenderPhase : std::uint8_t;

enum class ChunkMeshFaceDirection {
    negative_x,
    positive_x,
    negative_y,
    positive_y,
    negative_z,
    positive_z,
};

struct ChunkMeshVertex {
    math::Vec3f position{};
    math::Vec3f normal{};
    float u = 0.0F;
    float v = 0.0F;
    std::uint16_t voxel_type = 0;
    std::uint8_t light = 0;
    std::uint16_t state_bits = 0;
    std::uint8_t ambient_occlusion = 255;

    friend auto operator<=>(const ChunkMeshVertex&, const ChunkMeshVertex&) = default;
};

struct RichBlockMeshInstance {
    core::PrototypeId block_model_id;
    VoxelCoord owning_cell{};
    math::Bounds3f local_render_bounds{};
    std::uint16_t voxel_type = 0;
    std::uint16_t state_bits = 0;
    std::uint32_t metadata_handle = 0;
};

struct ChunkMeshSection {
    std::uint16_t material_index = 0;
    MeshingRenderPhase render_phase = static_cast<MeshingRenderPhase>(0);
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;

    friend auto operator<=>(const ChunkMeshSection&, const ChunkMeshSection&) = default;
};

struct ChunkMesh {
    ChunkCoord chunk_coord{};
    math::Bounds3f local_bounds{};
    std::vector<ChunkMeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<ChunkMeshSection> sections;
    std::vector<RichBlockMeshInstance> rich_instances;
    std::size_t face_count = 0;
    std::size_t triangle_face_count = 0;
    std::uint16_t required_halo_radius = 0;
    std::uint16_t provided_halo_radius = 0;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] core::Status finalize_sections();
    [[nodiscard]] core::Status validate() const;
};

struct ChunkMeshingContext {
    const VoxelChunk& chunk;
    const VoxelPalette* palette = nullptr;
    const ChunkDatabase* chunks = nullptr;
    std::uint16_t available_halo_radius = 1;
};

class ChunkMesher {
  public:
    [[nodiscard]] static core::Result<ChunkMesh> build_surface_mesh(const VoxelChunk& chunk);
    [[nodiscard]] static core::Result<ChunkMesh>
    build_surface_mesh(const ChunkMeshingContext& context);
    [[nodiscard]] static core::Result<ChunkMesh>
    build_surface_mesh(const ChunkNeighborhoodSnapshot& neighborhood,
                       const BlockRenderTableSnapshot& render_table);
    [[nodiscard]] static core::Result<ChunkMesh>
    build_specialized_surface_mesh(const ChunkNeighborhoodSnapshot& neighborhood,
                                   const BlockRenderTableSnapshot& render_table);
};

[[nodiscard]] math::Vec3f chunk_mesh_face_normal(ChunkMeshFaceDirection direction) noexcept;

} // namespace heartstead::world
