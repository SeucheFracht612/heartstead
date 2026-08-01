#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/voxels/voxel_mask.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

namespace world = heartstead::world;

void assert_mask_matches(const world::VoxelChunk& chunk) {
    assert(chunk.occupancy().content_revision() == chunk.content_revision());
    assert(chunk.occupancy().validate_against(chunk.cells(), chunk.content_revision()));
    const auto counted = static_cast<std::size_t>(
        std::ranges::count_if(chunk.cells(), [](world::VoxelCell cell) { return !cell.is_air(); }));
    assert(chunk.occupancy().occupied_count() == counted);
    assert(chunk.occupancy().empty() == (counted == 0));
    assert(chunk.occupancy().full() == (counted == world::VoxelChunk::total_cells));
}

void test_incremental_edits_track_content_revision() {
    world::VoxelChunk chunk({3, -2, 7});
    static_assert(world::VoxelOccupancyMask::payload_bytes == 4'096);
    static_assert(world::VoxelOccupancyMask::cell_count == world::VoxelChunk::total_cells);
    assert_mask_matches(chunk);
    assert(!chunk.occupancy().occupied(0));
    assert(!chunk.occupancy().occupied({32, 0, 0}));

    const auto initial_revision = chunk.content_revision();
    assert(chunk.set({0, 0, 0}, {4, 17, 2, 99}));
    assert(chunk.content_revision() == initial_revision + 1U);
    assert(chunk.occupancy().occupied(0));
    assert(chunk.occupancy().occupied({0, 0, 0}));
    assert(chunk.occupancy().occupied_count() == 1);
    assert_mask_matches(chunk);

    const auto occupied_revision = chunk.content_revision();
    assert(chunk.set({0, 0, 0}, {9, 200, 7, 0}));
    assert(chunk.content_revision() == occupied_revision + 1U);
    assert(chunk.occupancy().occupied_count() == 1);
    assert_mask_matches(chunk);

    const auto no_change_revision = chunk.content_revision();
    assert(chunk.set({0, 0, 0}, {9, 200, 7, 0}));
    assert(chunk.content_revision() == no_change_revision);
    assert_mask_matches(chunk);

    assert(chunk.set({0, 0, 0}, world::VoxelCell::air()));
    assert_mask_matches(chunk);
    assert(chunk.occupancy().empty());

    assert(chunk.set({31, 31, 31}, {12, 255}));
    assert(chunk.occupancy().occupied(world::VoxelChunk::total_cells - 1U));
    assert(chunk.apply_saved_cell({10, 11, 12}, {8, 0}));
    assert(chunk.occupancy().occupied({10, 11, 12}));
    assert_mask_matches(chunk);
}

void test_bulk_load_fill_and_light_retag_masks() {
    world::VoxelChunk chunk({0, 0, 0});
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells);
    std::size_t expected_count = 0;
    for (std::size_t index = 0; index < cells.size(); index += 65U) {
        cells[index] = {static_cast<std::uint16_t>((index % 31U) + 1U),
                        static_cast<std::uint8_t>(index), static_cast<std::uint16_t>(index), 0};
        ++expected_count;
    }
    assert(chunk.load_generated_cells(cells));
    assert(chunk.occupancy().occupied_count() == expected_count);
    assert_mask_matches(chunk);

    std::vector<std::uint8_t> light(world::VoxelChunk::total_cells, 123);
    const auto before_light_revision = chunk.content_revision();
    auto changed = chunk.apply_derived_light(light);
    assert(changed);
    assert(changed.value() > 0);
    assert(chunk.content_revision() == before_light_revision + 1U);
    assert(chunk.occupancy().occupied_count() == expected_count);
    assert_mask_matches(chunk);

    chunk.fill({5, 77, 3, 0});
    assert(chunk.occupancy().full());
    assert_mask_matches(chunk);
    const auto full_revision = chunk.content_revision();
    chunk.fill({5, 77, 3, 0});
    assert(chunk.content_revision() == full_revision);
    assert_mask_matches(chunk);

    chunk.fill({0, 44, 0, 0});
    assert(chunk.occupancy().empty());
    assert_mask_matches(chunk);

    const auto copied = chunk;
    assert_mask_matches(copied);
    assert(std::ranges::equal(copied.occupancy().words(), chunk.occupancy().words()));
}

void test_validation_rejects_wrong_revision_or_cell_count() {
    world::VoxelChunk chunk({0, 0, 0});
    assert(!chunk.occupancy().validate_against(chunk.cells(), chunk.content_revision() + 1U));
    assert(!chunk.occupancy().validate_against(chunk.cells().first(1), chunk.content_revision()));
}

} // namespace

int main() {
    test_incremental_edits_track_content_revision();
    test_bulk_load_fill_and_light_retag_masks();
    test_validation_rejects_wrong_revision_or_cell_count();
    return 0;
}
