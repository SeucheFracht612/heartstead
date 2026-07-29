#include "engine/content/content_validation.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "game/foundation/foundation_world.hpp"

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <ranges>
#include <string_view>

namespace {

using namespace heartstead;

[[nodiscard]] world::VoxelCell cell_at(const world::ChunkDatabase& chunks,
                                       world::BlockCoord block) {
    const auto address = world::block_to_chunk_local(block);
    auto cell = chunks.get(address.chunk, address.local);
    assert(cell);
    return cell.value();
}

[[nodiscard]] core::PrototypeId id(std::string_view value) {
    auto parsed = core::PrototypeId::parse(value);
    assert(parsed);
    return parsed.value();
}

void test_foundation_world_is_deterministic_and_voxel_native() {
    const auto source_root = std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
    const auto content = content::ContentValidation::validate(source_root);
    assert(!content.has_errors());

    world::ChunkDatabase first;
    world::ChunkDatabase second;
    const auto first_build = game::foundation::build_world(first, content.voxel_palette);
    const auto second_build = game::foundation::build_world(second, content.voxel_palette);
    assert(first_build && second_build);
    assert(first_build.value().voxel_writes == second_build.value().voxel_writes);
    assert(first_build.value().chunk_count == 2);
    assert(first_build.value().chunk_count == second_build.value().chunk_count);

    const auto first_records = first.records();
    const auto second_records = second.records();
    assert(first_records.size() == second_records.size());
    for (std::size_t index = 0; index < first_records.size(); ++index) {
        assert(first_records[index]->coord() == second_records[index]->coord());
        assert(std::ranges::equal(first_records[index]->cells(), second_records[index]->cells()));
    }

    const auto grass_type = content.voxel_palette.type_for(id("base:voxels/grass"));
    const auto dirt_type = content.voxel_palette.type_for(id("base:voxels/dirt"));
    const auto stone_type = content.voxel_palette.type_for(id("base:voxels/stone"));
    const auto step_type = content.voxel_palette.type_for(id("base:voxels/foundation_half_step"));
    const auto ceiling_type =
        content.voxel_palette.type_for(id("base:voxels/foundation_low_ceiling"));
    const auto water_type = content.voxel_palette.type_for(id("base:voxels/water"));
    assert(grass_type && dirt_type && stone_type && step_type && ceiling_type && water_type);

    const auto spawn = game::foundation::spawn_position();
    assert(spawn == (world::WorldPosition{8.5, 1.0, 8.5}));
    assert(cell_at(first, {8, 0, 8}).type == *grass_type);
    for (std::int64_t x = 6; x <= 12; ++x) {
        for (std::int64_t y = 1; y <= 4; ++y) {
            for (std::int64_t z = 6; z <= 12; ++z) {
                assert(cell_at(first, {x, y, z}).is_air());
            }
        }
    }

    assert(cell_at(first, {3, 1, 14}).type == *step_type);
    assert(cell_at(first, {3, 1, 15}).type == *stone_type);
    assert(cell_at(first, {3, 2, 16}).type == *step_type);
    assert(cell_at(first, {3, 2, 18}).type == *stone_type);
    const auto* step = content.voxel_palette.find_by_type(*step_type);
    assert(step != nullptr);
    assert(step->logical_occupancy == world::BlockLogicalOccupancy::partial);
    assert(step->collision_bounds.size() == 1);
    assert(step->collision_bounds.front().max.y == 0.5F);

    assert(cell_at(first, {15, 2, 16}).type == *ceiling_type);
    const auto* ceiling = content.voxel_palette.find_by_type(*ceiling_type);
    assert(ceiling != nullptr);
    assert(ceiling->collision_bounds.front().min.y == 0.5F);

    assert(cell_at(first, {20, 0, 15}).is_air());
    assert(cell_at(first, {20, -1, 15}).is_air());
    assert(cell_at(first, {20, -2, 15}).type == *stone_type);

    const auto water = cell_at(first, {25, 0, 15});
    assert(water.type == *water_type);
    assert(water.state_bits == world::full_fluid_state_bits());
    assert(cell_at(first, {25, -1, 15}).type == *water_type);

    assert(cell_at(first, {3, 0, 2}).type == *dirt_type);
    assert(cell_at(first, game::foundation::boundary_edit_upper).type == *grass_type);
    assert(cell_at(first, game::foundation::boundary_edit_lower).type == *dirt_type);
    assert(world::chunk_coord_for_block(game::foundation::boundary_edit_upper) ==
           (world::ChunkCoord{0, 0, 0}));
    assert(world::chunk_coord_for_block(game::foundation::boundary_edit_lower) ==
           (world::ChunkCoord{0, -1, 0}));
    assert(cell_at(first, {27, 0, 7}).type != *grass_type);
    assert(cell_at(first, {31, 0, 7}).type != *grass_type);
}

} // namespace

int main() {
    test_foundation_world_is_deterministic_and_voxel_native();
    return 0;
}
