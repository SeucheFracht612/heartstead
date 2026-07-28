#include "engine/world/fluids/fluid_state.hpp"
#include "engine/world/regions/region_graph.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/worldgen/terrain_generator.hpp"

#include <cassert>
#include <string_view>

namespace {

[[nodiscard]] heartstead::core::PrototypeId id(std::string_view value) {
    auto parsed = heartstead::core::PrototypeId::parse(value);
    assert(parsed);
    return parsed.value();
}

struct FluidFixture {
    heartstead::world::VoxelPalette palette;
    heartstead::world::RegionGraph regions;
    heartstead::core::PrototypeId stone_id = id("test:voxels/stone");
    heartstead::core::PrototypeId water_id = id("test:voxels/water");

    FluidFixture() {
        heartstead::world::VoxelDefinition stone;
        stone.type = 1;
        stone.prototype_id = stone_id;
        stone.display_name = "Stone";
        stone.terrain_material = "stone";
        stone.mining_tool = "pickaxe";
        assert(palette.add(std::move(stone)));

        heartstead::world::VoxelDefinition water;
        water.type = 2;
        water.prototype_id = water_id;
        water.display_name = "Water";
        water.terrain_material = "water";
        water.mining_tool = "bucket";
        water.logical_occupancy = heartstead::world::BlockLogicalOccupancy::fluid;
        water.collision_bounds.clear();
        water.occlusion_bounds.clear();
        water.occlusion = heartstead::world::BlockOcclusionBehavior::none;
        water.light_absorption = 8;
        assert(palette.add(std::move(water)));

        heartstead::world::RegionDescriptor region;
        region.id = "test_ocean";
        region.age = "age_0";
        region.biome_cluster = "coast";
        region.resource_rules.push_back({stone_id, "terrain", 1.0});
        assert(regions.add_region(std::move(region)));
    }
};

void test_fluid_state_round_trip_and_rejects_reserved_state() {
    using namespace heartstead::world;
    const FluidState expected{5, true, false, FluidFlowDirection::positive_z};
    auto encoded = encode_fluid_state(expected);
    assert(encoded);
    assert((encoded.value() & fluid_amount_mask) == 5);
    assert((encoded.value() & fluid_falling_bit) != 0);
    assert(decode_fluid_state(encoded.value()).value() == expected);

    auto partial_source =
        encode_fluid_state({7, false, true, FluidFlowDirection::none});
    assert(!partial_source);
    assert(partial_source.error().code == "fluid_state.partial_source");
    auto reserved = decode_fluid_state(0x8008U);
    assert(!reserved);
    assert(reserved.error().code == "fluid_state.reserved_bits");
    assert(fluid_surface_height(1) == 0.125F);
    assert(fluid_surface_height(maximum_fluid_amount) == 1.0F);
}

void test_palette_creates_full_finite_fluid_cells() {
    using namespace heartstead::world;
    FluidFixture fixture;
    auto water = fixture.palette.cell_for(fixture.water_id, 91);
    assert(water);
    assert(water.value().light == 91);
    auto state = decode_fluid_cell(water.value(), fixture.palette);
    assert(state);
    assert((state.value() == FluidState{maximum_fluid_amount, false, false,
                                       FluidFlowDirection::none}));

    auto invalid = water.value();
    invalid.metadata_handle = 4;
    assert(!decode_fluid_cell(invalid, fixture.palette));
    assert(!decode_fluid_cell({1, 0, full_fluid_state_bits()}, fixture.palette));
}

void test_worldgen_seeds_ocean_sources_below_sea_level() {
    using namespace heartstead::world;
    FluidFixture fixture;
    TerrainGenerationConfig config;
    config.world_seed = 42;
    config.region_id = "test_ocean";
    config.base_surface_y = 1;
    config.sea_level = 4;
    config.ocean_voxel_id = fixture.water_id;
    auto generated = DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, config, fixture.regions, fixture.palette);
    assert(generated);
    assert(generated.value().get({0, 1, 0}).value().type == 1);
    for (std::uint16_t y = 2; y <= 4; ++y) {
        const auto cell = generated.value().get({0, y, 0}).value();
        assert(cell.type == 2);
        const auto state = decode_fluid_cell(cell, fixture.palette);
        assert(state);
        assert(state.value().source);
        assert(state.value().amount == maximum_fluid_amount);
    }
    assert(generated.value().get({0, 5, 0}).value().is_air());

    config.ocean_voxel_id = fixture.stone_id;
    auto invalid = DeterministicTerrainGenerator::generate_chunk(
        {0, 0, 0}, config, fixture.regions, fixture.palette);
    assert(!invalid);
    assert(invalid.error().code == "terrain_generator.invalid_ocean_voxel");
}

} // namespace

int main() {
    test_fluid_state_round_trip_and_rejects_reserved_state();
    test_palette_creates_full_finite_fluid_cells();
    test_worldgen_seeds_ocean_sources_below_sea_level();
    return 0;
}
