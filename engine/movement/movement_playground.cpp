#include "engine/movement/movement_playground.hpp"

#include "engine/core/ids.hpp"
#include "engine/world/fluids/fluid_state.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace heartstead::movement {

namespace {

enum PlaygroundVoxel : std::uint16_t {
    solid = 1,
    half_step = 2,
    water = 3,
    ladder = 4,
    crouch_ceiling = 5,
    roll_ceiling = 6,
    marker = 7,
};

[[nodiscard]] core::Result<world::VoxelDefinition>
make_voxel(std::uint16_t type, std::string local_id, std::string display_name) {
    auto id = core::PrototypeId::parse("playground:voxels/" + local_id);
    if (!id) {
        return core::Result<world::VoxelDefinition>::failure(
            "movement_playground.invalid_voxel_id", "playground voxel id is invalid");
    }
    world::VoxelDefinition result;
    result.type = type;
    result.prototype_id = id.value();
    result.display_name = std::move(display_name);
    result.terrain_material = "debug";
    result.mining_tool = "hand";
    result.tags = {"playground"};
    return core::Result<world::VoxelDefinition>::success(std::move(result));
}

[[nodiscard]] core::Status set_block(MovementPlaygroundWorld& yard, world::BlockCoord block,
                                     std::uint16_t type) {
    const auto location = world::block_to_chunk_local(block);
    auto& chunk = yard.chunks.get_or_create(location.chunk);
    const auto state_bits =
        type == water ? world::full_fluid_state_bits() : std::uint16_t{0};
    return chunk.set(location.local, {type, 15, state_bits, 0});
}

[[nodiscard]] core::Status fill_box(MovementPlaygroundWorld& yard, world::BlockCoord minimum,
                                    world::BlockCoord maximum, std::uint16_t type) {
    for (auto x = minimum.x; x <= maximum.x; ++x) {
        for (auto y = minimum.y; y <= maximum.y; ++y) {
            for (auto z = minimum.z; z <= maximum.z; ++z) {
                auto status = set_block(yard, {x, y, z}, type);
                if (!status) {
                    return status;
                }
            }
        }
    }
    return core::Status::ok();
}

void add_station(MovementPlaygroundWorld& yard, std::string name, double x, double y, double z,
                 std::string purpose) {
    yard.stations.push_back(
        {std::move(name), world::WorldPosition{x, y, z}, std::move(purpose)});
}

} // namespace

core::Result<MovementPlaygroundWorld> MovementPlaygroundBuilder::build() {
    MovementPlaygroundWorld yard;
    auto solid_voxel = make_voxel(solid, "solid", "Yard Solid");
    auto step_voxel = make_voxel(half_step, "half_step", "Half Step");
    auto water_voxel = make_voxel(water, "water", "Water");
    auto ladder_voxel = make_voxel(ladder, "ladder", "Ladder");
    auto crouch_voxel = make_voxel(crouch_ceiling, "crouch_ceiling", "Crouch Ceiling");
    auto roll_voxel = make_voxel(roll_ceiling, "roll_ceiling", "Roll Ceiling");
    auto marker_voxel = make_voxel(marker, "marker", "Distance Marker");
    if (!solid_voxel || !step_voxel || !water_voxel || !ladder_voxel || !crouch_voxel ||
        !roll_voxel || !marker_voxel) {
        return core::Result<MovementPlaygroundWorld>::failure(
            "movement_playground.voxel_creation_failed", "failed to create playground voxels");
    }
    step_voxel.value().logical_occupancy = world::BlockLogicalOccupancy::partial;
    step_voxel.value().occlusion = world::BlockOcclusionBehavior::model;
    step_voxel.value().collision_bounds = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.5F, 1.0F}}};
    step_voxel.value().occlusion_bounds = step_voxel.value().collision_bounds;
    water_voxel.value().logical_occupancy = world::BlockLogicalOccupancy::fluid;
    water_voxel.value().occlusion = world::BlockOcclusionBehavior::none;
    water_voxel.value().collision_bounds.clear();
    water_voxel.value().occlusion_bounds.clear();
    water_voxel.value().tags.push_back("water");
    ladder_voxel.value().logical_occupancy = world::BlockLogicalOccupancy::decorative;
    ladder_voxel.value().occlusion = world::BlockOcclusionBehavior::none;
    ladder_voxel.value().collision_bounds.clear();
    ladder_voxel.value().occlusion_bounds.clear();
    ladder_voxel.value().tags.push_back("ladder");
    crouch_voxel.value().logical_occupancy = world::BlockLogicalOccupancy::partial;
    crouch_voxel.value().occlusion = world::BlockOcclusionBehavior::model;
    crouch_voxel.value().collision_bounds = {{{0.0F, 0.4F, 0.0F}, {1.0F, 1.0F, 1.0F}}};
    crouch_voxel.value().occlusion_bounds = crouch_voxel.value().collision_bounds;
    roll_voxel.value().logical_occupancy = world::BlockLogicalOccupancy::partial;
    roll_voxel.value().occlusion = world::BlockOcclusionBehavior::model;
    roll_voxel.value().collision_bounds = {{{0.0F, 0.05F, 0.0F}, {1.0F, 1.0F, 1.0F}}};
    roll_voxel.value().occlusion_bounds = roll_voxel.value().collision_bounds;
    marker_voxel.value().logical_occupancy = world::BlockLogicalOccupancy::partial;
    marker_voxel.value().occlusion = world::BlockOcclusionBehavior::none;
    marker_voxel.value().collision_bounds.clear();
    marker_voxel.value().occlusion_bounds.clear();

    for (auto* definition : {&solid_voxel.value(), &step_voxel.value(), &water_voxel.value(),
                             &ladder_voxel.value(), &crouch_voxel.value(), &roll_voxel.value(),
                             &marker_voxel.value()}) {
        auto status = yard.palette.add(std::move(*definition));
        if (!status) {
            return core::Result<MovementPlaygroundWorld>::failure(status.error().code,
                                                                  status.error().message);
        }
    }

    auto status = fill_box(yard, {-16, 0, -16}, {95, 0, 63}, solid);
    if (!status) {
        return core::Result<MovementPlaygroundWorld>::failure(status.error().code,
                                                              status.error().message);
    }
    // Acceleration lane markers, one metre apart.
    for (std::int64_t z = 5; z <= 45; z += 5) {
        status = fill_box(yard, {-1, 1, z}, {1, 1, z}, marker);
        if (!status) {
            return core::Result<MovementPlaygroundWorld>::failure(status.error().code,
                                                                  status.error().message);
        }
    }
    // Step and vault lane.
    status = fill_box(yard, {8, 1, 12}, {10, 1, 12}, half_step);
    if (status) {
        status = fill_box(yard, {8, 1, 20}, {10, 1, 20}, solid);
    }
    // Mantle wall set: 1 m, 2 m and 2.5+/3 m deliberate failure.
    if (status) {
        status = fill_box(yard, {18, 1, 15}, {21, 1, 15}, solid);
    }
    if (status) {
        status = fill_box(yard, {28, 1, 15}, {31, 2, 15}, solid);
    }
    if (status) {
        status = fill_box(yard, {38, 1, 15}, {41, 3, 15}, solid);
    }
    // Crouch and roll tunnels.
    if (status) {
        status = fill_box(yard, {48, 2, 8}, {51, 2, 18}, crouch_ceiling);
    }
    if (status) {
        status = fill_box(yard, {56, 2, 8}, {59, 2, 18}, roll_ceiling);
    }
    // Water pool and exit step.
    if (status) {
        status = fill_box(yard, {66, 1, 8}, {75, 2, 18}, water);
    }
    // Ladder backed by a tower.
    if (status) {
        status = fill_box(yard, {82, 1, 8}, {84, 10, 10}, solid);
    }
    if (status) {
        status = fill_box(yard, {81, 1, 9}, {81, 9, 9}, ladder);
    }
    // Fall/landing-roll platforms at 5 m and 9 m.
    if (status) {
        status = fill_box(yard, {66, 5, 30}, {72, 5, 36}, solid);
    }
    if (status) {
        status = fill_box(yard, {78, 9, 30}, {84, 9, 36}, solid);
    }
    if (!status) {
        return core::Result<MovementPlaygroundWorld>::failure(status.error().code,
                                                              status.error().message);
    }

    yard.spawn = world::WorldPosition{0.5, 1.0, 1.5};
    add_station(yard, "acceleration_lane", 0.5, 1.0, 4.5,
                "walk/sprint acceleration and marked speed calibration");
    add_station(yard, "step_lane", 9.5, 1.0, 8.5, "0.5 m auto-step then 1 m vault");
    add_station(yard, "mantle_1m", 19.5, 1.0, 10.5, "one metre mantle wall");
    add_station(yard, "mantle_2m", 29.5, 1.0, 10.5, "two metre maximum mantle wall");
    add_station(yard, "mantle_must_fail", 39.5, 1.0, 10.5,
                "three metre wall must reject mantle");
    add_station(yard, "crouch_tunnel", 49.5, 1.0, 6.5, "crouch clearance and no-edge-drop");
    add_station(yard, "roll_tunnel", 57.5, 1.0, 6.5, "roll-only one metre clearance");
    add_station(yard, "swim_pool", 70.5, 1.0, 6.5, "swimming and stamina drain");
    add_station(yard, "ladder_tower", 79.5, 1.0, 9.5, "ladder-only climbing");
    add_station(yard, "fall_towers", 70.0, 1.0, 27.0,
                "fall threshold and landing-roll mitigation");
    add_station(yard, "laden_trudge", 0.5, 1.0, 50.5,
                "load-tier speed, roll and regeneration presets");
    add_station(yard, "rope_rig_reserved", 12.5, 1.0, 50.5,
                "reserved visual fixture; rope climbing intentionally not implemented");
    return core::Result<MovementPlaygroundWorld>::success(std::move(yard));
}

} // namespace heartstead::movement
