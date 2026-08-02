#include "engine/renderer/terrain/far_terrain_renderer.hpp"

#include "engine/profiling/profiler.hpp"
#include "engine/renderer/camera/frustum.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <map>
#include <ranges>
#include <set>

namespace heartstead::renderer {

namespace {

[[nodiscard]] std::uint32_t patch_seed(const FarTerrainPatchKey& key) noexcept {
    auto value = static_cast<std::uint64_t>(key.x) ^
                 (static_cast<std::uint64_t>(key.z) * 0x9e3779b97f4a7c15ULL) ^
                 (static_cast<std::uint64_t>(key.level) << 48U);
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    return static_cast<std::uint32_t>(value ^ (value >> 32U));
}

} // namespace

FarTerrainRenderer::FarTerrainRenderer(rhi::IRenderDevice& device) noexcept : device_(&device) {}

FarTerrainRenderer::~FarTerrainRenderer() {
    static_cast<void>(shutdown());
}

core::Status FarTerrainRenderer::initialize(FarTerrainRendererConfig config,
                                            rhi::RenderResourceHandle pipeline) {
    if (!pipeline.is_valid() || config.maximum_patch_builds_per_frame == 0 ||
        config.maximum_upload_bytes_per_frame == 0 || config.maximum_resident_bytes == 0 ||
        config.maximum_replacement_headroom_bytes == 0 ||
        config.maximum_replacement_headroom_bytes >
            std::numeric_limits<std::size_t>::max() - config.maximum_resident_bytes) {
        return core::Status::failure("renderer.invalid_far_terrain_renderer",
                                     "far terrain requires a pipeline and positive budgets");
    }
    auto clipmap = FarTerrainClipmap::create(config.clipmap);
    if (!clipmap) {
        return core::Status::failure(clipmap.error().code, clipmap.error().message);
    }
    auto lod_updates = FarTerrainLodUpdateGraph::create(
        config.lod_updates, config.clipmap.level_count, config.maximum_patch_builds_per_frame);
    if (!lod_updates) {
        return core::Status::failure(lod_updates.error().code, lod_updates.error().message);
    }
    const auto allocation_budget =
        config.maximum_resident_bytes + config.maximum_replacement_headroom_bytes;
    const auto vertex_budget =
        std::max<std::size_t>(allocation_budget - allocation_budget / 3U, 1U * 1024U * 1024U);
    const auto index_budget = std::max<std::size_t>(
        allocation_budget - std::min(allocation_budget, vertex_budget), 1U * 1024U * 1024U);
    auto vertex_arena =
        GpuBufferArena::create(*device_, {rhi::RenderBufferUsage::vertex,
                                          std::min<std::size_t>(16U * 1024U * 1024U, vertex_budget),
                                          vertex_budget, "far_terrain_vertex_arena"});
    if (!vertex_arena) {
        return core::Status::failure(vertex_arena.error().code, vertex_arena.error().message);
    }
    auto index_arena =
        GpuBufferArena::create(*device_, {rhi::RenderBufferUsage::index,
                                          std::min<std::size_t>(8U * 1024U * 1024U, index_budget),
                                          index_budget, "far_terrain_index_arena"});
    if (!index_arena) {
        static_cast<void>(vertex_arena.value()->shutdown());
        return core::Status::failure(index_arena.error().code, index_arena.error().message);
    }
    maximum_draw_count_ = static_cast<std::size_t>(config.clipmap.level_count) *
                          config.clipmap.patches_per_axis * config.clipmap.patches_per_axis;
    const auto indirect_bytes = maximum_draw_count_ * sizeof(rhi::RenderIndexedIndirectCommand);
    const auto draw_data_bytes = maximum_draw_count_ * sizeof(FarTerrainDrawData);
    std::array<rhi::RenderResourceHandle, 4> indirect_buffers{};
    std::array<rhi::RenderResourceHandle, 4> draw_data_buffers{};
    for (std::size_t frame = 0; frame < indirect_buffers.size(); ++frame) {
        auto indirect = device_->create_buffer({rhi::RenderBufferUsage::indirect, indirect_bytes,
                                                "far_terrain_indirect_" + std::to_string(frame),
                                                rhi::RenderBufferMemory::device_local});
        if (!indirect) {
            for (const auto handle : indirect_buffers) {
                if (handle.is_valid()) {
                    static_cast<void>(device_->release_resource(handle));
                }
            }
            static_cast<void>(index_arena.value()->shutdown());
            static_cast<void>(vertex_arena.value()->shutdown());
            return core::Status::failure(indirect.error().code, indirect.error().message);
        }
        indirect_buffers[frame] = indirect.value().handle;
        auto draw_data = device_->create_buffer({rhi::RenderBufferUsage::storage, draw_data_bytes,
                                                 "far_terrain_draw_data_" + std::to_string(frame),
                                                 rhi::RenderBufferMemory::device_local});
        if (!draw_data) {
            static_cast<void>(device_->release_resource(indirect_buffers[frame]));
            indirect_buffers[frame] = {};
            for (const auto handle : indirect_buffers) {
                if (handle.is_valid()) {
                    static_cast<void>(device_->release_resource(handle));
                }
            }
            for (const auto handle : draw_data_buffers) {
                if (handle.is_valid()) {
                    static_cast<void>(device_->release_resource(handle));
                }
            }
            static_cast<void>(index_arena.value()->shutdown());
            static_cast<void>(vertex_arena.value()->shutdown());
            return core::Status::failure(draw_data.error().code, draw_data.error().message);
        }
        draw_data_buffers[frame] = draw_data.value().handle;
    }
    config_ = std::move(config);
    clipmap_ = std::move(clipmap.value());
    lod_updates_ = std::move(lod_updates.value());
    pipeline_ = pipeline;
    vertex_arena_ = std::move(vertex_arena.value());
    index_arena_ = std::move(index_arena.value());
    indirect_buffers_ = indirect_buffers;
    draw_data_buffers_ = draw_data_buffers;
    const auto material = core::PrototypeId::parse("base:materials/far_terrain");
    if (!material) {
        static_cast<void>(shutdown());
        return core::Status::failure("renderer.invalid_far_terrain_material",
                                     "far terrain material id is invalid");
    }
    const std::array descriptor_writes{
        rhi::RenderDescriptorWrite{*material, "far_patch_draws", draw_data_buffers_.front(), 0,
                                   draw_data_bytes},
    };
    auto descriptor_status = device_->write_descriptors(descriptor_writes);
    if (!descriptor_status) {
        const auto error = descriptor_status.error();
        static_cast<void>(shutdown());
        return core::Status::failure(error.code, error.message);
    }
    return core::Status::ok();
}

core::Status FarTerrainRenderer::set_pipeline(rhi::RenderResourceHandle pipeline) noexcept {
    if (!pipeline.is_valid()) {
        return core::Status::failure("far_terrain.invalid_pipeline",
                                     "far terrain pipeline must be valid");
    }
    pipeline_ = pipeline;
    return core::Status::ok();
}

core::Status FarTerrainRenderer::update(math::Vec3d camera_world,
                                        const FarTerrainSurfaceSampler& sampler,
                                        std::uint64_t surface_revision,
                                        std::span<const math::Bounds3d> invalidated_regions) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("renderer.far_terrain.update");
    if (!clipmap_.has_value() || !sampler) {
        return core::Status::failure("renderer.far_terrain_uninitialized",
                                     "far terrain must be initialized with a surface sampler");
    }
    stats_.built_patches = 0;
    stats_.replaced_patches = 0;
    stats_.rebuilt_mid_patches = 0;
    stats_.rebuilt_far_patches = 0;
    stats_.upload_deferred_patches = 0;
    stats_.evicted_patches = 0;
    stats_.uploaded_bytes = 0;
    vertex_arena_->collect(device_->completed_submission_serial());
    index_arena_->collect(device_->completed_submission_serial());
    plan_ = clipmap_->plan(camera_world);
    stats_.planned_patches = plan_.patches.size();
    auto graph_status = lod_updates_->synchronize(plan_, surface_revision, invalidated_regions);
    if (!graph_status) {
        return graph_status;
    }

    std::set<FarTerrainPatchKey> desired;
    for (const auto& patch : plan_.patches) {
        desired.insert(patch.key);
        const auto resident = resident_.find(patch.key);
        if (resident != resident_.end()) {
            resident->second.patch.streaming_priority = patch.streaming_priority;
        }
    }
    for (auto iterator = resident_.begin(); iterator != resident_.end();) {
        if (desired.contains(iterator->first)) {
            ++iterator;
        } else {
            auto removed = iterator++;
            release_patch(removed);
        }
    }

    auto updates = lod_updates_->schedule_updates();
    const auto retry_from = [this, &updates](std::size_t first) {
        auto status = core::Status::ok();
        for (auto index = first; index < updates.size(); ++index) {
            if (!lod_updates_->accepts_result(updates[index].patch.key,
                                              updates[index].request_revision)) {
                continue;
            }
            auto retry =
                lod_updates_->retry(updates[index].patch.key, updates[index].request_revision);
            if (!retry && status) {
                status = retry;
            }
        }
        return status;
    };
    for (std::size_t index = 0; index < updates.size(); ++index) {
        auto uploaded = upload_patch(updates[index], sampler);
        if (!uploaded) {
            const auto upload_error = uploaded.error();
            static_cast<void>(retry_from(index));
            return core::Status::failure(upload_error.code, upload_error.message);
        }
        if (!uploaded.value()) {
            ++stats_.upload_deferred_patches;
            auto retry = retry_from(index);
            if (!retry) {
                return retry;
            }
            break;
        }
        auto published =
            lod_updates_->publish(updates[index].patch.key, updates[index].request_revision);
        if (!published) {
            static_cast<void>(retry_from(index + 1U));
            return published;
        }
        if (updates[index].replaces_resident_patch) {
            ++stats_.replaced_patches;
            if (updates[index].band == FarTerrainLodBand::mid) {
                ++stats_.rebuilt_mid_patches;
            } else {
                ++stats_.rebuilt_far_patches;
            }
        }
    }
    auto budget_status = enforce_resident_budget();
    if (!budget_status) {
        return budget_status;
    }
    refresh_resident_stats();
    const auto& lod = lod_updates_->stats();
    stats_.stale_resident_patches = lod.stale_resident_patches;
    stats_.pending_mid_updates = lod.pending_mid_updates;
    stats_.pending_far_updates = lod.pending_far_updates;
    stats_.in_flight_updates = lod.in_flight_updates;
    stats_.maximum_pending_frames = lod.maximum_pending_frames;
    stats_.total_invalidated_patches = lod.total_invalidated_patches;
    stats_.total_coalesced_invalidations = lod.total_coalesced_invalidations;
    stats_.total_published_updates = lod.total_published_updates;
    stats_.total_stale_results = lod.total_stale_results;
    stats_.total_retried_updates = lod.total_retried_updates;
    stats_.pending_patches = lod.pending_mid_updates + lod.pending_far_updates;
    HEARTSTEAD_PROFILE_PLOT("far_terrain.pending_mid", stats_.pending_mid_updates);
    HEARTSTEAD_PROFILE_PLOT("far_terrain.pending_far", stats_.pending_far_updates);
    HEARTSTEAD_PROFILE_PLOT("far_terrain.stale_resident", stats_.stale_resident_patches);
    HEARTSTEAD_PROFILE_PLOT("far_terrain.maximum_pending_frames", stats_.maximum_pending_frames);
    return core::Status::ok();
}

core::Result<bool> FarTerrainRenderer::upload_patch(const FarTerrainLodUpdateRequest& request,
                                                    const FarTerrainSurfaceSampler& sampler) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("renderer.far_terrain.patch_build_upload");
    auto mesh = clipmap_->build_patch_mesh(request.patch, sampler);
    if (!mesh) {
        return core::Result<bool>::failure(mesh.error().code, mesh.error().message);
    }
    if (mesh.value().indices.empty()) {
        install_patch(ResidentPatch{
            request.patch, mesh.value().world_origin, mesh.value().local_bounds, {}, {}, 0, 0});
        ++stats_.built_patches;
        return core::Result<bool>::success(true);
    }
    std::vector<FarTerrainGpuVertex> vertices;
    vertices.reserve(mesh.value().vertices.size());
    for (const auto& vertex : mesh.value().vertices) {
        vertices.push_back({vertex.local_position, vertex.normal, vertex.uv, vertex.material, 0,
                            vertex.transition, 0.0F});
    }
    const auto vertex_bytes = std::as_bytes(std::span{vertices});
    const auto index_bytes = std::as_bytes(std::span{mesh.value().indices});
    const auto byte_size = vertex_bytes.size() + index_bytes.size();
    if (stats_.uploaded_bytes > 0 &&
        stats_.uploaded_bytes + byte_size > config_.maximum_upload_bytes_per_frame) {
        return core::Result<bool>::success(false);
    }
    auto vertex = vertex_arena_->allocate(vertex_bytes.size(), 4U);
    if (!vertex) {
        return core::Result<bool>::failure(vertex.error().code, vertex.error().message);
    }
    auto index = index_arena_->allocate(index_bytes.size(), 4U);
    if (!index) {
        static_cast<void>(vertex_arena_->retire(vertex.value(), device_->last_submission_serial()));
        return core::Result<bool>::failure(index.error().code, index.error().message);
    }
    const std::array writes{
        rhi::RenderBufferWrite{vertex.value().buffer,
                               static_cast<std::size_t>(vertex.value().offset), vertex_bytes},
        rhi::RenderBufferWrite{index.value().buffer, static_cast<std::size_t>(index.value().offset),
                               index_bytes},
    };
    auto uploaded = device_->upload_buffer_batch(writes);
    if (!uploaded) {
        static_cast<void>(vertex_arena_->retire(vertex.value(), device_->last_submission_serial()));
        static_cast<void>(index_arena_->retire(index.value(), device_->last_submission_serial()));
        return core::Result<bool>::failure(uploaded.error().code, uploaded.error().message);
    }
    install_patch(ResidentPatch{
        request.patch, mesh.value().world_origin, mesh.value().local_bounds, vertex.value(),
        index.value(), static_cast<std::uint32_t>(mesh.value().indices.size()), byte_size});
    stats_.uploaded_bytes += byte_size;
    ++stats_.built_patches;
    return core::Result<bool>::success(true);
}

void FarTerrainRenderer::install_patch(ResidentPatch patch) {
    const auto found = resident_.find(patch.patch.key);
    if (found == resident_.end()) {
        resident_.emplace(patch.patch.key, std::move(patch));
        return;
    }
    auto previous = std::move(found->second);
    found->second = std::move(patch);
    retire_patch_allocations(previous);
}

void FarTerrainRenderer::retire_patch_allocations(const ResidentPatch& patch) noexcept {
    if (patch.vertex_allocation.is_valid()) {
        static_cast<void>(
            vertex_arena_->retire(patch.vertex_allocation, device_->last_submission_serial()));
    }
    if (patch.index_allocation.is_valid()) {
        static_cast<void>(
            index_arena_->retire(patch.index_allocation, device_->last_submission_serial()));
    }
}

void FarTerrainRenderer::release_patch(
    std::map<FarTerrainPatchKey, ResidentPatch>::iterator iterator) {
    retire_patch_allocations(iterator->second);
    resident_.erase(iterator);
    ++stats_.evicted_patches;
}

core::Status FarTerrainRenderer::enforce_resident_budget() {
    refresh_resident_stats();
    while (stats_.resident_bytes > config_.maximum_resident_bytes && !resident_.empty()) {
        const auto candidate = std::ranges::min_element(resident_, [](const auto& left,
                                                                      const auto& right) {
            if (left.second.patch.streaming_priority != right.second.patch.streaming_priority) {
                return left.second.patch.streaming_priority < right.second.patch.streaming_priority;
            }
            return left.first < right.first;
        });
        auto evicted = lod_updates_->evict_resident(candidate->first);
        if (!evicted) {
            return evicted;
        }
        release_patch(candidate);
        refresh_resident_stats();
    }
    return core::Status::ok();
}

void FarTerrainRenderer::refresh_resident_stats() noexcept {
    stats_.resident_patches = resident_.size();
    stats_.resident_bytes = 0;
    for (const auto& [key, patch] : resident_) {
        static_cast<void>(key);
        stats_.resident_bytes += patch.resident_bytes;
    }
}

std::vector<rhi::RenderDrawCommand>
FarTerrainRenderer::build_draws(const RenderCamera& camera,
                                std::vector<rhi::RenderDrawCommand> reusable) {
    reusable.clear();
    stats_.visible_triangle_count = 0;
    const auto frustum = RenderFrustum::from_view_projection(camera.view_projection);
    struct VisiblePatch {
        const ResidentPatch* patch = nullptr;
        math::Vec3f origin{};
        std::uint32_t seed = 0;
    };
    std::vector<VisiblePatch> visible;
    visible.reserve(resident_.size());
    for (const auto& [key, patch] : resident_) {
        if (patch.index_count == 0) {
            continue;
        }
        auto world_origin = world::WorldPosition::from_legacy_global(patch.world_origin);
        if (!world_origin) {
            continue;
        }
        auto origin = world::to_camera_relative(world_origin.value(), camera.floating_origin);
        if (!origin) {
            continue;
        }
        const math::Bounds3f bounds{patch.local_bounds.min + origin.value(),
                                    patch.local_bounds.max + origin.value()};
        if (!frustum.intersects(bounds)) {
            continue;
        }
        visible.push_back({&patch, origin.value(), patch_seed(key)});
        stats_.visible_triangle_count += patch.index_count / 3U;
    }
    const auto append_direct = [&]() {
        for (const auto& item : visible) {
            rhi::RenderDrawCommand draw;
            draw.pipeline = pipeline_;
            draw.vertex_buffer = item.patch->vertex_allocation.buffer;
            draw.index_buffer = item.patch->index_allocation.buffer;
            draw.index_count = item.patch->index_count;
            draw.first_index = static_cast<std::uint32_t>(item.patch->index_allocation.offset /
                                                          sizeof(std::uint32_t));
            draw.vertex_offset = static_cast<std::int32_t>(item.patch->vertex_allocation.offset /
                                                           sizeof(FarTerrainGpuVertex));
            draw.instance_count = 1;
            draw.index_type = rhi::RenderIndexType::uint32;
            draw.camera_relative_origin = item.origin;
            draw.texture_variation_seed = item.seed;
            reusable.push_back(draw);
        }
    };
    if (!visible.empty() && device_->capabilities().supports_multi_draw_indirect) {
        using BufferPair = std::pair<rhi::RenderResourceHandle, rhi::RenderResourceHandle>;
        std::map<BufferPair, std::vector<const VisiblePatch*>> groups;
        for (const auto& item : visible) {
            groups[{item.patch->vertex_allocation.buffer, item.patch->index_allocation.buffer}]
                .push_back(&item);
        }
        std::vector<rhi::RenderIndexedIndirectCommand> commands;
        std::vector<FarTerrainDrawData> draw_data;
        commands.reserve(visible.size());
        draw_data.reserve(visible.size());
        struct GroupRange {
            BufferPair buffers;
            std::size_t first_command = 0;
            std::size_t command_count = 0;
        };
        std::vector<GroupRange> ranges;
        for (const auto& [buffers, items] : groups) {
            GroupRange range{buffers, commands.size(), items.size()};
            for (const auto* item : items) {
                const auto data_index = static_cast<std::uint32_t>(draw_data.size());
                commands.push_back(
                    {item->patch->index_count, 1U,
                     static_cast<std::uint32_t>(item->patch->index_allocation.offset /
                                                sizeof(std::uint32_t)),
                     static_cast<std::int32_t>(item->patch->vertex_allocation.offset /
                                               sizeof(FarTerrainGpuVertex)),
                     data_index});
                draw_data.push_back({item->origin, item->seed});
            }
            ranges.push_back(range);
        }
        frame_buffer_index_ = (frame_buffer_index_ + 1U) % indirect_buffers_.size();
        const auto command_bytes = std::as_bytes(std::span{commands});
        const auto draw_data_bytes = std::as_bytes(std::span{draw_data});
        const std::array writes{
            rhi::RenderBufferWrite{indirect_buffers_[frame_buffer_index_], 0, command_bytes},
            rhi::RenderBufferWrite{draw_data_buffers_[frame_buffer_index_], 0, draw_data_bytes},
        };
        auto uploaded = device_->upload_buffer_batch(writes);
        const auto material = core::PrototypeId::parse("base:materials/far_terrain");
        core::Result<rhi::RenderDescriptorWriteStats> bound =
            core::Result<rhi::RenderDescriptorWriteStats>::failure(
                "renderer.invalid_far_terrain_material", "far terrain material id is invalid");
        if (uploaded && material) {
            const std::array descriptor_writes{
                rhi::RenderDescriptorWrite{*material, "far_patch_draws",
                                           draw_data_buffers_[frame_buffer_index_], 0,
                                           draw_data_bytes.size()},
            };
            bound = device_->write_descriptors(descriptor_writes);
        }
        if (uploaded && bound) {
            constexpr std::uint32_t indirect_marker = 0x7fc00001U;
            for (const auto& range : ranges) {
                rhi::RenderDrawCommand draw;
                draw.pipeline = pipeline_;
                draw.vertex_buffer = range.buffers.first;
                draw.index_buffer = range.buffers.second;
                draw.index_count = commands[range.first_command].index_count;
                draw.instance_count = 1;
                draw.index_type = rhi::RenderIndexType::uint32;
                draw.texture_variation_seed = indirect_marker;
                draw.indirect.command_buffer = indirect_buffers_[frame_buffer_index_];
                draw.indirect.command_offset =
                    range.first_command * sizeof(rhi::RenderIndexedIndirectCommand);
                draw.indirect.maximum_draw_count = static_cast<std::uint32_t>(range.command_count);
                reusable.push_back(draw);
            }
        } else {
            append_direct();
        }
    } else {
        append_direct();
    }
    stats_.visible_patches = visible.size();
    stats_.draw_count = reusable.size();
    return reusable;
}

core::Status FarTerrainRenderer::clear() {
    core::Status first_failure = core::Status::ok();
    for (auto iterator = resident_.begin(); iterator != resident_.end();) {
        auto removed = iterator++;
        release_patch(removed);
    }
    if (vertex_arena_ != nullptr) {
        vertex_arena_->collect(std::numeric_limits<std::uint64_t>::max());
    }
    if (index_arena_ != nullptr) {
        index_arena_->collect(std::numeric_limits<std::uint64_t>::max());
    }
    resident_.clear();
    plan_ = {};
    if (lod_updates_.has_value()) {
        lod_updates_->clear();
    }
    stats_ = {};
    return first_failure;
}

core::Status FarTerrainRenderer::shutdown() {
    core::Status first_failure = clear();
    for (const auto handle : indirect_buffers_) {
        if (handle.is_valid()) {
            auto status = device_->release_resource(handle);
            if (!status && first_failure) {
                first_failure = status;
            }
        }
    }
    for (const auto handle : draw_data_buffers_) {
        if (handle.is_valid()) {
            auto status = device_->release_resource(handle);
            if (!status && first_failure) {
                first_failure = status;
            }
        }
    }
    if (vertex_arena_ != nullptr) {
        auto status = vertex_arena_->shutdown();
        if (!status && first_failure) {
            first_failure = status;
        }
        vertex_arena_.reset();
    }
    if (index_arena_ != nullptr) {
        auto status = index_arena_->shutdown();
        if (!status && first_failure) {
            first_failure = status;
        }
        index_arena_.reset();
    }
    indirect_buffers_ = {};
    draw_data_buffers_ = {};
    clipmap_.reset();
    lod_updates_.reset();
    pipeline_ = {};
    stats_ = {};
    return first_failure;
}

const FarTerrainRendererStats& FarTerrainRenderer::stats() const noexcept {
    return stats_;
}

} // namespace heartstead::renderer
