#include "engine/renderer/benchmark/benchmark_scene.hpp"

#include "engine/core/ids.hpp"
#include "engine/world/blocks/block_model.hpp"
#include "engine/world/coords/world_coords.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "engine/world/voxels/voxel_surface_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace heartstead::renderer::benchmark {

namespace {

constexpr std::uint16_t stone_type = 1;
constexpr std::uint16_t surface_type = 2;
constexpr std::uint16_t soil_type = 3;
constexpr std::uint16_t foliage_type = 4;
constexpr std::uint16_t water_type = 5;
constexpr std::uint16_t slope_type = 6;

[[nodiscard]] std::uint64_t mix_hash(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t coordinate_hash(std::int64_t x, std::int64_t y, std::int64_t z,
                                            std::uint64_t seed) noexcept {
    auto value = mix_hash(static_cast<std::uint64_t>(x) ^ seed);
    value ^= mix_hash(static_cast<std::uint64_t>(y) + 0x9e3779b97f4a7c15ULL);
    value ^= mix_hash(static_cast<std::uint64_t>(z) + 0xd1b54a32d192ed03ULL);
    return mix_hash(value);
}

[[nodiscard]] std::size_t cell_index(std::uint16_t x, std::uint16_t y, std::uint16_t z) noexcept {
    constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    return static_cast<std::size_t>(z) * edge * edge + static_cast<std::size_t>(y) * edge +
           static_cast<std::size_t>(x);
}

[[nodiscard]] core::Result<core::PrototypeId> prototype(std::string_view value) {
    auto parsed = core::PrototypeId::parse(value);
    if (!parsed) {
        return core::Result<core::PrototypeId>::failure("renderer.benchmark_invalid_prototype",
                                                        "invalid built-in benchmark prototype id");
    }
    return core::Result<core::PrototypeId>::success(std::move(*parsed));
}

[[nodiscard]] BenchmarkSceneKind terrain_shape_for(BenchmarkSceneKind kind) noexcept {
    switch (kind) {
    case BenchmarkSceneKind::mountainous_terrain:
    case BenchmarkSceneKind::dense_caves:
    case BenchmarkSceneKind::checkerboard_geometry:
    case BenchmarkSceneKind::forest_cross_planes:
    case BenchmarkSceneKind::terrain_material_preview:
    case BenchmarkSceneKind::starting_biome:
    case BenchmarkSceneKind::character_workshop:
        return kind;
    case BenchmarkSceneKind::flat_terrain:
    case BenchmarkSceneKind::rapid_voxel_edits:
    case BenchmarkSceneKind::mass_excavation:
    case BenchmarkSceneKind::high_speed_flythrough:
    case BenchmarkSceneKind::chunk_load_unload_churn:
    case BenchmarkSceneKind::large_coordinates:
    case BenchmarkSceneKind::resize_minimize_stress:
    case BenchmarkSceneKind::active_water:
    case BenchmarkSceneKind::particle_stress:
    case BenchmarkSceneKind::light_heavy_settlement:
        return BenchmarkSceneKind::flat_terrain;
    }
    return BenchmarkSceneKind::flat_terrain;
}

} // namespace

std::string_view benchmark_scene_name(BenchmarkSceneKind kind) noexcept {
    switch (kind) {
    case BenchmarkSceneKind::flat_terrain:
        return "flat";
    case BenchmarkSceneKind::mountainous_terrain:
        return "mountains";
    case BenchmarkSceneKind::dense_caves:
        return "caves";
    case BenchmarkSceneKind::checkerboard_geometry:
        return "checkerboard";
    case BenchmarkSceneKind::forest_cross_planes:
        return "forest";
    case BenchmarkSceneKind::rapid_voxel_edits:
        return "rapid-edits";
    case BenchmarkSceneKind::mass_excavation:
        return "mass-excavation";
    case BenchmarkSceneKind::high_speed_flythrough:
        return "flythrough";
    case BenchmarkSceneKind::chunk_load_unload_churn:
        return "churn";
    case BenchmarkSceneKind::large_coordinates:
        return "large-coordinates";
    case BenchmarkSceneKind::resize_minimize_stress:
        return "resize-minimize";
    case BenchmarkSceneKind::active_water:
        return "active-water";
    case BenchmarkSceneKind::particle_stress:
        return "particles";
    case BenchmarkSceneKind::light_heavy_settlement:
        return "light-heavy";
    case BenchmarkSceneKind::terrain_material_preview:
        return "terrain-materials";
    case BenchmarkSceneKind::starting_biome:
        return "starting-biome";
    case BenchmarkSceneKind::character_workshop:
        return "character-workshop";
    }
    return "unknown";
}

std::optional<BenchmarkSceneKind> parse_benchmark_scene(std::string_view name) noexcept {
    constexpr std::array kinds{
        BenchmarkSceneKind::flat_terrain,
        BenchmarkSceneKind::mountainous_terrain,
        BenchmarkSceneKind::dense_caves,
        BenchmarkSceneKind::checkerboard_geometry,
        BenchmarkSceneKind::forest_cross_planes,
        BenchmarkSceneKind::rapid_voxel_edits,
        BenchmarkSceneKind::mass_excavation,
        BenchmarkSceneKind::high_speed_flythrough,
        BenchmarkSceneKind::chunk_load_unload_churn,
        BenchmarkSceneKind::large_coordinates,
        BenchmarkSceneKind::resize_minimize_stress,
        BenchmarkSceneKind::active_water,
        BenchmarkSceneKind::particle_stress,
        BenchmarkSceneKind::light_heavy_settlement,
        BenchmarkSceneKind::terrain_material_preview,
        BenchmarkSceneKind::starting_biome,
        BenchmarkSceneKind::character_workshop,
    };
    const auto found = std::ranges::find_if(
        kinds, [name](BenchmarkSceneKind kind) { return benchmark_scene_name(kind) == name; });
    return found == kinds.end() ? std::nullopt : std::optional<BenchmarkSceneKind>{*found};
}

core::Status BenchmarkSceneConfig::validate() const {
    if (chunk_radius > 8) {
        return core::Status::failure("renderer.benchmark_radius_too_large",
                                     "benchmark chunk radius must be in the range 0..8");
    }
    if (!initial_extent.is_valid()) {
        return core::Status::failure("renderer.benchmark_invalid_extent",
                                     "benchmark initial extent must be nonzero");
    }
    return core::Status::ok();
}

BenchmarkScene::BenchmarkScene(BenchmarkSceneConfig config) : config_(config) {
    if (config_.kind == BenchmarkSceneKind::large_coordinates) {
        center_chunk_ = {1'000'000'000, 0, -1'000'000'000};
    }
}

core::Result<std::unique_ptr<BenchmarkScene>> BenchmarkScene::create(BenchmarkSceneConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<BenchmarkScene>>::failure(status.error().code,
                                                                      status.error().message);
    }
    auto scene = std::unique_ptr<BenchmarkScene>(new BenchmarkScene(config));
    status = scene->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<BenchmarkScene>>::failure(status.error().code,
                                                                      status.error().message);
    }
    return core::Result<std::unique_ptr<BenchmarkScene>>::success(std::move(scene));
}

world::WorldState& BenchmarkScene::world() noexcept {
    return world_;
}

const world::WorldState& BenchmarkScene::world() const noexcept {
    return world_;
}

world::VoxelPalette& BenchmarkScene::palette() noexcept {
    return palette_;
}

const world::VoxelPalette& BenchmarkScene::palette() const noexcept {
    return palette_;
}

RenderCamera& BenchmarkScene::camera() noexcept {
    return camera_;
}

const RenderCamera& BenchmarkScene::camera() const noexcept {
    return camera_;
}

BenchmarkSceneKind BenchmarkScene::kind() const noexcept {
    return config_.kind;
}

std::uint64_t BenchmarkScene::seed() const noexcept {
    return config_.seed;
}

core::Status BenchmarkScene::initialize() {
    auto status = initialize_palette();
    if (!status) {
        return status;
    }
    status = populate_initial_chunks();
    if (!status) {
        return status;
    }
    auto origin = world::chunk_local_to_block(center_chunk_, {16, 0, 16});
    if (!origin) {
        return core::Status::failure(origin.error().code, origin.error().message);
    }
    constexpr auto edge = static_cast<std::int64_t>(world::VoxelChunk::edge_length);
    camera_.floating_origin.block = {
        origin.value().x,
        origin.value().y,
        origin.value().z + (config_.kind == BenchmarkSceneKind::terrain_material_preview
                                ? static_cast<std::int64_t>(config_.chunk_radius) + 1
                                : static_cast<std::int64_t>(config_.chunk_radius) + 3) *
                               edge,
    };
    camera_.local_position = config_.kind == BenchmarkSceneKind::terrain_material_preview
                                 ? math::Vec3f{0.0F, 22.0F, 0.0F}
                                 : math::Vec3f{0.0F, 24.0F, 0.0F};
    camera_.pitch_radians =
        config_.kind == BenchmarkSceneKind::terrain_material_preview ? -0.36F : -0.30F;
    return camera_.set_aspect_ratio(static_cast<float>(config_.initial_extent.width) /
                                    static_cast<float>(config_.initial_extent.height));
}

core::Status BenchmarkScene::initialize_palette() {
    auto foliage_model_id = prototype("benchmark:cross_plane");
    if (!foliage_model_id) {
        return core::Status::failure(foliage_model_id.error().code,
                                     foliage_model_id.error().message);
    }
    world::BlockModelDefinition foliage_model;
    foliage_model.prototype_id = foliage_model_id.value();
    foliage_model.kind = world::BlockModelKind::cross_plane;
    foliage_model.neighbor_dependency_radius = 0;
    foliage_model.mesh_invalidation_radius = 0;
    auto status = palette_.add_block_model(std::move(foliage_model));
    if (!status) {
        return status;
    }
    auto slope_model_id = prototype("benchmark:block_models/slope");
    if (!slope_model_id) {
        return core::Status::failure(slope_model_id.error().code, slope_model_id.error().message);
    }
    world::BlockModelDefinition slope_model;
    slope_model.prototype_id = slope_model_id.value();
    slope_model.kind = world::BlockModelKind::custom_voxel;
    slope_model.render_bounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    slope_model.triangles = {
        {.positions = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 1.0F}}}},
        {.positions = {{{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F}}}},
        {.positions = {{{0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}}},
        {.positions = {{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}}}},
        {.positions = {{{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}}}},
        {.positions = {{{1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 1.0F}}}},
        {.positions = {{{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 1.0F}}}},
        {.positions = {{{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 0.0F}}}},
    };
    status = palette_.add_block_model(std::move(slope_model));
    if (!status) {
        return status;
    }

    struct DefinitionSpec {
        std::uint16_t type;
        std::string_view id;
        std::string_view name;
        std::string_view material;
    };
    constexpr std::array definitions{
        DefinitionSpec{stone_type, "benchmark:stone", "Stone", "stone"},
        DefinitionSpec{surface_type, "benchmark:surface", "Surface", "surface"},
        DefinitionSpec{soil_type, "benchmark:soil", "Soil", "soil"},
        DefinitionSpec{foliage_type, "benchmark:foliage", "Foliage", "foliage"},
        DefinitionSpec{water_type, "benchmark:water", "Water", "water"},
        DefinitionSpec{slope_type, "benchmark:slope", "Authored slope", "stone"},
    };
    for (const auto& spec : definitions) {
        auto id = prototype(spec.id);
        if (!id) {
            return core::Status::failure(id.error().code, id.error().message);
        }
        world::VoxelDefinition definition;
        definition.type = spec.type;
        definition.prototype_id = std::move(id.value());
        definition.display_name = spec.name;
        definition.terrain_material = spec.material;
        definition.mining_tool = "pick";
        if (spec.type == foliage_type) {
            definition.block_model_id = foliage_model_id.value();
            definition.logical_occupancy = world::BlockLogicalOccupancy::decorative;
            definition.occlusion = world::BlockOcclusionBehavior::none;
            definition.collision_bounds.clear();
            definition.occlusion_bounds.clear();
        } else if (spec.type == water_type) {
            definition.logical_occupancy = world::BlockLogicalOccupancy::fluid;
            definition.occlusion = world::BlockOcclusionBehavior::none;
            definition.collision_bounds.clear();
            definition.occlusion_bounds.clear();
            definition.light_absorption = 8;
        } else if (spec.type == slope_type) {
            definition.block_model_id = slope_model_id.value();
            definition.logical_occupancy = world::BlockLogicalOccupancy::partial;
            definition.occlusion = world::BlockOcclusionBehavior::model;
        }
        status = palette_.add(std::move(definition));
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Status BenchmarkScene::populate_initial_chunks() {
    const auto radius = static_cast<std::int64_t>(config_.chunk_radius);
    for (std::int64_t z = -radius; z <= radius; ++z) {
        for (std::int64_t x = -radius; x <= radius; ++x) {
            const world::ChunkCoord coordinate{center_chunk_.x + x, center_chunk_.y,
                                               center_chunk_.z + z};
            auto status = insert_generated_chunk(coordinate);
            if (!status) {
                return status;
            }
            managed_chunks_.push_back(coordinate);
        }
    }
    return core::Status::ok();
}

core::Status BenchmarkScene::insert_generated_chunk(world::ChunkCoord coordinate) {
    world::VoxelChunk chunk(coordinate);
    auto status = chunk.load_generated_cells(generate_cells(coordinate));
    if (!status) {
        return status;
    }
    return world_.chunks().insert_generated(std::move(chunk), world_.dirty_regions());
}

std::vector<world::VoxelCell> BenchmarkScene::generate_cells(world::ChunkCoord coordinate) const {
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells, world::VoxelCell::air());
    constexpr auto edge_i64 = static_cast<std::int64_t>(world::VoxelChunk::edge_length);
    const auto shape = terrain_shape_for(config_.kind);
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < world::VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
                const auto global_x = coordinate.x * edge_i64 + static_cast<std::int64_t>(x);
                const auto global_y = coordinate.y * edge_i64 + static_cast<std::int64_t>(y);
                const auto global_z = coordinate.z * edge_i64 + static_cast<std::int64_t>(z);
                const auto noise = coordinate_hash(global_x, global_y, global_z, config_.seed);
                if (config_.kind == BenchmarkSceneKind::active_water &&
                    coordinate == center_chunk_) {
                    cells[cell_index(x, y, z)] = {water_type, 255,
                                                  world::full_fluid_source_state_bits(), 0};
                    continue;
                }
                std::uint16_t type = 0;
                std::uint16_t state_bits = 0;
                switch (shape) {
                case BenchmarkSceneKind::flat_terrain:
                    if (y <= 8) {
                        type = y == 8 ? surface_type : (y >= 6 ? soil_type : stone_type);
                    }
                    break;
                case BenchmarkSceneKind::mountainous_terrain: {
                    const auto folded_x = std::abs((global_x % 64) - 32);
                    const auto folded_z = std::abs((global_z % 48) - 24);
                    const auto roughness = static_cast<std::int64_t>(noise % 4);
                    const auto height = std::clamp<std::int64_t>(
                        31 - folded_x / 3 - folded_z / 4 + roughness, 4, 31);
                    if (static_cast<std::int64_t>(y) <= height) {
                        type = static_cast<std::int64_t>(y) == height ? surface_type : stone_type;
                    }
                    break;
                }
                case BenchmarkSceneKind::dense_caves:
                    if (y == 0 || (y < 27 && noise % 100 >= 35)) {
                        type = y > 23 ? surface_type : stone_type;
                    }
                    break;
                case BenchmarkSceneKind::checkerboard_geometry:
                    if (((static_cast<std::uint32_t>(x) + static_cast<std::uint32_t>(y) +
                          static_cast<std::uint32_t>(z)) &
                         1U) == 0U) {
                        type = stone_type;
                    }
                    break;
                case BenchmarkSceneKind::forest_cross_planes:
                    if (y <= 6) {
                        type = y == 6 ? surface_type : soil_type;
                    } else if (y == 7 &&
                               coordinate_hash(global_x, 0, global_z, config_.seed) % 29 == 0) {
                        type = foliage_type;
                    }
                    break;
                case BenchmarkSceneKind::terrain_material_preview: {
                    if (y <= 4) {
                        const auto band = static_cast<std::uint32_t>((global_x & 31) / 8);
                        constexpr std::array<std::uint16_t, 4> materials{stone_type, soil_type,
                                                                         surface_type, stone_type};
                        type = y == 4 ? materials[band] : stone_type;
                    }
                    const auto preview_chunk = world::ChunkCoord{
                        center_chunk_.x, center_chunk_.y,
                        center_chunk_.z + static_cast<std::int64_t>(config_.chunk_radius)};
                    if (coordinate == preview_chunk && y == 5 && x >= 5 && x <= 25 && z >= 5 &&
                        z <= 25) {
                        const auto column = static_cast<std::uint8_t>((x - 5U) / 7U);
                        const auto row = static_cast<std::uint8_t>((z - 5U) / 7U);
                        const auto pad_x = static_cast<std::uint8_t>((x - 5U) % 7U);
                        const auto pad_z = static_cast<std::uint8_t>((z - 5U) % 7U);
                        if (column < 3U && row < 3U && pad_x < 5U && pad_z < 5U) {
                            const auto state_index = static_cast<std::uint8_t>(row * 3U + column);
                            type = 1U + static_cast<std::uint16_t>(state_index % 3U);
                            state_bits = world::encode_voxel_surface_states(
                                {static_cast<std::uint16_t>(1U << state_index), 7U});
                        }
                    }
                    if (coordinate == preview_chunk && y == 5 && z == 27 && x >= 5 && x <= 25 &&
                        (x - 5U) % 3U == 0U) {
                        type = slope_type;
                    }
                    break;
                }
                case BenchmarkSceneKind::starting_biome: {
                    const auto river_center =
                        static_cast<double>(center_chunk_.x * edge_i64 + 16) +
                        std::sin(static_cast<double>(global_z) * 0.075) * 7.0;
                    const auto river =
                        std::abs(static_cast<double>(global_x) - river_center) < 3.25;
                    if (river) {
                        if (y <= 5) {
                            type = y == 5 ? soil_type : stone_type;
                        } else if (y <= 8) {
                            type = water_type;
                            state_bits = world::full_fluid_source_state_bits();
                        }
                    } else {
                        const auto height = std::clamp<std::int64_t>(
                            9 + static_cast<std::int64_t>(
                                    std::round(std::sin(static_cast<double>(global_x) * 0.055) *
                                                   2.5 +
                                               std::cos(static_cast<double>(global_z) * 0.047) *
                                                   1.8)),
                            6, 14);
                        if (static_cast<std::int64_t>(y) <= height) {
                            type = static_cast<std::int64_t>(y) == height
                                       ? surface_type
                                       : (static_cast<std::int64_t>(y) >= height - 2
                                              ? soil_type
                                              : stone_type);
                        }
                    }
                    break;
                }
                case BenchmarkSceneKind::rapid_voxel_edits:
                case BenchmarkSceneKind::mass_excavation:
                case BenchmarkSceneKind::high_speed_flythrough:
                case BenchmarkSceneKind::chunk_load_unload_churn:
                case BenchmarkSceneKind::large_coordinates:
                case BenchmarkSceneKind::resize_minimize_stress:
                case BenchmarkSceneKind::active_water:
                case BenchmarkSceneKind::particle_stress:
                case BenchmarkSceneKind::light_heavy_settlement:
                case BenchmarkSceneKind::character_workshop:
                    break;
                }
                if (type != 0) {
                    cells[cell_index(x, y, z)] = world::VoxelCell{type, 255, state_bits, 0};
                }
            }
        }
    }
    return cells;
}

core::Result<BenchmarkSceneStep> BenchmarkScene::advance(std::uint64_t frame_index) {
    BenchmarkSceneStep step;
    core::Status status = core::Status::ok();
    switch (config_.kind) {
    case BenchmarkSceneKind::rapid_voxel_edits:
        if ((frame_index & 1U) == 0U) {
            status = apply_voxel_edits(frame_index / 2U, 1);
        }
        break;
    case BenchmarkSceneKind::mass_excavation:
        status = apply_voxel_edits(frame_index, 32);
        break;
    case BenchmarkSceneKind::high_speed_flythrough:
    case BenchmarkSceneKind::large_coordinates:
        update_flythrough_camera(frame_index);
        break;
    case BenchmarkSceneKind::chunk_load_unload_churn:
        status = apply_chunk_churn(frame_index);
        break;
    case BenchmarkSceneKind::resize_minimize_stress: {
        const auto phase = frame_index % 120;
        if (phase == 0) {
            step.requested_extent = rhi::RenderExtent{};
            step.skip_render = true;
        } else if (phase == 1) {
            step.requested_extent = rhi::RenderExtent{800, 600};
        } else if (phase == 40) {
            step.requested_extent = rhi::RenderExtent{1280, 720};
        } else if (phase == 80) {
            step.requested_extent = rhi::RenderExtent{1600, 900};
        }
        break;
    }
    case BenchmarkSceneKind::flat_terrain:
    case BenchmarkSceneKind::mountainous_terrain:
    case BenchmarkSceneKind::dense_caves:
    case BenchmarkSceneKind::checkerboard_geometry:
    case BenchmarkSceneKind::forest_cross_planes:
    case BenchmarkSceneKind::active_water:
    case BenchmarkSceneKind::particle_stress:
    case BenchmarkSceneKind::light_heavy_settlement:
    case BenchmarkSceneKind::terrain_material_preview:
    case BenchmarkSceneKind::starting_biome:
    case BenchmarkSceneKind::character_workshop:
        break;
    }
    if (!status) {
        return core::Result<BenchmarkSceneStep>::failure(status.error().code,
                                                         status.error().message);
    }
    status = camera_.update_matrices();
    if (!status) {
        return core::Result<BenchmarkSceneStep>::failure(status.error().code,
                                                         status.error().message);
    }
    return core::Result<BenchmarkSceneStep>::success(step);
}

void BenchmarkScene::activate_fluid_work(world::ChunkFluidSystem& fluids) const {
    if (config_.kind != BenchmarkSceneKind::active_water) {
        return;
    }
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < world::VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
                fluids.activate(world::ChunkLocalCoord{center_chunk_, {x, y, z}});
            }
        }
    }
}

core::Status BenchmarkScene::apply_voxel_edits(std::uint64_t frame_index,
                                               std::uint16_t edit_count) {
    for (std::uint16_t edit = 0; edit < edit_count; ++edit) {
        const auto coordinate = managed_chunks_[static_cast<std::size_t>(
            (frame_index + static_cast<std::uint64_t>(edit)) % managed_chunks_.size())];
        const auto boundary = (frame_index + static_cast<std::uint64_t>(edit)) % 8 == 0;
        const auto x = boundary ? static_cast<std::uint16_t>((frame_index / 8) % 2 == 0 ? 0 : 31)
                                : static_cast<std::uint16_t>((frame_index + edit * 7U) % 32);
        const auto y = static_cast<std::uint16_t>(10U + (edit % 8U));
        const auto z = static_cast<std::uint16_t>((frame_index * 3U + edit * 11U) % 32U);
        const world::VoxelCoord voxel{x, y, z};
        const auto current = world_.chunks().get(coordinate, voxel);
        if (!current) {
            return core::Status::failure(current.error().code, current.error().message);
        }
        const auto cell = current.value().is_air() ? world::VoxelCell{stone_type, 255, 0, 0}
                                                   : world::VoxelCell::air();
        auto status =
            world_.chunks().set(coordinate, voxel, cell, world_.dirty_regions(), palette_);
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Status BenchmarkScene::apply_chunk_churn(std::uint64_t frame_index) {
    if (frame_index % 8 != 0 || managed_chunks_.empty()) {
        return core::Status::ok();
    }
    const auto coordinate = managed_chunks_[churn_cursor_ % managed_chunks_.size()];
    ++churn_cursor_;
    if (!world_.chunks().erase(coordinate)) {
        return core::Status::failure("renderer.benchmark_churn_missing_chunk",
                                     "chunk churn expected a loaded chunk");
    }
    return insert_generated_chunk(coordinate);
}

void BenchmarkScene::update_flythrough_camera(std::uint64_t frame_index) noexcept {
    constexpr auto edge = static_cast<std::int64_t>(world::VoxelChunk::edge_length);
    const auto center_x = center_chunk_.x * edge + 16;
    const auto center_z = center_chunk_.z * edge + 16;
    const auto amplitude =
        std::max<std::int64_t>(edge / 2, static_cast<std::int64_t>(config_.chunk_radius) * edge);
    const auto path_length = amplitude * 4;
    const auto phase =
        static_cast<std::int64_t>((frame_index * 4) % static_cast<std::uint64_t>(path_length));
    const auto travel = phase <= amplitude * 2 ? -amplitude + phase : amplitude * 3 - phase;
    camera_.floating_origin.block = {center_x + travel, 0, center_z + edge * 3};
    camera_.local_position = {0.25F, 24.0F, 0.5F};
    camera_.yaw_radians = -0.15F;
    camera_.pitch_radians = -0.24F;
}

} // namespace heartstead::renderer::benchmark
