#include "engine/physics/chunk_collision_system.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace heartstead::physics {

namespace {

[[nodiscard]] bool region_contains(const dirty::DirtyRegionBounds& bounds,
                                   world::ChunkCoord coordinate) noexcept {
    return coordinate.x >= bounds.min.x && coordinate.x <= bounds.max.x &&
           coordinate.y >= bounds.min.y && coordinate.y <= bounds.max.y &&
           coordinate.z >= bounds.min.z && coordinate.z <= bounds.max.z;
}

[[nodiscard]] std::uint64_t terrain_user_data(world::ChunkCoord coordinate) noexcept {
    std::uint64_t value = 1'469'598'103'934'665'603ULL;
    const auto mix = [&value](std::int64_t coordinate_value) {
        value ^= static_cast<std::uint64_t>(coordinate_value);
        value *= 1'099'511'628'211ULL;
    };
    mix(coordinate.x);
    mix(coordinate.y);
    mix(coordinate.z);
    return value | (std::uint64_t{1} << 63U);
}

} // namespace

core::Status ChunkCollisionSystemConfig::validate() const {
    auto scheduler_status = scheduler.validate();
    if (!scheduler_status) {
        return scheduler_status;
    }
    if (!std::isfinite(physics_island.max_local_extent) ||
        physics_island.max_local_extent <= 0.0F || max_submissions_per_update == 0 ||
        max_applies_per_update == 0 || !std::isfinite(apply_time_budget_ms) ||
        apply_time_budget_ms <= 0.0) {
        return core::Status::failure(
            "chunk_collision.invalid_system_config",
            "chunk collision system budgets and physics-island extent must be positive");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<ChunkCollisionSystem>>
ChunkCollisionSystem::create(IPhysicsWorld& physics_world, const world::VoxelPalette& palette,
                             ChunkCollisionSystemConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkCollisionSystem>>::failure(status.error().code,
                                                                            status.error().message);
    }
    auto table = world::build_chunk_collision_table_snapshot(&palette);
    if (!table) {
        return core::Result<std::unique_ptr<ChunkCollisionSystem>>::failure(table.error().code,
                                                                            table.error().message);
    }
    auto scheduler = ChunkCollisionScheduler::create(config.scheduler);
    if (!scheduler) {
        return core::Result<std::unique_ptr<ChunkCollisionSystem>>::failure(
            scheduler.error().code, scheduler.error().message);
    }
    auto shared_table =
        std::make_shared<const world::ChunkCollisionTableSnapshot>(std::move(table).value());
    return core::Result<std::unique_ptr<ChunkCollisionSystem>>::success(
        std::unique_ptr<ChunkCollisionSystem>(new ChunkCollisionSystem(
            physics_world, config, std::move(scheduler).value(), std::move(shared_table))));
}

ChunkCollisionSystem::ChunkCollisionSystem(
    IPhysicsWorld& physics_world, ChunkCollisionSystemConfig config,
    std::unique_ptr<ChunkCollisionScheduler> scheduler,
    std::shared_ptr<const world::ChunkCollisionTableSnapshot> table)
    : physics_world_(&physics_world), config_(config), scheduler_(std::move(scheduler)),
      collision_table_(std::move(table)) {}

ChunkCollisionSystem::~ChunkCollisionSystem() {
    shutdown();
}

core::Status ChunkCollisionSystem::update(world::ChunkDatabase& chunks,
                                          dirty::DirtyRegionTracker& dirty_regions,
                                          const world::VoxelPalette& palette) {
    stats_.submitted_this_update = 0;
    stats_.applied_this_update = 0;
    stats_.removed_this_update = 0;
    stats_.last_apply_ms = 0.0;

    auto status = refresh_collision_table(palette, chunks);
    if (!status) {
        return status;
    }
    collect_dirty_chunks(chunks, dirty_regions);
    status = reconcile_unloaded_chunks(chunks);
    if (!status) {
        return status;
    }
    status = apply_completed(chunks);
    if (!status) {
        return status;
    }
    status = submit_pending(chunks);
    refresh_stats();
    return status;
}

void ChunkCollisionSystem::shutdown() noexcept {
    if (scheduler_ != nullptr) {
        scheduler_->shutdown();
        scheduler_.reset();
    }
    if (physics_world_ != nullptr) {
        for (const auto& [_, record] : bodies_) {
            (void)physics_world_->destroy_body(record.body_id);
        }
    }
    bodies_.clear();
    pending_chunks_.clear();
    refresh_stats();
    physics_world_ = nullptr;
}

const ChunkCollisionBodyRecord*
ChunkCollisionSystem::find(world::ChunkCoord coordinate) const noexcept {
    const auto found = bodies_.find(coordinate);
    return found == bodies_.end() ? nullptr : &found->second;
}

const ChunkCollisionSystemStats& ChunkCollisionSystem::stats() noexcept {
    refresh_stats();
    return stats_;
}

const ChunkCollisionSystemStats& ChunkCollisionSystem::stats() const noexcept {
    return stats_;
}

core::Status ChunkCollisionSystem::refresh_collision_table(const world::VoxelPalette& palette,
                                                           const world::ChunkDatabase& chunks) {
    if (collision_table_ != nullptr && collision_table_->revision == palette.render_revision()) {
        return core::Status::ok();
    }
    auto table = world::build_chunk_collision_table_snapshot(&palette);
    if (!table) {
        return core::Status::failure(table.error().code, table.error().message);
    }
    collision_table_ =
        std::make_shared<const world::ChunkCollisionTableSnapshot>(std::move(table).value());
    for (const auto* chunk : chunks.records()) {
        pending_chunks_.insert(chunk->coord());
    }
    return core::Status::ok();
}

void ChunkCollisionSystem::collect_dirty_chunks(world::ChunkDatabase& chunks,
                                                dirty::DirtyRegionTracker& dirty_regions) {
    const auto regions = dirty_regions.consume_kind(dirty::DirtyRegionKind::chunk_collision);
    if (!regions.empty()) {
        for (const auto* chunk : chunks.records()) {
            if (std::ranges::any_of(regions, [chunk](const dirty::DirtyRegion& region) {
                    return region_contains(region.bounds, chunk->coord());
                })) {
                pending_chunks_.insert(chunk->coord());
            }
        }
    }
    for (const auto* chunk : chunks.records()) {
        if (chunk->dirty().contains(world::ChunkDirtyFlag::collision)) {
            pending_chunks_.insert(chunk->coord());
        }
    }
}

core::Status ChunkCollisionSystem::reconcile_unloaded_chunks(const world::ChunkDatabase& chunks) {
    for (auto body = bodies_.begin(); body != bodies_.end();) {
        const auto* chunk = chunks.find(body->first);
        if (chunk != nullptr && chunk->identity() == body->second.identity) {
            ++body;
            continue;
        }
        scheduler_->cancel(body->second.identity);
        auto status = physics_world_->destroy_body(body->second.body_id);
        if (!status) {
            return status;
        }
        stats_.current_collision_boxes -= body->second.box_count;
        ++stats_.removed_bodies;
        ++stats_.removed_this_update;
        if (chunk != nullptr) {
            pending_chunks_.insert(chunk->coord());
        }
        body = bodies_.erase(body);
    }
    return core::Status::ok();
}

core::Status ChunkCollisionSystem::apply_completed(world::ChunkDatabase& chunks) {
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    while (stats_.applied_this_update < config_.max_applies_per_update) {
        const auto elapsed_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        if (elapsed_ms >= config_.apply_time_budget_ms) {
            break;
        }
        auto completed = scheduler_->drain_completed(1);
        if (completed.empty()) {
            break;
        }
        auto status = apply_result(chunks, std::move(completed.front()));
        if (!status) {
            ++stats_.failed_results;
            return status;
        }
    }
    stats_.last_apply_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return core::Status::ok();
}

core::Status ChunkCollisionSystem::submit_pending(const world::ChunkDatabase& chunks) {
    std::size_t submitted = 0;
    for (auto pending = pending_chunks_.begin();
         pending != pending_chunks_.end() && submitted < config_.max_submissions_per_update;) {
        const auto* chunk = chunks.find(*pending);
        if (chunk == nullptr) {
            pending = pending_chunks_.erase(pending);
            continue;
        }
        if (scheduler_->has_in_flight(chunk->identity())) {
            ++pending;
            continue;
        }
        if (!scheduler_->has_capacity()) {
            break;
        }
        auto storage = scheduler_->acquire_snapshot_cells(world::VoxelChunk::total_cells);
        auto snapshot = world::build_chunk_collision_snapshot(
            chunks, chunk->identity(), *collision_table_, std::move(storage));
        if (!snapshot) {
            return core::Status::failure(snapshot.error().code, snapshot.error().message);
        }
        ChunkCollisionRequest request;
        request.snapshot = std::move(snapshot).value();
        request.collision_table = collision_table_;
        auto status = scheduler_->submit(std::move(request));
        if (!status) {
            return status;
        }
        pending = pending_chunks_.erase(pending);
        ++submitted;
        ++stats_.submitted_this_update;
    }
    return core::Status::ok();
}

core::Status ChunkCollisionSystem::apply_result(world::ChunkDatabase& chunks,
                                                ChunkCollisionResult result) {
    stats_.last_cooking_ms = result.cooking_ms;
    const auto* chunk = chunks.find(result.identity.coordinate);
    if (result.state == ChunkCollisionResultState::cancelled || chunk == nullptr ||
        chunk->identity() != result.identity ||
        chunk->content_revision() != result.center_revision ||
        collision_table_->revision != result.collision_table_revision) {
        ++stats_.stale_results;
        if (chunk != nullptr) {
            pending_chunks_.insert(chunk->coord());
        }
        return core::Status::ok();
    }
    if (result.state == ChunkCollisionResultState::failed || !result.shape.has_value()) {
        return core::Status::failure(
            result.error_code.empty() ? "chunk_collision.cook_failed" : result.error_code,
            result.error_message.empty() ? "chunk collision cook failed" : result.error_message);
    }
    auto shape_status = result.shape->validate();
    if (!shape_status) {
        return shape_status;
    }

    PhysicsBodyId new_body;
    if (!result.shape->empty()) {
        auto position = chunk_physics_position(result.identity.coordinate);
        if (!position) {
            return core::Status::failure(position.error().code, position.error().message);
        }
        PhysicsBodyDesc desc;
        desc.motion_type = BodyMotionType::static_body;
        desc.position = position.value();
        desc.user_data = terrain_user_data(result.identity.coordinate);
        desc.shape.kind = ShapeKind::compound;
        desc.shape.children = result.shape->boxes;
        auto created = physics_world_->create_body(std::move(desc));
        if (!created) {
            return core::Status::failure(created.error().code, created.error().message);
        }
        new_body = created.value();
    }

    const auto existing = bodies_.find(result.identity.coordinate);
    if (existing != bodies_.end()) {
        auto status = physics_world_->destroy_body(existing->second.body_id);
        if (!status) {
            if (new_body.is_valid()) {
                (void)physics_world_->destroy_body(new_body);
            }
            return status;
        }
        stats_.current_collision_boxes -= existing->second.box_count;
        bodies_.erase(existing);
    }
    if (new_body.is_valid()) {
        const auto box_count = static_cast<std::uint32_t>(result.shape->boxes.size());
        bodies_.emplace(result.identity.coordinate,
                        ChunkCollisionBodyRecord{result.identity, result.center_revision,
                                                 result.collision_table_revision, new_body,
                                                 box_count});
        stats_.current_collision_boxes += box_count;
    }
    if (auto* mutable_chunk = chunks.find(result.identity.coordinate);
        mutable_chunk != nullptr && mutable_chunk->identity() == result.identity &&
        mutable_chunk->content_revision() == result.center_revision) {
        mutable_chunk->clear_dirty(world::ChunkDirtyFlag::collision);
        pending_chunks_.erase(result.identity.coordinate);
    }
    ++stats_.applied_shapes;
    ++stats_.applied_this_update;
    return core::Status::ok();
}

core::Result<Vec3>
ChunkCollisionSystem::chunk_physics_position(world::ChunkCoord coordinate) const {
    auto block = world::chunk_local_to_block(coordinate, {0, 0, 0});
    if (!block) {
        return core::Result<Vec3>::failure(block.error().code, block.error().message);
    }
    auto position = world::WorldPosition::from_anchor(block.value(), {});
    if (!position) {
        return core::Result<Vec3>::failure(position.error().code, position.error().message);
    }
    return world::to_physics_local(position.value(), config_.physics_island);
}

void ChunkCollisionSystem::refresh_stats() noexcept {
    stats_.resident_body_count = bodies_.size();
    stats_.pending_chunk_count = pending_chunks_.size();
    if (scheduler_ != nullptr) {
        const auto& scheduler_stats = scheduler_->stats();
        stats_.in_flight_job_count = scheduler_stats.in_flight_jobs;
        stats_.completed_mailbox_count = scheduler_stats.completed_mailbox_count;
    } else {
        stats_.in_flight_job_count = 0;
        stats_.completed_mailbox_count = 0;
    }
}

} // namespace heartstead::physics
