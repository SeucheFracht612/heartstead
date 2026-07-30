#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/meshing/chunk_mesher.hpp"
#include "engine/world/meshing/greedy_chunk_mesher.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

namespace {

using namespace heartstead;

[[nodiscard]] double triangle_area(const math::Vec3f& a, const math::Vec3f& b,
                                   const math::Vec3f& c) {
    const auto ab = b - a;
    const auto ac = c - a;
    const math::Vec3f cross{ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z,
                            ab.x * ac.y - ab.y * ac.x};
    return 0.5 * std::sqrt(static_cast<double>(cross.x * cross.x + cross.y * cross.y +
                                               cross.z * cross.z));
}

[[nodiscard]] std::array<double, 6> directional_surface_area(const world::ChunkMesh& mesh) {
    std::array<double, 6> result{};
    for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
        const auto& a = mesh.vertices[mesh.indices[index]];
        const auto& b = mesh.vertices[mesh.indices[index + 1]];
        const auto& c = mesh.vertices[mesh.indices[index + 2]];
        std::size_t direction = 0;
        if (a.normal.x > 0.5F) {
            direction = 1;
        } else if (a.normal.y < -0.5F) {
            direction = 2;
        } else if (a.normal.y > 0.5F) {
            direction = 3;
        } else if (a.normal.z < -0.5F) {
            direction = 4;
        } else if (a.normal.z > 0.5F) {
            direction = 5;
        }
        result[direction] += triangle_area(a.position, b.position, c.position);
    }
    return result;
}

struct MeshPair {
    world::ChunkMesh reference;
    world::ChunkMesh greedy;
};

[[nodiscard]] MeshPair build_pair(world::ChunkDatabase& chunks, world::ChunkIdentity identity) {
    auto table = world::build_block_render_table_snapshot(nullptr);
    assert(table);
    auto snapshot = world::build_chunk_neighborhood_snapshot(chunks, identity, table.value());
    assert(snapshot);
    auto reference = world::ChunkMesher::build_surface_mesh(snapshot.value(), table.value());
    auto greedy = world::GreedyChunkMesher::build_surface_mesh(snapshot.value(), table.value());
    assert(reference);
    assert(greedy);
    assert(reference.value().validate());
    assert(greedy.value().validate());
    return {std::move(reference).value(), std::move(greedy).value()};
}

void test_rectangular_volume_merges_to_six_quads() {
    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({-12, 4, -9});
    for (std::uint16_t z = 0; z < 2; ++z) {
        for (std::uint16_t y = 0; y < 3; ++y) {
            for (std::uint16_t x = 0; x < 4; ++x) {
                assert(chunk.set({x, y, z}, {7, 180, 3}));
            }
        }
    }
    const auto meshes = build_pair(chunks, chunk.identity());
    assert(meshes.reference.face_count == 52);
    assert(meshes.greedy.face_count == 6);
    assert(meshes.greedy.vertices.size() == 24);
    assert(meshes.greedy.indices.size() == 36);
    assert(meshes.greedy.sections.size() == 1);
    assert(meshes.greedy.sections.front().material_index == 7);
    assert(meshes.greedy.sections.front().render_phase == world::MeshingRenderPhase::opaque);
    assert(meshes.greedy.sections.front().first_index == 0);
    assert(meshes.greedy.sections.front().index_count == meshes.greedy.indices.size());
    assert(meshes.greedy.chunk_coord == chunk.coord());
    const auto reference_area = directional_surface_area(meshes.reference);
    const auto greedy_area = directional_surface_area(meshes.greedy);
    for (std::size_t direction = 0; direction < reference_area.size(); ++direction) {
        assert(std::abs(reference_area[direction] - greedy_area[direction]) < 0.0001);
    }
}

void test_material_sections_are_grouped_and_cover_indices() {
    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create({-3, 2, 8});
    assert(chunk.set({0, 0, 0}, {1, 200, 4}));
    assert(chunk.set({1, 0, 0}, {2, 200, 4}));
    auto meshes = build_pair(chunks, chunk.identity());
    assert(meshes.greedy.sections.size() == 2);
    assert(meshes.greedy.sections[0].material_index == 1);
    assert(meshes.greedy.sections[1].material_index == 2);
    assert(meshes.greedy.sections[0].first_index == 0);
    assert(meshes.greedy.sections[1].first_index == meshes.greedy.sections[0].index_count);
    assert(meshes.greedy.sections[0].index_count + meshes.greedy.sections[1].index_count ==
           meshes.greedy.indices.size());

    auto invalid = meshes.greedy;
    invalid.sections.back().first_index += 1;
    assert(!invalid.validate());
}

void test_merge_compatibility_respects_light_material_and_state() {
    const auto face_count_for = [](world::VoxelCell left, world::VoxelCell right) {
        world::ChunkDatabase chunks;
        auto& chunk = chunks.get_or_create({0, 0, 0});
        assert(chunk.set({0, 0, 0}, left));
        assert(chunk.set({1, 0, 0}, right));
        return build_pair(chunks, chunk.identity()).greedy.face_count;
    };
    assert(face_count_for({1, 200, 4}, {1, 200, 4}) == 6);
    assert(face_count_for({1, 200, 4}, {1, 150, 4}) == 10);
    assert(face_count_for({1, 200, 4}, {2, 200, 4}) == 10);
    assert(face_count_for({1, 200, 4}, {1, 200, 5}) == 10);
}

void test_checkerboard_and_cross_chunk_occlusion_match_reference() {
    world::ChunkDatabase chunks;
    auto& center = chunks.get_or_create({-1, 0, -1});
    for (std::uint16_t z = 0; z < 4; ++z) {
        for (std::uint16_t y = 0; y < 4; ++y) {
            for (std::uint16_t x = 0; x < 4; ++x) {
                if ((x + y + z) % 2U == 0) {
                    assert(center.set({x, y, z}, {3, 255}));
                }
            }
        }
    }
    auto checkerboard = build_pair(chunks, center.identity());
    assert(checkerboard.reference.face_count == checkerboard.greedy.face_count);
    assert(directional_surface_area(checkerboard.reference) ==
           directional_surface_area(checkerboard.greedy));

    auto& boundary_center = chunks.get_or_create({4, -2, 7});
    auto& neighbor = chunks.get_or_create({5, -2, 7});
    assert(boundary_center.set({31, 5, 6}, {9, 255}));
    assert(neighbor.set({0, 5, 6}, {9, 255}));
    const auto boundary = build_pair(chunks, boundary_center.identity());
    assert(boundary.reference.face_count == 5);
    assert(boundary.greedy.face_count == 5);
    assert(directional_surface_area(boundary.reference) ==
           directional_surface_area(boundary.greedy));
}

void test_voxel_ao_matches_reference_across_chunk_boundary() {
    world::ChunkDatabase chunks;
    auto& center = chunks.get_or_create({12, 0, -4});
    auto& east = chunks.get_or_create({13, 0, -4});
    assert(center.set({31, 1, 1}, {7, 255}));
    // Three full occluders surround one top-face corner, including two samples from the east
    // chunk halo. This is the strongest classic voxel-AO configuration.
    assert(east.set({0, 2, 1}, {9, 255}));
    assert(center.set({31, 2, 0}, {9, 255}));
    assert(east.set({0, 2, 0}, {9, 255}));
    const auto meshes = build_pair(chunks, center.identity());
    const auto top_ao = [](const world::ChunkMesh& mesh) {
        std::vector<std::uint8_t> values;
        for (const auto& vertex : mesh.vertices) {
            if (vertex.voxel_type == 7 && vertex.normal.y > 0.5F &&
                std::abs(vertex.position.y - 2.0F) < 0.0001F) {
                values.push_back(vertex.ambient_occlusion);
            }
        }
        std::ranges::sort(values);
        return values;
    };
    const auto reference = top_ao(meshes.reference);
    const auto greedy = top_ao(meshes.greedy);
    assert(reference.size() == 4);
    assert(greedy == reference);
    assert(reference.front() == 112);
    assert(reference.back() == 255);
}

} // namespace

int main() {
    test_rectangular_volume_merges_to_six_quads();
    test_merge_compatibility_respects_light_material_and_state();
    test_material_sections_are_grouped_and_cover_indices();
    test_checkerboard_and_cross_chunk_occlusion_match_reference();
    test_voxel_ao_matches_reference_across_chunk_boundary();
    return 0;
}
