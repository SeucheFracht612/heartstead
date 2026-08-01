#include "engine/physics/chunk_collision_system.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <exception>
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

[[nodiscard]] std::uint64_t
collision_shape_fingerprint(const world::ChunkCollisionShape& shape) noexcept {
    std::uint64_t value = 1'469'598'103'934'665'603ULL;
    const auto mix = [&value](std::uint64_t component) {
        value ^= component;
        value *= 1'099'511'628'211ULL;
    };
    const auto mix_float = [&mix](float component) {
        mix(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(component)));
    };
    mix(static_cast<std::uint64_t>(shape.boxes.size()));
    for (const auto& box : shape.boxes) {
        mix(static_cast<std::uint64_t>(box.kind));
        mix_float(box.local_position.x);
        mix_float(box.local_position.y);
        mix_float(box.local_position.z);
        mix_float(box.half_extents.x);
        mix_float(box.half_extents.y);
        mix_float(box.half_extents.z);
        mix_float(box.radius);
        mix_float(box.half_height);
    }
    return value;
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
    stats_.body_changes_this_update = 0;
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
    if (stats_.body_changes_this_update > 0 || stats_.removed_this_update > 0) {
        if (world_revision_ == std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++world_revision_;
    }
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

std::uint64_t ChunkCollisionSystem::world_revision() const noexcept {
    return world_revision_;
}

const ChunkCollisionSystemStats& ChunkCollisionSystem::stats() noexcept {
    refresh_stats();
    return stats_;
}

const ChunkCollisionSystemStats& ChunkCollisionSystem::stats() const noexcept {
    return stats_;
}

core::Status ChunkCollisionSystem::refresh_collision_table(const world::VoxelPalette& palette,
                                                           world::ChunkDatabase& chunks) {
    if (collision_table_ != nullptr && collision_table_->revision == palette.render_revision()) {
        return core::Status::ok();
    }
    auto table = world::build_chunk_collision_table_snapshot(&palette);
    if (!table) {
        return core::Status::failure(table.error().code, table.error().message);
    }
    collision_table_ =
        std::make_shared<const world::ChunkCollisionTableSnapshot>(std::move(table).value());
    for (const auto identity : chunks.identities()) {
        auto* chunk = chunks.find(identity.coordinate);
        if (chunk != nullptr && chunk->identity() == identity) {
            static_cast<void>(chunk->request_stage(world::ChunkStage::collision));
            pending_chunks_.insert(chunk->coord());
        }
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
        const auto active_stage_revision = scheduler_->in_flight_stage_revision(chunk->identity());
        if (active_stage_revision.has_value() &&
            *active_stage_revision !=
                chunk->stages().requested_revision(world::ChunkStage::collision)) {
            scheduler_->cancel(chunk->identity());
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

core::Status ChunkCollisionSystem::submit_pending(world::ChunkDatabase& chunks) {
    std::size_t submitted = 0;
    for (auto pending = pending_chunks_.begin();
         pending != pending_chunks_.end() && submitted < config_.max_submissions_per_update;) {
        auto* chunk = chunks.find(*pending);
        if (chunk == nullptr) {
            pending = pending_chunks_.erase(pending);
            continue;
        }
        if (scheduler_->has_in_flight(chunk->identity())) {
            const auto active_stage_revision =
                scheduler_->in_flight_stage_revision(chunk->identity());
            if (active_stage_revision.has_value() &&
                *active_stage_revision !=
                    chunk->stages().requested_revision(world::ChunkStage::collision)) {
                scheduler_->cancel(chunk->identity());
            }
            ++pending;
            continue;
        }
        if (!scheduler_->has_capacity()) {
            break;
        }
        auto stage_ticket = chunk->stage_ticket(world::ChunkStage::collision);
        if (chunk->stages().record(world::ChunkStage::collision).state !=
            world::ChunkStageState::requested) {
            stage_ticket = chunk->request_stage(world::ChunkStage::collision);
        }
        if (!stage_ticket.is_valid()) {
            return core::Status::failure("chunk_collision.invalid_stage_ticket",
                                         "chunk collision work requires a valid stage ticket");
        }

        auto storage = scheduler_->acquire_snapshot_cells(world::VoxelChunk::total_cells);
        auto snapshot = world::build_chunk_collision_snapshot(
            chunks, chunk->identity(), *collision_table_, std::move(storage));
        if (!snapshot) {
            return core::Status::failure(snapshot.error().code, snapshot.error().message);
        }
        ChunkCollisionRequest request;
        request.stage_ticket = stage_ticket;
        request.snapshot = std::move(snapshot).value();
        request.collision_table = collision_table_;
        auto status = scheduler_->submit(std::move(request));
        if (!status) {
            return status;
        }
        status = chunk->mark_stage_running(stage_ticket);
        if (!status) {
            scheduler_->cancel(chunk->identity());
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
    auto* chunk = chunks.find(result.identity.coordinate);
    const bool same_identity = chunk != nullptr && chunk->identity() == result.identity;
    if (result.state == ChunkCollisionResultState::cancelled) {
        ++stats_.stale_results;
        if (same_identity) {
            static_cast<void>(chunk->note_stage_cancelled(result.stage_ticket));
            pending_chunks_.insert(chunk->coord());
        }
        return core::Status::ok();
    }
    if (!same_identity || !chunk->stage_ticket_is_current(result.stage_ticket) ||
        chunk->content_revision() != result.center_revision || collision_table_ == nullptr ||
        collision_table_->revision != result.collision_table_revision) {
        ++stats_.stale_results;
        if (same_identity) {
            static_cast<void>(chunk->note_stage_stale(result.stage_ticket));
            pending_chunks_.insert(chunk->coord());
        }
        return core::Status::ok();
    }

    const auto fail_and_retry = [this, &chunks, &result](core::Status failure) {
        auto* current = chunks.find(result.identity.coordinate);
        if (current != nullptr && current->identity() == result.identity &&
            current->stage_ticket_is_current(result.stage_ticket)) {
            auto retry = current->retry_stage(result.stage_ticket);
            if (!retry) {
                return retry;
            }
            pending_chunks_.insert(current->coord());
        }
        return failure;
    };
    if (result.state == ChunkCollisionResultState::failed || !result.shape.has_value()) {
        return fail_and_retry(core::Status::failure(
            result.error_code.empty() ? "chunk_collision.cook_failed" : result.error_code,
            result.error_message.empty() ? "chunk collision cook failed" : result.error_message));
    }
    auto shape_status = result.shape->validate();
    if (!shape_status) {
        return fail_and_retry(shape_status);
    }
    if (result.shape->identity != result.identity ||
        result.shape->content_revision != result.center_revision ||
        result.shape->collision_table_revision != result.collision_table_revision) {
        return fail_and_retry(
            core::Status::failure("chunk_collision.shape_revision_mismatch",
                                  "chunk collision shape does not match its publication metadata"));
    }
    auto stage_status = chunk->mark_stage_ready(result.stage_ticket);
    if (!stage_status) {
        return stage_status;
    }

    const auto shape_fingerprint = collision_shape_fingerprint(*result.shape);
    const auto existing = bodies_.find(result.identity.coordinate);
    const bool geometry_is_unchanged = (result.shape->empty() && existing == bodies_.end()) ||
                                       (!result.shape->empty() && existing != bodies_.end() &&
                                        existing->second.box_count == result.shape->boxes.size() &&
                                        existing->second.shape_fingerprint == shape_fingerprint);
    if (geometry_is_unchanged) {
        stage_status = chunk->publish_stage(result.stage_ticket, false);
        if (!stage_status) {
            return stage_status;
        }
        if (existing != bodies_.end()) {
            existing->second.identity = result.identity;
            existing->second.content_revision = result.center_revision;
            existing->second.collision_table_revision = result.collision_table_revision;
        }
        chunk->clear_dirty(world::ChunkDirtyFlag::collision);
        pending_chunks_.erase(result.identity.coordinate);
        ++stats_.applied_shapes;
        ++stats_.applied_this_update;
        return core::Status::ok();
    }

    PhysicsBodyId new_body;
    if (!result.shape->empty()) {
        auto position = chunk_physics_position(result.identity.coordinate);
        if (!position) {
            return fail_and_retry(
                core::Status::failure(position.error().code, position.error().message));
        }
        PhysicsBodyDesc desc;
        desc.motion_type = BodyMotionType::static_body;
        desc.position = position.value();
        desc.user_data = terrain_user_data(result.identity.coordinate);
        desc.shape.kind = ShapeKind::compound;
        desc.shape.children = result.shape->boxes;
        auto created = physics_world_->create_body(std::move(desc));
        if (!created) {
            return fail_and_retry(
                core::Status::failure(created.error().code, created.error().message));
        }
        new_body = created.value();
    }

    if (existing != bodies_.end()) {
        auto status = physics_world_->destroy_body(existing->second.body_id);
        if (!status) {
            if (new_body.is_valid()) {
                (void)physics_world_->destroy_body(new_body);
            }
            return fail_and_retry(status);
        }
        stats_.current_collision_boxes -= existing->second.box_count;
        bodies_.erase(existing);
    }
    if (new_body.is_valid()) {
        const auto box_count = static_cast<std::uint32_t>(result.shape->boxes.size());
        bodies_.emplace(result.identity.coordinate,
                        ChunkCollisionBodyRecord{result.identity, result.center_revision,
                                                 result.collision_table_revision, shape_fingerprint,
                                                 new_body, box_count});
        stats_.current_collision_boxes += box_count;
    }
    stage_status = chunk->publish_stage(result.stage_ticket);
    if (!stage_status) {
        return stage_status;
    }
    chunk->clear_dirty(world::ChunkDirtyFlag::collision);
    pending_chunks_.erase(result.identity.coordinate);
    ++stats_.applied_shapes;
    ++stats_.applied_this_update;
    ++stats_.body_changes_this_update;
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
    auto local = world::to_physics_local(position.value(), config_.physics_island);
    if (!local) {
        return core::Result<Vec3>::failure(
            local.error().code,
            local.error().message + ": chunk " + std::to_string(coordinate.x) + "," +
                std::to_string(coordinate.y) + "," + std::to_string(coordinate.z) +
                " physics origin " + std::to_string(config_.physics_island.block.x) + "," +
                std::to_string(config_.physics_island.block.y) + "," +
                std::to_string(config_.physics_island.block.z));
    }
    return local;
}

void ChunkCollisionSystem::refresh_stats() noexcept {
    stats_.world_revision = world_revision_;
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
