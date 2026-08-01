#include "engine/renderer/chunks/chunk_render_system.hpp"

#include "engine/core/logging.hpp"
#include "engine/renderer/terrain/terrain_mapping.hpp"
#include "engine/world/blocks/block_model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] bool region_contains(const dirty::DirtyRegion& region,
                                   world::ChunkCoord coordinate) noexcept {
    return coordinate.x >= region.bounds.min.x && coordinate.x <= region.bounds.max.x &&
           coordinate.y >= region.bounds.min.y && coordinate.y <= region.bounds.max.y &&
           coordinate.z >= region.bounds.min.z && coordinate.z <= region.bounds.max.z;
}

[[nodiscard]] math::Bounds3f translated_bounds(math::Bounds3f bounds, math::Vec3f origin) noexcept {
    if (!bounds.is_valid()) {
        return bounds;
    }
    return {bounds.min + origin, bounds.max + origin};
}

[[nodiscard]] math::Bounds3f default_chunk_bounds() noexcept {
    constexpr auto edge = static_cast<float>(world::VoxelChunk::edge_length);
    return {{0.0F, 0.0F, 0.0F}, {edge, edge, edge}};
}

[[nodiscard]] std::uint64_t axis_distance(std::int64_t left, std::int64_t right) noexcept {
    if (left >= right) {
        return static_cast<std::uint64_t>(left) - static_cast<std::uint64_t>(right);
    }
    return static_cast<std::uint64_t>(right) - static_cast<std::uint64_t>(left);
}

[[nodiscard]] core::Result<world::ChunkCoord>
camera_chunk_coordinate(const RenderCamera& camera) noexcept {
    if (!camera.local_position.is_finite()) {
        return core::Result<world::ChunkCoord>::failure("renderer.invalid_camera_position",
                                                        "camera local position must be finite");
    }
    const auto floor_axis = [](float value) -> core::Result<std::int64_t> {
        const auto floored = std::floor(static_cast<long double>(value));
        constexpr auto minimum = static_cast<long double>(std::numeric_limits<std::int64_t>::min());
        constexpr auto maximum = static_cast<long double>(std::numeric_limits<std::int64_t>::max());
        if (floored < minimum || floored > maximum) {
            return core::Result<std::int64_t>::failure(
                "renderer.camera_local_position_overflow",
                "camera local position does not fit an exact block offset");
        }
        return core::Result<std::int64_t>::success(static_cast<std::int64_t>(floored));
    };
    auto x = floor_axis(camera.local_position.x);
    auto y = floor_axis(camera.local_position.y);
    auto z = floor_axis(camera.local_position.z);
    if (!x || !y || !z) {
        const auto& error = !x ? x.error() : (!y ? y.error() : z.error());
        return core::Result<world::ChunkCoord>::failure(error.code, error.message);
    }
    auto block = world::checked_block_coord_offset(camera.floating_origin.block,
                                                   {x.value(), y.value(), z.value()});
    if (!block) {
        return core::Result<world::ChunkCoord>::failure(block.error().code, block.error().message);
    }
    return core::Result<world::ChunkCoord>::success(world::chunk_coord_for_block(block.value()));
}

[[nodiscard]] bool within_cylinder(world::ChunkCoord coord, world::ChunkCoord center,
                                   std::uint16_t horizontal_radius,
                                   std::uint16_t vertical_radius) noexcept {
    const auto dx = axis_distance(coord.x, center.x);
    const auto dy = axis_distance(coord.y, center.y);
    const auto dz = axis_distance(coord.z, center.z);
    const auto horizontal = static_cast<std::uint64_t>(horizontal_radius);
    return dx <= horizontal && dz <= horizontal &&
           dy <= static_cast<std::uint64_t>(vertical_radius) &&
           dx * dx + dz * dz <= horizontal * horizontal;
}

} // namespace

core::Status ChunkRenderConfig::validate() const {
    auto distance_status = distances.validate();
    if (!distance_status) {
        return distance_status;
    }
    if (max_chunks_meshed_per_frame == 0 || max_chunks_uploaded_per_frame == 0) {
        return core::Status::failure("renderer.invalid_chunk_mesh_budget",
                                     "chunk mesh and upload budgets must allow at least one chunk");
    }
    if (max_bytes_uploaded_per_frame == 0) {
        return core::Status::failure("renderer.invalid_chunk_upload_budget",
                                     "chunk upload budget must allow at least one byte");
    }
    if (gpu_terrain_budget_bytes == 0) {
        return core::Status::failure("renderer.invalid_gpu_terrain_budget",
                                     "GPU terrain residency budget must allow at least one byte");
    }
    constexpr auto maximum_snapshot_side =
        static_cast<std::size_t>(world::VoxelChunk::edge_length) +
        static_cast<std::size_t>(world::BlockModelDefinition::max_dependency_radius) * 2;
    constexpr auto maximum_snapshot_cells =
        maximum_snapshot_side * maximum_snapshot_side * maximum_snapshot_side;
    if (max_snapshot_cells_per_frame < maximum_snapshot_cells) {
        return core::Status::failure(
            "renderer.invalid_chunk_snapshot_budget",
            "chunk snapshot budget must fit one maximum-halo neighborhood");
    }
    ChunkMeshSchedulerConfig scheduler_config;
    scheduler_config.worker_count = mesh_worker_count;
    scheduler_config.max_concurrent_jobs = max_concurrent_mesh_jobs;
    scheduler_config.max_completed_results = max_completed_mesh_results_per_frame * 4;
    scheduler_config.max_cached_snapshot_buffers = max_cached_snapshot_buffers;
    scheduler_config.max_cached_mesh_buffers = max_cached_mesh_buffers;
    scheduler_config.meshing_mode = meshing_mode;
    auto scheduler_status = scheduler_config.validate();
    if (!scheduler_status) {
        return scheduler_status;
    }
    if (max_completed_mesh_results_per_frame == 0) {
        return core::Status::failure("renderer.invalid_completed_mesh_budget",
                                     "completed mesh drain budget must be nonzero");
    }
    return core::Status::ok();
}

std::size_t ChunkRenderSystem::PendingUpload::byte_size() const noexcept {
    return vertices.size() * sizeof(terrain::GpuChunkVertex) +
           mesh.indices.size() * sizeof(std::uint32_t);
}

bool TerrainPipelineSet::is_valid() const noexcept {
    return opaque.is_valid() && alpha_tested.is_valid() && transparent.is_valid() &&
           fluid.is_valid();
}

rhi::RenderResourceHandle
TerrainPipelineSet::for_phase(world::MeshingRenderPhase phase) const noexcept {
    switch (phase) {
    case world::MeshingRenderPhase::opaque:
        return opaque;
    case world::MeshingRenderPhase::alpha_tested:
        return alpha_tested;
    case world::MeshingRenderPhase::transparent:
        return transparent;
    case world::MeshingRenderPhase::fluid:
        return fluid;
    }
    return {};
}

ChunkRenderSystem::ChunkRenderSystem(ChunkGpuCache& cache,
                                     rhi::RenderResourceHandle terrain_pipeline,
                                     const world::VoxelPalette* palette, ChunkRenderConfig config)
    : ChunkRenderSystem(cache,
                        TerrainPipelineSet{terrain_pipeline, terrain_pipeline, terrain_pipeline,
                                           terrain_pipeline},
                        palette, config) {}

ChunkRenderSystem::ChunkRenderSystem(ChunkGpuCache& cache, TerrainPipelineSet terrain_pipelines,
                                     const world::VoxelPalette* palette, ChunkRenderConfig config)
    : cache_(&cache), terrain_pipelines_(terrain_pipelines), palette_(palette), config_(config) {}

ChunkRenderSystem::~ChunkRenderSystem() {
    shutdown();
}

core::Status ChunkRenderSystem::initialize() {
    auto status = config_.validate();
    if (!status) {
        return status;
    }
    auto table = world::build_block_render_table_snapshot(palette_);
    if (!table) {
        return core::Status::failure(table.error().code, table.error().message);
    }
    render_table_ =
        std::make_shared<const world::BlockRenderTableSnapshot>(std::move(table).value());
    ChunkMeshSchedulerConfig scheduler_config;
    scheduler_config.worker_count = config_.mesh_worker_count;
    scheduler_config.max_concurrent_jobs = config_.max_concurrent_mesh_jobs;
    scheduler_config.max_completed_results = config_.max_completed_mesh_results_per_frame * 4;
    scheduler_config.max_cached_snapshot_buffers = config_.max_cached_snapshot_buffers;
    scheduler_config.max_cached_mesh_buffers = config_.max_cached_mesh_buffers;
    scheduler_config.meshing_mode = config_.meshing_mode;
    auto scheduler = ChunkMeshScheduler::create(scheduler_config);
    if (!scheduler) {
        render_table_.reset();
        return core::Status::failure(scheduler.error().code, scheduler.error().message);
    }
    mesh_scheduler_ = std::move(scheduler).value();
    return core::Status::ok();
}

void ChunkRenderSystem::shutdown() noexcept {
    if (mesh_scheduler_ != nullptr) {
        for (auto& pending : pending_uploads_) {
            recycle_upload_storage(pending);
        }
        pending_uploads_.clear();
        mesh_scheduler_->shutdown();
        mesh_scheduler_.reset();
    }
    pending_meshes_.clear();
    pending_uploads_.clear();
    gpu_vertex_pool_.clear();
    render_table_.reset();
}

core::Status
ChunkRenderSystem::process_chunk_loads(std::span<const world::ChunkStreamLoadReport> loads) {
    for (const auto& load : loads) {
        if (!load.identity.is_valid()) {
            return core::Status::failure("renderer.invalid_chunk_load_identity",
                                         "chunk load report has no load generation");
        }
        if (!cache_->contains(load.identity)) {
            const auto* old_entry = cache_->find_by_coordinate(load.identity.coordinate);
            if (old_entry != nullptr) {
                const auto old_identity = old_entry->identity;
                if (mesh_scheduler_ != nullptr) {
                    mesh_scheduler_->cancel(old_identity);
                }
                remove_pending(old_identity);
                auto erase_status = cache_->erase(old_identity);
                if (!erase_status) {
                    return erase_status;
                }
            }
            auto insert_status = cache_->insert(load.identity);
            if (!insert_status) {
                return insert_status;
            }
        }
        enqueue_mesh(load.identity, false);
    }
    refresh_queue_stats();
    return core::Status::ok();
}

core::Status
ChunkRenderSystem::process_chunk_evictions(std::span<const world::ChunkIdentity> evictions) {
    core::Status first_failure = core::Status::ok();
    for (const auto identity : evictions) {
        if (mesh_scheduler_ != nullptr) {
            mesh_scheduler_->cancel(identity);
        }
        remove_pending(identity);
        if (!cache_->contains(identity)) {
            // A stale eviction must never remove a newer generation at the same coordinate.
            continue;
        }
        auto status = cache_->erase(identity);
        if (!status && first_failure) {
            first_failure = status;
        }
    }
    refresh_queue_stats();
    return first_failure;
}

core::Status
ChunkRenderSystem::process_chunk_evictions(const world::ChunkStreamEvictionReport& eviction) {
    return process_chunk_evictions(eviction.evicted_identities);
}

core::Status ChunkRenderSystem::synchronize(world::WorldState& world, const RenderCamera& camera) {
    auto status = config_.validate();
    if (!status) {
        return status;
    }
    if (mesh_scheduler_ == nullptr || render_table_ == nullptr) {
        status = initialize();
        if (!status) {
            return status;
        }
    }
    if (!terrain_pipelines_.is_valid()) {
        return core::Status::failure("renderer.invalid_terrain_pipeline",
                                     "chunk render system requires a terrain pipeline");
    }
    stats_.meshed_chunk_count = 0;
    stats_.uploaded_chunk_count = 0;
    stats_.uploaded_bytes = 0;
    stats_.failed_mesh_count = 0;
    stats_.failed_upload_count = 0;
    stats_.scheduled_mesh_count = 0;
    stats_.cancelled_mesh_count = 0;
    stats_.stale_mesh_result_count = 0;
    stats_.snapshot_cells_copied = 0;
    stats_.meshing_ms = 0.0;
    stats_.gpu_wait_ms = 0.0;
    timings_.reset();

    status = refresh_render_table(world, camera);
    if (!status) {
        return status;
    }
    status = reconcile_loaded_chunks(world, camera);
    if (!status) {
        return status;
    }
    consume_dirty_regions(world, camera);

    status = enforce_distance_residency(camera);
    if (!status) {
        return status;
    }

    core::Status first_failure = process_completed_meshes(world, camera);
    auto upload_status = process_upload_queue(world, camera);
    if (!upload_status && first_failure) {
        first_failure = upload_status;
    }
    auto residency_status = enforce_gpu_residency_budget(camera);
    if (!residency_status && first_failure) {
        first_failure = residency_status;
    }
    auto scheduling_status = schedule_mesh_jobs(world, camera);
    if (!scheduling_status && first_failure) {
        first_failure = scheduling_status;
    }
    stats_.cache = cache_->stats();
    refresh_queue_stats();
    refresh_timing_stats();
    return first_failure;
}

core::Status
ChunkRenderSystem::set_terrain_pipeline(rhi::RenderResourceHandle terrain_pipeline) noexcept {
    if (!terrain_pipeline.is_valid()) {
        return core::Status::failure("chunk_render_system.invalid_terrain_pipeline",
                                     "terrain pipeline handle must be valid");
    }
    terrain_pipelines_ = {terrain_pipeline, terrain_pipeline, terrain_pipeline, terrain_pipeline};
    return core::Status::ok();
}

core::Status ChunkRenderSystem::set_terrain_pipelines(TerrainPipelineSet pipelines) noexcept {
    if (!pipelines.is_valid()) {
        return core::Status::failure("chunk_render_system.invalid_terrain_pipelines",
                                     "all terrain phase pipeline handles must be valid");
    }
    terrain_pipelines_ = pipelines;
    return core::Status::ok();
}

ChunkDrawList ChunkRenderSystem::build_draw_list(const RenderCamera& camera) {
    return build_draw_list(camera, {});
}

ChunkDrawList
ChunkRenderSystem::build_draw_list(const RenderCamera& camera,
                                   std::vector<rhi::RenderDrawCommand> reusable_draw_storage,
                                   std::span<const math::Mat4f> shadow_view_projections) {
    ChunkDrawList result;
    result.draws = std::move(reusable_draw_storage);
    result.draws.clear();
    visible_chunks_scratch_.clear();
    visible_chunks_scratch_.reserve(cache_->stats().entry_count);
    result.draws.reserve(cache_->stats().resident_section_count);
    const auto shadow_view_count = std::min<std::size_t>(shadow_view_projections.size(), 64U);
    std::vector<RenderFrustum> shadow_frusta;
    shadow_frusta.reserve(shadow_view_count);
    for (std::size_t shadow_view = 0; shadow_view < shadow_view_count; ++shadow_view) {
        shadow_frusta.push_back(
            RenderFrustum::from_view_projection(shadow_view_projections[shadow_view]));
    }
    result.shadow_draws.resize(shadow_view_count);
    ++visibility_epoch_;
    if (visibility_epoch_ == 0) {
        ++visibility_epoch_;
    }
    {
        profiling::ScopedCpuTimingZone culling_zone(timings_,
                                                    profiling::CpuTimingZone::visibility_culling);
        std::unordered_set<VisibilityKey> current_keys;
        current_keys.reserve(cache_->stats().entry_count);
        for (const auto* entry : cache_->entries()) {
            if (entry->state != ChunkGpuState::resident) {
                continue;
            }
            if (!within_visible_distance(entry->identity.coordinate, camera)) {
                ++result.culled_chunk_count;
                ++result.distance_culled_chunk_count;
                continue;
            }
            const auto origin = camera_relative_chunk_origin(entry->identity.coordinate, camera);
            if (!origin) {
                ++result.culled_chunk_count;
                ++result.frustum_culled_chunk_count;
                continue;
            }
            const auto bounds = translated_bounds(entry->local_bounds, origin.value());
            const auto key = static_cast<VisibilityKey>(entry->identity.load_generation);
            current_keys.insert(key);
            visibility_hierarchy_.upsert(
                {key,
                 {{static_cast<double>(bounds.min.x), static_cast<double>(bounds.min.y),
                   static_cast<double>(bounds.min.z)},
                  {static_cast<double>(bounds.max.x), static_cast<double>(bounds.max.y),
                   static_cast<double>(bounds.max.z)}},
                 {}, 0.0, 1.0F, true, false});
            visible_chunks_scratch_.push_back({entry, origin.value(), false, 0});
        }
        for (const auto key : visibility_keys_) {
            if (!current_keys.contains(key)) {
                static_cast<void>(visibility_hierarchy_.erase(key));
            }
        }
        visibility_keys_ = std::move(current_keys);

        RenderCamera relative_camera = camera;
        relative_camera.local_position = {};
        static_cast<void>(relative_camera.update_matrices());
        std::vector<VisibilityView> views;
        views.reserve(shadow_frusta.size() + 1U);
        views.push_back({1U,
                         VisibilityViewKind::main,
                         {static_cast<double>(camera.local_position.x),
                          static_cast<double>(camera.local_position.y),
                          static_cast<double>(camera.local_position.z)},
                         RenderFrustum::from_view_projection(relative_camera.view_projection),
                         1U, camera.vertical_fov_radians, 0.0});
        for (std::size_t shadow_view = 0; shadow_view < shadow_frusta.size(); ++shadow_view) {
            views.push_back({static_cast<VisibilityViewId>(shadow_view + 2U),
                             VisibilityViewKind::directional_shadow, {},
                             shadow_frusta[shadow_view], 1U,
                             camera.vertical_fov_radians, 0.0});
        }
        const auto visibility = visibility_hierarchy_.query(views);
        std::unordered_map<VisibilityKey, std::uint64_t> masks;
        masks.reserve(visibility.selections.size());
        for (const auto& selected : visibility.selections) {
            auto& mask = masks[selected.key];
            if (selected.view_id == 1U) {
                mask |= std::uint64_t{1} << 63U;
            } else {
                mask |= std::uint64_t{1} << (selected.view_id - 2U);
            }
        }
        std::size_t retained = 0;
        for (auto visible : visible_chunks_scratch_) {
            const auto key =
                static_cast<VisibilityKey>(visible.entry->identity.load_generation);
            const auto found = masks.find(key);
            const auto mask = found == masks.end() ? std::uint64_t{0} : found->second;
            visible.camera_visible = (mask & (std::uint64_t{1} << 63U)) != 0;
            visible.shadow_visibility_mask = mask & ~(std::uint64_t{1} << 63U);
            if (!visible.camera_visible) {
                ++result.culled_chunk_count;
                ++result.frustum_culled_chunk_count;
            }
            if (!visible.camera_visible && visible.shadow_visibility_mask == 0) {
                continue;
            }
            result.visible_chunk_count += visible.camera_visible ? 1U : 0U;
            cache_->mark_visible(visible.entry->identity, visibility_epoch_);
            visible_chunks_scratch_[retained++] = visible;
        }
        visible_chunks_scratch_.resize(retained);
        result.visibility_hierarchy_nodes = visibility_hierarchy_.node_count();
        result.visibility_nodes_tested = visibility.stats.hierarchy_nodes_tested;
        result.visibility_nodes_culled = visibility.stats.hierarchy_nodes_culled;
    }

    {
        profiling::ScopedCpuTimingZone draw_zone(timings_,
                                                 profiling::CpuTimingZone::draw_list_construction);
        for (const auto& visible : visible_chunks_scratch_) {
            if (!visible.entry->has_drawable_mesh()) {
                continue;
            }
            result.drawn_chunk_count += visible.camera_visible ? 1U : 0U;
            const auto allocation_first_index = static_cast<std::uint32_t>(
                visible.entry->mesh.indices.offset /
                rhi::render_index_type_size(visible.entry->mesh.index_type));
            for (const auto& section : visible.entry->mesh.sections) {
                rhi::RenderDrawCommand draw;
                draw.pipeline = terrain_pipelines_.for_phase(section.render_phase);
                draw.vertex_buffer = visible.entry->mesh.vertices.buffer;
                draw.index_buffer = visible.entry->mesh.indices.buffer;
                draw.index_count = section.index_count;
                draw.first_index = allocation_first_index + section.first_index;
                draw.vertex_offset = static_cast<std::int32_t>(visible.entry->mesh.vertices.offset /
                                                               sizeof(terrain::GpuChunkVertex));
                draw.instance_count = 1;
                draw.camera_relative_origin = visible.origin;
                draw.texture_variation_seed =
                    terrain::terrain_coordinate_key(visible.entry->identity.coordinate);
                draw.index_type = visible.entry->mesh.index_type;
                const auto center =
                    visible.origin +
                    (visible.entry->local_bounds.min + visible.entry->local_bounds.max) * 0.5F;
                draw.sort_depth = math::length_squared(center - camera.local_position);
                if (visible.camera_visible) {
                    result.draws.push_back(draw);
                }
                if (section.render_phase == world::MeshingRenderPhase::opaque ||
                    section.render_phase == world::MeshingRenderPhase::alpha_tested) {
                    for (std::size_t shadow_view = 0; shadow_view < result.shadow_draws.size();
                         ++shadow_view) {
                        if ((visible.shadow_visibility_mask & (std::uint64_t{1} << shadow_view)) !=
                            0) {
                            result.shadow_draws[shadow_view].push_back(draw);
                        }
                    }
                }
            }
            if (visible.camera_visible) {
                result.vertex_count += visible.entry->mesh.vertex_count;
                result.index_count += visible.entry->mesh.index_count;
            }
        }
        const auto phase_rank = [this](rhi::RenderResourceHandle pipeline) {
            if (pipeline == terrain_pipelines_.opaque) {
                return 0;
            }
            if (pipeline == terrain_pipelines_.alpha_tested) {
                return 1;
            }
            return 2;
        };
        std::ranges::stable_sort(result.draws, [&](const rhi::RenderDrawCommand& left,
                                                   const rhi::RenderDrawCommand& right) {
            const auto left_phase = phase_rank(left.pipeline);
            const auto right_phase = phase_rank(right.pipeline);
            if (left_phase != right_phase) {
                return left_phase < right_phase;
            }
            if (left_phase == 2 && left.sort_depth != right.sort_depth) {
                return left.sort_depth > right.sort_depth;
            }
            return left.pipeline.value < right.pipeline.value;
        });
    }
    stats_.visible_chunk_count = result.visible_chunk_count;
    stats_.culled_chunk_count = result.culled_chunk_count;
    stats_.distance_culled_chunk_count = result.distance_culled_chunk_count;
    stats_.frustum_culled_chunk_count = result.frustum_culled_chunk_count;
    stats_.drawn_chunk_count = result.drawn_chunk_count;
    stats_.draw_count = result.draws.size();
    stats_.visible_vertex_count = result.vertex_count;
    stats_.visible_index_count = result.index_count;
    stats_.visibility_hierarchy_nodes = result.visibility_hierarchy_nodes;
    stats_.visibility_nodes_tested = result.visibility_nodes_tested;
    stats_.visibility_nodes_culled = result.visibility_nodes_culled;
    refresh_timing_stats();
    return result;
}

const ChunkRenderStats& ChunkRenderSystem::stats() const noexcept {
    return stats_;
}

void ChunkRenderSystem::enqueue_mesh(world::ChunkIdentity identity, bool forced) {
    const auto found =
        std::ranges::find_if(pending_meshes_, [identity](const PendingMesh& pending) {
            return pending.identity == identity;
        });
    if (found != pending_meshes_.end()) {
        found->forced = found->forced || forced;
        return;
    }
    pending_meshes_.push_back({identity, forced, next_sequence_++});
}

void ChunkRenderSystem::remove_pending(world::ChunkIdentity identity) {
    std::erase_if(pending_meshes_,
                  [identity](const PendingMesh& pending) { return pending.identity == identity; });
    if (mesh_scheduler_ != nullptr) {
        for (auto& pending : pending_uploads_) {
            if (pending.identity == identity) {
                recycle_upload_storage(pending);
            }
        }
    }
    std::erase_if(pending_uploads_, [identity](const PendingUpload& pending) {
        return pending.identity == identity;
    });
}

core::Status ChunkRenderSystem::reconcile_loaded_chunks(world::WorldState& world,
                                                        const RenderCamera& camera) {
    const auto loaded = world.chunks().identities();
    for (const auto identity : loaded) {
        const auto* existing = cache_->find_by_coordinate(identity.coordinate);
        if (existing != nullptr && existing->identity != identity) {
            const auto old_identity = existing->identity;
            mesh_scheduler_->cancel(old_identity);
            remove_pending(old_identity);
            auto erase_status = cache_->erase(old_identity);
            if (!erase_status) {
                return erase_status;
            }
        }
        if (!cache_->contains(identity)) {
            auto insert_status = cache_->insert(identity);
            if (!insert_status) {
                return insert_status;
            }
            if (within_mesh_distance(identity.coordinate, camera)) {
                enqueue_mesh(identity, false);
            }
        }

        if (!within_mesh_distance(identity.coordinate, camera)) {
            mesh_scheduler_->cancel(identity);
            remove_pending(identity);
            continue;
        }

        const auto* chunk = world.chunks().find(identity.coordinate);
        auto* entry = cache_->find(identity);
        if (chunk == nullptr || entry == nullptr || chunk->identity() != identity) {
            continue;
        }
        if (entry->residency_suppressed) {
            if (!should_restore_suppressed_residency(*entry, camera)) {
                continue;
            }
            entry->residency_suppressed = false;
        }
        const auto queued_upload =
            std::ranges::find_if(pending_uploads_, [identity](const PendingUpload& pending) {
                return pending.identity == identity;
            });
        const bool newest_revision_awaits_upload =
            queued_upload != pending_uploads_.end() &&
            queued_upload->content_revision == chunk->content_revision() &&
            queued_upload->render_table_revision == render_table_->revision;
        const auto in_flight_revision = mesh_scheduler_->in_flight_revision(identity);
        const bool newest_revision_in_flight =
            in_flight_revision.has_value() && *in_flight_revision == chunk->content_revision();
        if (in_flight_revision.has_value() && *in_flight_revision != chunk->content_revision()) {
            mesh_scheduler_->cancel(identity);
        }
        const bool stale_revision =
            entry->state != ChunkGpuState::resident ||
            entry->resident_content_revision != chunk->content_revision() ||
            entry->resident_render_table_revision != render_table_->revision;
        if (!newest_revision_awaits_upload && !newest_revision_in_flight &&
            (stale_revision || chunk->dirty().contains(world::ChunkDirtyFlag::mesh))) {
            enqueue_mesh(identity, chunk->dirty().contains(world::ChunkDirtyFlag::mesh));
        }
    }

    std::vector<world::ChunkIdentity> unloaded;
    for (const auto* entry : cache_->entries()) {
        if (!std::ranges::binary_search(loaded, entry->identity)) {
            unloaded.push_back(entry->identity);
        }
    }
    return process_chunk_evictions(unloaded);
}

void ChunkRenderSystem::consume_dirty_regions(world::WorldState& world,
                                              const RenderCamera& camera) {
    const auto regions = world.dirty_regions().consume_kind(dirty::DirtyRegionKind::chunk_mesh);
    if (regions.empty()) {
        return;
    }
    for (const auto identity : world.chunks().identities()) {
        const auto* entry = cache_->find(identity);
        const bool residency_allows_work = entry == nullptr || !entry->residency_suppressed ||
                                           should_restore_suppressed_residency(*entry, camera);
        if (within_mesh_distance(identity.coordinate, camera) && residency_allows_work &&
            std::ranges::any_of(regions, [identity](const dirty::DirtyRegion& region) {
                return region_contains(region, identity.coordinate);
            })) {
            enqueue_mesh(identity, true);
        }
    }
}

core::Status ChunkRenderSystem::refresh_render_table(world::WorldState& world,
                                                     const RenderCamera& camera) {
    const auto current_revision =
        palette_ == nullptr ? std::uint64_t{1} : palette_->render_revision();
    if (render_table_ != nullptr && render_table_->revision == current_revision) {
        return core::Status::ok();
    }
    auto rebuilt = world::build_block_render_table_snapshot(palette_);
    if (!rebuilt) {
        return core::Status::failure(rebuilt.error().code, rebuilt.error().message);
    }
    mesh_scheduler_->cancel_all();
    for (auto& pending : pending_uploads_) {
        recycle_upload_storage(pending);
    }
    pending_uploads_.clear();
    render_table_ =
        std::make_shared<const world::BlockRenderTableSnapshot>(std::move(rebuilt).value());
    for (const auto identity : world.chunks().identities()) {
        if (within_mesh_distance(identity.coordinate, camera)) {
            enqueue_mesh(identity, true);
        }
    }
    return core::Status::ok();
}

bool ChunkRenderSystem::mesh_result_is_current(const ChunkMeshResult& result,
                                               const world::WorldState& world) const {
    if (render_table_ == nullptr || result.block_render_table_revision != render_table_->revision ||
        !cache_->contains(result.identity)) {
        return false;
    }
    const auto* chunk = world.chunks().find(result.identity.coordinate);
    if (chunk == nullptr || chunk->identity() != result.identity ||
        chunk->content_revision() != result.center_revision ||
        !world::dependency_revisions_match(world.chunks(), result.dependency_revisions)) {
        return false;
    }
    const auto* entry = cache_->find(result.identity);
    if (entry == nullptr) {
        return false;
    }
    const bool exact_mesh_is_resident =
        entry->state == ChunkGpuState::resident &&
        entry->resident_content_revision == result.center_revision &&
        entry->resident_render_table_revision == result.block_render_table_revision &&
        std::ranges::equal(entry->resident_dependency_revisions, result.dependency_revisions);
    return !exact_mesh_is_resident;
}

bool ChunkRenderSystem::pending_upload_is_current(const PendingUpload& upload,
                                                  const world::WorldState& world) const {
    if (render_table_ == nullptr || upload.render_table_revision != render_table_->revision ||
        !cache_->contains(upload.identity)) {
        return false;
    }
    const auto* chunk = world.chunks().find(upload.identity.coordinate);
    return chunk != nullptr && chunk->identity() == upload.identity &&
           chunk->content_revision() == upload.content_revision &&
           world::dependency_revisions_match(world.chunks(), upload.dependency_revisions);
}

core::Status ChunkRenderSystem::process_completed_meshes(world::WorldState& world,
                                                         const RenderCamera& camera) {
    const auto requeue_latest = [this, &world, &camera](world::ChunkCoord coordinate) {
        const auto* chunk = world.chunks().find(coordinate);
        if (chunk != nullptr && cache_->contains(chunk->identity()) &&
            within_mesh_distance(coordinate, camera)) {
            enqueue_mesh(chunk->identity(), true);
        }
    };
    auto results = mesh_scheduler_->drain_completed(config_.max_completed_mesh_results_per_frame);
    for (auto& result : results) {
        stats_.meshing_ms += result.meshing_ms;
        if (result.state == ChunkMeshResultState::cancelled) {
            ++stats_.cancelled_mesh_count;
            requeue_latest(result.identity.coordinate);
            continue;
        }
        ++stats_.meshed_chunk_count;
        if (result.state == ChunkMeshResultState::failed || !result.mesh.has_value()) {
            ++stats_.failed_mesh_count;
            ++stats_.total_failed_mesh_count;
            cache_->mark_failed(result.identity);
            requeue_latest(result.identity.coordinate);
            core::log(core::LogLevel::warning, "Asynchronous chunk mesh failed: " +
                                                   result.error_code + ": " + result.error_message);
            continue;
        }
        const auto* result_entry = cache_->find(result.identity);
        if ((result_entry != nullptr && result_entry->residency_suppressed &&
             !should_restore_suppressed_residency(*result_entry, camera)) ||
            !within_mesh_distance(result.identity.coordinate, camera) ||
            !mesh_result_is_current(result, world)) {
            ++stats_.stale_mesh_result_count;
            ++stats_.total_stale_mesh_result_count;
            if (result.mesh.has_value()) {
                mesh_scheduler_->recycle_mesh(std::move(*result.mesh));
            }
            requeue_latest(result.identity.coordinate);
            continue;
        }
        const auto duplicate_upload =
            std::ranges::find_if(pending_uploads_, [&result](const PendingUpload& upload) {
                return upload.identity == result.identity &&
                       upload.content_revision == result.center_revision &&
                       upload.render_table_revision == result.block_render_table_revision &&
                       std::ranges::equal(upload.dependency_revisions, result.dependency_revisions);
            });
        if (duplicate_upload != pending_uploads_.end()) {
            mesh_scheduler_->recycle_mesh(std::move(*result.mesh));
            continue;
        }

        for (auto& upload : pending_uploads_) {
            if (upload.identity == result.identity) {
                recycle_upload_storage(upload);
            }
        }
        std::erase_if(pending_uploads_, [&result](const PendingUpload& upload) {
            return upload.identity == result.identity;
        });
        {
            profiling::ScopedCpuTimingZone preparation_zone(
                timings_, profiling::CpuTimingZone::upload_preparation);
            auto built_mesh = std::move(*result.mesh);
            PendingUpload upload;
            upload.identity = result.identity;
            upload.content_revision = result.center_revision;
            upload.render_table_revision = result.block_render_table_revision;
            upload.dependency_revisions = std::move(result.dependency_revisions);
            upload.vertices = terrain::make_gpu_chunk_vertices(
                built_mesh.vertices, acquire_gpu_vertices(built_mesh.vertices.size()));
            upload.mesh = std::move(built_mesh);
            upload.sequence = result.priority.sequence;
            pending_uploads_.push_back(std::move(upload));
        }
        cache_->mark_cpu_mesh_ready(result.identity);
        cache_->mark_upload_pending(result.identity);
    }
    return core::Status::ok();
}

core::Status ChunkRenderSystem::schedule_mesh_jobs(world::WorldState& world,
                                                   const RenderCamera& camera) {
    prioritize_mesh_queue(world, camera);
    core::Status first_failure = core::Status::ok();
    const auto available_at_start = pending_meshes_.size();
    std::size_t attempted = 0;
    while (!pending_meshes_.empty() && attempted < available_at_start &&
           stats_.scheduled_mesh_count < config_.max_chunks_meshed_per_frame &&
           mesh_scheduler_->has_capacity()) {
        auto pending = pending_meshes_.front();
        pending_meshes_.erase(pending_meshes_.begin());
        ++attempted;

        auto* chunk = world.chunks().find(pending.identity.coordinate);
        const auto* pending_entry = cache_->find(pending.identity);
        if (chunk == nullptr || chunk->identity() != pending.identity ||
            !cache_->contains(pending.identity) ||
            (pending_entry != nullptr && pending_entry->residency_suppressed) ||
            !within_mesh_distance(pending.identity.coordinate, camera)) {
            continue;
        }
        if (mesh_scheduler_->has_in_flight(pending.identity)) {
            const auto active_revision = mesh_scheduler_->in_flight_revision(pending.identity);
            if (active_revision.has_value() && *active_revision != chunk->content_revision()) {
                mesh_scheduler_->cancel(pending.identity);
            }
            pending_meshes_.push_back(pending);
            continue;
        }

        const auto halo = world::required_chunk_halo(chunk->cells(), *render_table_);
        if (!halo) {
            ++stats_.failed_mesh_count;
            ++stats_.total_failed_mesh_count;
            cache_->mark_failed(pending.identity);
            pending.sequence = next_sequence_++;
            pending_meshes_.push_back(pending);
            if (first_failure) {
                first_failure = core::Status::failure(halo.error().code, halo.error().message);
            }
            continue;
        }
        const auto side = static_cast<std::size_t>(world::VoxelChunk::edge_length) +
                          static_cast<std::size_t>(halo.value()) * 2;
        const auto snapshot_cells = side * side * side;
        if (stats_.snapshot_cells_copied + snapshot_cells > config_.max_snapshot_cells_per_frame) {
            pending_meshes_.insert(pending_meshes_.begin(), pending);
            break;
        }

        auto storage = mesh_scheduler_->acquire_snapshot_cells(snapshot_cells);
        auto snapshot = [&]() {
            profiling::ScopedCpuTimingZone preparation_zone(
                timings_, profiling::CpuTimingZone::chunk_snapshot);
            return world::build_chunk_neighborhood_snapshot(world.chunks(), pending.identity,
                                                            *render_table_, std::move(storage));
        }();
        if (!snapshot) {
            ++stats_.failed_mesh_count;
            ++stats_.total_failed_mesh_count;
            cache_->mark_failed(pending.identity);
            pending.sequence = next_sequence_++;
            pending_meshes_.push_back(pending);
            if (first_failure) {
                first_failure =
                    core::Status::failure(snapshot.error().code, snapshot.error().message);
            }
            continue;
        }
        stats_.snapshot_cells_copied += snapshot.value().cell_count();

        const auto* entry = cache_->find(pending.identity);
        const auto bounds = entry != nullptr && entry->local_bounds.is_valid()
                                ? entry->local_bounds
                                : default_chunk_bounds();
        ChunkMeshRequest request;
        request.identity = pending.identity;
        request.center_revision = snapshot.value().center_revision;
        request.block_render_table_revision = render_table_->revision;
        request.neighborhood = std::move(snapshot).value();
        request.render_table = render_table_;
        request.priority.visible = is_visible(pending.identity.coordinate, bounds, camera);
        request.priority.missing_resident_mesh =
            entry == nullptr || entry->state != ChunkGpuState::resident;
        request.priority.distance_squared =
            distance_squared(pending.identity.coordinate, bounds, camera);
        request.priority.sequence = pending.sequence;
        auto submit_status = mesh_scheduler_->submit(std::move(request));
        if (!submit_status) {
            pending_meshes_.push_back(pending);
            if (submit_status.error().code == "renderer.chunk_mesh_scheduler_full" ||
                submit_status.error().code == "renderer.chunk_mesh_request_coalesced") {
                break;
            }
            if (first_failure) {
                first_failure = submit_status;
            }
            continue;
        }
        ++stats_.scheduled_mesh_count;
    }
    return first_failure;
}

core::Status ChunkRenderSystem::process_upload_queue(world::WorldState& world,
                                                     const RenderCamera& camera) {
    prioritize_upload_queue(camera);
    const auto available_at_start = pending_uploads_.size();
    std::size_t attempted = 0;
    std::size_t selected_bytes = 0;
    std::vector<PendingUpload> selected;
    selected.reserve(std::min(config_.max_chunks_uploaded_per_frame, available_at_start));
    while (!pending_uploads_.empty() && attempted < available_at_start &&
           selected.size() < config_.max_chunks_uploaded_per_frame) {
        const auto bytes = pending_uploads_.front().byte_size();
        const bool budget_available =
            selected_bytes <= config_.max_bytes_uploaded_per_frame &&
            bytes <= config_.max_bytes_uploaded_per_frame - selected_bytes;
        if (!budget_available && !selected.empty()) {
            break;
        }

        auto pending = std::move(pending_uploads_.front());
        pending_uploads_.erase(pending_uploads_.begin());
        ++attempted;
        if (!pending_upload_is_current(pending, world)) {
            const auto* newest = world.chunks().find(pending.identity.coordinate);
            if (newest != nullptr && cache_->contains(newest->identity())) {
                enqueue_mesh(newest->identity(), true);
            }
            recycle_upload_storage(pending);
            continue;
        }
        if (!within_mesh_distance(pending.identity.coordinate, camera)) {
            recycle_upload_storage(pending);
            continue;
        }
        const auto* pending_entry = cache_->find(pending.identity);
        if (pending_entry != nullptr && pending_entry->residency_suppressed &&
            !should_restore_suppressed_residency(*pending_entry, camera)) {
            recycle_upload_storage(pending);
            continue;
        }
        selected_bytes += pending.byte_size();
        selected.push_back(std::move(pending));
    }
    if (selected.empty()) {
        return core::Status::ok();
    }

    std::vector<ChunkGpuMeshUpload> requests;
    requests.reserve(selected.size());
    for (const auto& pending : selected) {
        requests.push_back({pending.identity, pending.content_revision,
                            pending.render_table_revision, pending.mesh.local_bounds,
                            pending.vertices, pending.mesh.indices, pending.dependency_revisions,
                            pending.mesh.sections});
    }
    auto upload = [&]() {
        profiling::ScopedCpuTimingZone upload_zone(timings_, profiling::CpuTimingZone::upload);
        return cache_->replace_meshes(requests);
    }();
    if (!upload) {
        stats_.failed_upload_count += selected.size();
        stats_.total_failed_upload_count += selected.size();
        for (auto& pending : selected) {
            pending.sequence = next_sequence_++;
            pending_uploads_.push_back(std::move(pending));
        }
        return core::Status::failure(upload.error().code, upload.error().message);
    }
    stats_.gpu_wait_ms += upload.value().cpu_gpu_wait_ms;

    for (std::size_t index = 0; index < selected.size(); ++index) {
        const auto& pending = selected[index];
        ++stats_.uploaded_chunk_count;
        stats_.uploaded_bytes += upload.value().uploads[index].uploaded_bytes;
        auto* chunk = world.chunks().find(pending.identity.coordinate);
        if (chunk != nullptr && pending_upload_is_current(pending, world)) {
            chunk->clear_dirty(world::ChunkDirtyFlag::mesh);
        } else {
            const auto* newest = world.chunks().find(pending.identity.coordinate);
            if (newest != nullptr && cache_->contains(newest->identity())) {
                enqueue_mesh(newest->identity(), true);
            }
        }
        recycle_upload_storage(selected[index]);
    }
    return core::Status::ok();
}

void ChunkRenderSystem::prioritize_mesh_queue(const world::WorldState& world,
                                              const RenderCamera& camera) {
    std::ranges::stable_sort(pending_meshes_, [this, &camera](const PendingMesh& left,
                                                              const PendingMesh& right) {
        const auto* left_entry = cache_->find(left.identity);
        const auto* right_entry = cache_->find(right.identity);
        const auto left_bounds = left_entry != nullptr && left_entry->local_bounds.is_valid()
                                     ? left_entry->local_bounds
                                     : default_chunk_bounds();
        const auto right_bounds = right_entry != nullptr && right_entry->local_bounds.is_valid()
                                      ? right_entry->local_bounds
                                      : default_chunk_bounds();
        const bool left_visible = is_visible(left.identity.coordinate, left_bounds, camera);
        const bool right_visible = is_visible(right.identity.coordinate, right_bounds, camera);
        if (left_visible != right_visible) {
            return left_visible;
        }
        const bool left_missing =
            left_entry == nullptr || left_entry->state != ChunkGpuState::resident;
        const bool right_missing =
            right_entry == nullptr || right_entry->state != ChunkGpuState::resident;
        if (left_missing != right_missing) {
            return left_missing;
        }
        const auto left_distance = distance_squared(left.identity.coordinate, left_bounds, camera);
        const auto right_distance =
            distance_squared(right.identity.coordinate, right_bounds, camera);
        if (left_distance != right_distance) {
            return left_distance < right_distance;
        }
        return left.sequence < right.sequence;
    });
    (void)world;
}

void ChunkRenderSystem::prioritize_upload_queue(const RenderCamera& camera) {
    std::ranges::stable_sort(
        pending_uploads_, [this, &camera](const PendingUpload& left, const PendingUpload& right) {
            const bool left_visible =
                is_visible(left.identity.coordinate, left.mesh.local_bounds, camera);
            const bool right_visible =
                is_visible(right.identity.coordinate, right.mesh.local_bounds, camera);
            if (left_visible != right_visible) {
                return left_visible;
            }
            const auto left_distance =
                distance_squared(left.identity.coordinate, left.mesh.local_bounds, camera);
            const auto right_distance =
                distance_squared(right.identity.coordinate, right.mesh.local_bounds, camera);
            if (left_distance != right_distance) {
                return left_distance < right_distance;
            }
            return left.sequence < right.sequence;
        });
}

std::vector<terrain::GpuChunkVertex>
ChunkRenderSystem::acquire_gpu_vertices(std::size_t minimum_capacity) {
    auto best = gpu_vertex_pool_.end();
    for (auto candidate = gpu_vertex_pool_.begin(); candidate != gpu_vertex_pool_.end();
         ++candidate) {
        if (candidate->capacity() >= minimum_capacity &&
            (best == gpu_vertex_pool_.end() || candidate->capacity() < best->capacity())) {
            best = candidate;
        }
    }
    if (best != gpu_vertex_pool_.end()) {
        auto result = std::move(*best);
        gpu_vertex_pool_.erase(best);
        return result;
    }
    std::vector<terrain::GpuChunkVertex> result;
    result.reserve(minimum_capacity);
    return result;
}

void ChunkRenderSystem::recycle_upload_storage(PendingUpload& upload) noexcept {
    if (mesh_scheduler_ != nullptr) {
        mesh_scheduler_->recycle_mesh(std::move(upload.mesh));
    }
    upload.vertices.clear();
    if (gpu_vertex_pool_.size() < config_.max_cached_mesh_buffers) {
        gpu_vertex_pool_.push_back(std::move(upload.vertices));
    }
}

bool ChunkRenderSystem::is_visible(world::ChunkCoord coord, math::Bounds3f local_bounds,
                                   const RenderCamera& camera) const {
    if (!within_visible_distance(coord, camera)) {
        return false;
    }
    const auto origin = camera_relative_chunk_origin(coord, camera);
    if (!origin) {
        return false;
    }
    const auto frustum = RenderFrustum::from_view_projection(camera.view_projection);
    return frustum.intersects(translated_bounds(local_bounds, origin.value()));
}

bool ChunkRenderSystem::within_mesh_distance(world::ChunkCoord coord,
                                             const RenderCamera& camera) const {
    const auto center = camera_chunk_coordinate(camera);
    return center &&
           within_cylinder(coord, center.value(), config_.distances.mesh_horizontal_radius,
                           config_.distances.mesh_vertical_radius);
}

bool ChunkRenderSystem::within_visible_distance(world::ChunkCoord coord,
                                                const RenderCamera& camera) const {
    const auto center = camera_chunk_coordinate(camera);
    return center &&
           within_cylinder(coord, center.value(), config_.distances.visible_horizontal_radius,
                           config_.distances.visible_vertical_radius);
}

bool ChunkRenderSystem::within_gpu_retention_distance(world::ChunkCoord coord,
                                                      const RenderCamera& camera) const {
    const auto center = camera_chunk_coordinate(camera);
    if (!center) {
        return false;
    }
    const auto horizontal =
        static_cast<std::uint16_t>(config_.distances.gpu_resident_horizontal_radius +
                                   config_.distances.gpu_resident_hysteresis);
    const auto vertical = static_cast<std::uint16_t>(
        config_.distances.gpu_resident_vertical_radius + config_.distances.gpu_resident_hysteresis);
    return within_cylinder(coord, center.value(), horizontal, vertical);
}

core::Status ChunkRenderSystem::enforce_distance_residency(const RenderCamera& camera) {
    std::vector<world::ChunkIdentity> releases;
    for (const auto* entry : cache_->entries()) {
        if (entry->state == ChunkGpuState::resident &&
            !within_gpu_retention_distance(entry->identity.coordinate, camera)) {
            releases.push_back(entry->identity);
        }
    }
    for (const auto identity : releases) {
        const auto* entry = cache_->find(identity);
        if (entry != nullptr) {
            ++stats_.distance_evicted_mesh_count;
            stats_.distance_evicted_mesh_bytes += entry->resident_bytes();
        }
        auto status = cache_->release_mesh(identity);
        if (!status) {
            return status;
        }
        auto* released = cache_->find(identity);
        if (released != nullptr) {
            released->residency_suppressed = false;
        }
    }
    return core::Status::ok();
}

core::Status ChunkRenderSystem::enforce_gpu_residency_budget(const RenderCamera& camera) {
    while (cache_->stats().resident_bytes > config_.gpu_terrain_budget_bytes) {
        std::vector<const ChunkGpuEntry*> candidates;
        for (const auto* entry : cache_->entries()) {
            if (entry->state == ChunkGpuState::resident && entry->resident_bytes() > 0) {
                candidates.push_back(entry);
            }
        }
        if (candidates.empty()) {
            return core::Status::failure(
                "renderer.gpu_terrain_budget_unrecoverable",
                "resident terrain bytes exceed the budget without an evictable mesh");
        }
        std::ranges::stable_sort(
            candidates, [this, &camera](const ChunkGpuEntry* left, const ChunkGpuEntry* right) {
                const bool left_visible =
                    is_visible(left->identity.coordinate, left->local_bounds, camera);
                const bool right_visible =
                    is_visible(right->identity.coordinate, right->local_bounds, camera);
                if (left_visible != right_visible) {
                    return !left_visible;
                }
                if (left->last_visible_epoch != right->last_visible_epoch) {
                    return left->last_visible_epoch < right->last_visible_epoch;
                }
                const auto left_distance =
                    distance_squared(left->identity.coordinate, left->local_bounds, camera);
                const auto right_distance =
                    distance_squared(right->identity.coordinate, right->local_bounds, camera);
                if (left_distance != right_distance) {
                    return left_distance > right_distance;
                }
                return left->identity < right->identity;
            });
        const auto identity = candidates.front()->identity;
        const auto bytes = candidates.front()->resident_bytes();
        auto status = cache_->release_mesh(identity);
        if (!status) {
            return status;
        }
        auto* released = cache_->find(identity);
        if (released != nullptr) {
            released->residency_suppressed = true;
        }
        mesh_scheduler_->cancel(identity);
        remove_pending(identity);
        ++stats_.memory_pressure_evicted_mesh_count;
        stats_.memory_pressure_evicted_mesh_bytes += bytes;
    }
    return core::Status::ok();
}

bool ChunkRenderSystem::should_restore_suppressed_residency(const ChunkGpuEntry& entry,
                                                            const RenderCamera& camera) const {
    const auto resident_bytes = cache_->stats().resident_bytes;
    if (entry.last_resident_bytes <=
        config_.gpu_terrain_budget_bytes -
            std::min(config_.gpu_terrain_budget_bytes, resident_bytes)) {
        return true;
    }
    if (!is_visible(entry.identity.coordinate, entry.local_bounds, camera)) {
        return false;
    }

    const ChunkGpuEntry* least_useful_resident = nullptr;
    for (const auto* candidate : cache_->entries()) {
        if (candidate->state != ChunkGpuState::resident || candidate->resident_bytes() == 0) {
            continue;
        }
        if (least_useful_resident == nullptr) {
            least_useful_resident = candidate;
            continue;
        }
        const bool candidate_visible =
            is_visible(candidate->identity.coordinate, candidate->local_bounds, camera);
        const bool current_visible = is_visible(least_useful_resident->identity.coordinate,
                                                least_useful_resident->local_bounds, camera);
        const auto candidate_distance =
            distance_squared(candidate->identity.coordinate, candidate->local_bounds, camera);
        const auto current_distance = distance_squared(least_useful_resident->identity.coordinate,
                                                       least_useful_resident->local_bounds, camera);
        if ((!candidate_visible && current_visible) ||
            (candidate_visible == current_visible && candidate_distance > current_distance)) {
            least_useful_resident = candidate;
        }
    }
    if (least_useful_resident == nullptr) {
        return true;
    }
    const bool resident_visible = is_visible(least_useful_resident->identity.coordinate,
                                             least_useful_resident->local_bounds, camera);
    if (!resident_visible) {
        return true;
    }
    return distance_squared(entry.identity.coordinate, entry.local_bounds, camera) <
           distance_squared(least_useful_resident->identity.coordinate,
                            least_useful_resident->local_bounds, camera);
}

float ChunkRenderSystem::distance_squared(world::ChunkCoord coord, math::Bounds3f local_bounds,
                                          const RenderCamera& camera) const {
    const auto origin = camera_relative_chunk_origin(coord, camera);
    if (!origin) {
        return std::numeric_limits<float>::max();
    }
    const auto center = local_bounds.is_valid()
                            ? (local_bounds.min + local_bounds.max) * 0.5F + origin.value()
                            : origin.value();
    return math::length_squared(center - camera.local_position);
}

core::Result<math::Vec3f>
ChunkRenderSystem::camera_relative_chunk_origin(world::ChunkCoord coord,
                                                const RenderCamera& camera) {
    auto block = world::chunk_local_to_block(coord, {0, 0, 0});
    if (!block) {
        return core::Result<math::Vec3f>::failure(block.error().code, block.error().message);
    }
    world::WorldPosition position;
    position.anchor = block.value();
    return world::to_camera_relative(position, camera.floating_origin);
}

void ChunkRenderSystem::refresh_queue_stats() noexcept {
    stats_.cache = cache_->stats();
    const auto scheduler_stats =
        mesh_scheduler_ == nullptr ? ChunkMeshSchedulerStats{} : mesh_scheduler_->stats();
    stats_.in_flight_mesh_count = scheduler_stats.in_flight_jobs;
    stats_.pooled_cpu_mesh_buffers = scheduler_stats.pooled_mesh_buffers;
    stats_.pooled_cpu_mesh_vertex_capacity = scheduler_stats.pooled_mesh_vertex_capacity;
    stats_.pooled_cpu_mesh_index_capacity = scheduler_stats.pooled_mesh_index_capacity;
    stats_.pooled_gpu_vertex_buffers = gpu_vertex_pool_.size();
    stats_.pooled_gpu_vertex_capacity = 0;
    for (const auto& vertices : gpu_vertex_pool_) {
        stats_.pooled_gpu_vertex_capacity += vertices.capacity();
    }
    stats_.pending_mesh_count = pending_meshes_.size() + stats_.in_flight_mesh_count;
    stats_.pending_upload_count = pending_uploads_.size();
    stats_.pending_upload_bytes = 0;
    for (const auto& upload : pending_uploads_) {
        stats_.pending_upload_bytes += upload.byte_size();
    }
    stats_.gpu_terrain_budget_bytes = config_.gpu_terrain_budget_bytes;
    stats_.residency_suppressed_chunk_count = 0;
    for (const auto* entry : cache_->entries()) {
        stats_.residency_suppressed_chunk_count += entry->residency_suppressed ? 1U : 0U;
    }
}

void ChunkRenderSystem::refresh_timing_stats() noexcept {
    stats_.culling_ms = timings_.milliseconds(profiling::CpuTimingZone::visibility_culling);
    stats_.draw_list_ms = timings_.milliseconds(profiling::CpuTimingZone::draw_list_construction);
    stats_.chunk_snapshot_ms = timings_.milliseconds(profiling::CpuTimingZone::chunk_snapshot);
    stats_.upload_preparation_ms =
        timings_.milliseconds(profiling::CpuTimingZone::upload_preparation);
    stats_.upload_ms = timings_.milliseconds(profiling::CpuTimingZone::upload);
}

} // namespace heartstead::renderer
