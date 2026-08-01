#include "engine/renderer/chunks/chunk_gpu_cache.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace heartstead::renderer {

bool ChunkGpuMesh::is_empty() const noexcept {
    return !vertices.is_valid() && !indices.is_valid() && vertex_count == 0 && index_count == 0 &&
           sections.empty();
}

bool ChunkGpuEntry::has_drawable_mesh() const noexcept {
    return state == ChunkGpuState::resident && mesh.vertices.is_valid() &&
           mesh.indices.is_valid() && mesh.vertex_count > 0 && mesh.index_count > 0 &&
           !mesh.sections.empty();
}

std::size_t ChunkGpuEntry::resident_bytes() const noexcept {
    return static_cast<std::size_t>(mesh.vertices.size + mesh.indices.size);
}

core::Status ChunkGpuCacheConfig::validate() const {
    constexpr auto maximum_vertex_address =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) *
        sizeof(terrain::GpuChunkVertex);
    constexpr auto maximum_index_address =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) *
        sizeof(std::uint32_t);
    if (vertex_maximum_bytes > maximum_vertex_address ||
        index_maximum_bytes > maximum_index_address) {
        return core::Status::failure(
            "chunk_gpu_cache.draw_offset_capacity_exceeded",
            "chunk GPU arena budgets exceed indexed-draw offset addressability");
    }
    if (vertex_maximum_bytes > std::numeric_limits<std::size_t>::max() ||
        index_maximum_bytes > std::numeric_limits<std::size_t>::max() ||
        vertex_maximum_bytes > std::numeric_limits<std::size_t>::max() - index_maximum_bytes) {
        return core::Status::failure("chunk_gpu_cache.combined_budget_unsupported",
                                     "combined chunk GPU arena budgets overflow size_t");
    }
    GpuBufferArenaConfig vertex;
    vertex.usage = rhi::RenderBufferUsage::vertex;
    vertex.initial_block_bytes = vertex_initial_bytes;
    vertex.maximum_capacity_bytes = vertex_maximum_bytes;
    vertex.debug_name = "chunk_vertex_arena";
    auto status = vertex.validate();
    if (!status) {
        return status;
    }
    GpuBufferArenaConfig index;
    index.usage = rhi::RenderBufferUsage::index;
    index.initial_block_bytes = index_initial_bytes;
    index.maximum_capacity_bytes = index_maximum_bytes;
    index.debug_name = "chunk_index_arena";
    return index.validate();
}

ChunkGpuCache::ChunkGpuCache(rhi::IRenderDevice& device) noexcept : device_(&device) {}

ChunkGpuCache::~ChunkGpuCache() {
    (void)shutdown();
}

core::Status ChunkGpuCache::initialize(ChunkGpuCacheConfig config) {
    if (vertex_arena_ != nullptr || index_arena_ != nullptr) {
        return core::Status::failure("chunk_gpu_cache.already_initialized",
                                     "chunk GPU cache cannot be initialized twice");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    GpuBufferArenaConfig vertex_config;
    vertex_config.usage = rhi::RenderBufferUsage::vertex;
    vertex_config.initial_block_bytes = config.vertex_initial_bytes;
    vertex_config.maximum_capacity_bytes = config.vertex_maximum_bytes;
    vertex_config.debug_name = "chunk_vertex_arena";
    auto vertices = GpuBufferArena::create(*device_, std::move(vertex_config));
    if (!vertices) {
        return core::Status::failure(vertices.error().code, vertices.error().message);
    }
    GpuBufferArenaConfig index_config;
    index_config.usage = rhi::RenderBufferUsage::index;
    index_config.initial_block_bytes = config.index_initial_bytes;
    index_config.maximum_capacity_bytes = config.index_maximum_bytes;
    index_config.debug_name = "chunk_index_arena";
    auto indices = GpuBufferArena::create(*device_, std::move(index_config));
    if (!indices) {
        return core::Status::failure(indices.error().code, indices.error().message);
    }
    vertex_arena_ = std::move(vertices).value();
    index_arena_ = std::move(indices).value();
    refresh_current_stats();
    return core::Status::ok();
}

core::Status ChunkGpuCache::shutdown() {
    core::Status first_failure = clear();
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
    refresh_current_stats();
    return first_failure;
}

core::Status ChunkGpuCache::insert(world::ChunkIdentity identity) {
    if (!identity.is_valid()) {
        return core::Status::failure("chunk_gpu_cache.invalid_identity",
                                     "chunk GPU entries require a valid load generation");
    }
    if (entries_.contains(identity)) {
        return core::Status::failure("chunk_gpu_cache.duplicate_identity",
                                     "chunk GPU identity is already cached");
    }
    if (find_by_coordinate(identity.coordinate) != nullptr) {
        return core::Status::failure(
            "chunk_gpu_cache.coordinate_generation_conflict",
            "a different load generation is still cached at the chunk coordinate");
    }

    ChunkGpuEntry entry;
    entry.identity = identity;
    entries_.emplace(identity, std::move(entry));
    refresh_current_stats();
    return core::Status::ok();
}

core::Status ChunkGpuCache::erase(world::ChunkIdentity identity) {
    const auto found = entries_.find(identity);
    if (found == entries_.end()) {
        const auto* coordinate_entry = find_by_coordinate(identity.coordinate);
        return core::Status::failure(
            coordinate_entry == nullptr ? "chunk_gpu_cache.missing_identity"
                                        : "chunk_gpu_cache.stale_load_generation",
            coordinate_entry == nullptr ? "chunk GPU identity is not cached"
                                        : "chunk GPU eviction references a stale load generation");
    }

    auto retirement = retire_entry_allocations(found->second, device_->last_submission_serial());
    entries_.erase(found);
    collect_retired();
    refresh_current_stats();
    return retirement;
}

core::Status ChunkGpuCache::release_mesh(world::ChunkIdentity identity) {
    auto* entry = find(identity);
    if (entry == nullptr) {
        return core::Status::failure("chunk_gpu_cache.missing_identity",
                                     "cannot release a mesh for an uncached chunk");
    }
    auto retirement = retire_entry_allocations(*entry, device_->last_submission_serial());
    entry->last_resident_bytes = entry->resident_bytes();
    entry->resident_content_revision = 0;
    entry->resident_render_table_revision = 0;
    entry->resident_dependency_revisions.clear();
    entry->mesh = {};
    entry->state = ChunkGpuState::missing;
    collect_retired();
    refresh_current_stats();
    return retirement;
}

core::Status ChunkGpuCache::clear() {
    core::Status first_failure = core::Status::ok();
    for (const auto& [_, entry] : entries_) {
        auto status = retire_entry_allocations(entry, device_->last_submission_serial());
        if (!status && first_failure) {
            first_failure = status;
        }
    }
    entries_.clear();
    collect_retired();
    refresh_current_stats();
    return first_failure;
}

bool ChunkGpuCache::contains(world::ChunkIdentity identity) const noexcept {
    return entries_.contains(identity);
}

const ChunkGpuEntry* ChunkGpuCache::find(world::ChunkIdentity identity) const noexcept {
    const auto found = entries_.find(identity);
    return found == entries_.end() ? nullptr : &found->second;
}

ChunkGpuEntry* ChunkGpuCache::find(world::ChunkIdentity identity) noexcept {
    const auto found = entries_.find(identity);
    return found == entries_.end() ? nullptr : &found->second;
}

const ChunkGpuEntry* ChunkGpuCache::find_by_coordinate(world::ChunkCoord coord) const noexcept {
    for (const auto& [identity, entry] : entries_) {
        if (identity.coordinate == coord) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const ChunkGpuEntry*> ChunkGpuCache::entries() const {
    std::vector<const ChunkGpuEntry*> result;
    result.reserve(entries_.size());
    for (const auto& [_, entry] : entries_) {
        result.push_back(&entry);
    }
    return result;
}

void ChunkGpuCache::mark_cpu_mesh_ready(world::ChunkIdentity identity) noexcept {
    auto* entry = find(identity);
    if (entry != nullptr && entry->state != ChunkGpuState::resident) {
        entry->state = ChunkGpuState::cpu_mesh_ready;
    }
}

void ChunkGpuCache::mark_upload_pending(world::ChunkIdentity identity) noexcept {
    auto* entry = find(identity);
    if (entry != nullptr && entry->state != ChunkGpuState::resident) {
        entry->state = ChunkGpuState::upload_pending;
    }
}

void ChunkGpuCache::mark_failed(world::ChunkIdentity identity) noexcept {
    auto* entry = find(identity);
    if (entry != nullptr && entry->state != ChunkGpuState::resident) {
        entry->state = ChunkGpuState::failed;
    }
}

void ChunkGpuCache::mark_visible(world::ChunkIdentity identity, std::uint64_t epoch) noexcept {
    auto* entry = find(identity);
    if (entry != nullptr) {
        entry->last_visible_epoch = epoch;
    }
}

core::Result<ChunkGpuUploadResult> ChunkGpuCache::replace_mesh(
    world::ChunkIdentity identity, std::uint64_t content_revision, math::Bounds3f local_bounds,
    std::span<const terrain::GpuChunkVertex> vertices, std::span<const std::uint32_t> indices,
    std::uint64_t render_table_revision,
    std::span<const world::ChunkDependencyRevision> dependency_revisions,
    std::span<const world::ChunkMeshSection> sections) {
    const std::array<ChunkGpuMeshUpload, 1> uploads{
        ChunkGpuMeshUpload{identity, content_revision, render_table_revision, local_bounds,
                           vertices, indices, dependency_revisions, sections}};
    auto batch = replace_meshes(uploads);
    if (!batch) {
        return core::Result<ChunkGpuUploadResult>::failure(batch.error().code,
                                                           batch.error().message);
    }
    return core::Result<ChunkGpuUploadResult>::success(batch.value().uploads.front());
}

core::Result<ChunkGpuBatchUploadResult>
ChunkGpuCache::replace_meshes(std::span<const ChunkGpuMeshUpload> uploads) {
    if (uploads.empty()) {
        return core::Result<ChunkGpuBatchUploadResult>::failure(
            "chunk_gpu_cache.empty_upload_batch", "chunk GPU upload batch must not be empty");
    }
    if (vertex_arena_ == nullptr || index_arena_ == nullptr) {
        auto status = initialize();
        if (!status) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(status.error().code,
                                                                    status.error().message);
        }
    }

    std::set<world::ChunkIdentity> identities;
    for (const auto& upload : uploads) {
        if (find(upload.identity) == nullptr) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.missing_identity", "cannot upload a mesh for an uncached chunk");
        }
        if (!identities.insert(upload.identity).second) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.duplicate_batch_identity",
                "chunk GPU upload batch contains an identity more than once");
        }
        if (upload.content_revision == 0 || upload.render_table_revision == 0) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.invalid_revision",
                "chunk GPU uploads require nonzero content and render-table revisions");
        }
        if (upload.vertices.empty() != upload.indices.empty()) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.incomplete_mesh",
                "chunk GPU meshes must provide both vertex and index data or neither");
        }
        if (upload.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            upload.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.mesh_too_large",
                "chunk GPU mesh counts exceed uint32 draw limits");
        }
        if (std::ranges::any_of(upload.indices, [&upload](std::uint32_t value) {
                return value >= upload.vertices.size();
            })) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.index_out_of_bounds",
                "chunk GPU mesh contains an index outside its vertex range");
        }
        std::size_t covered_indices = 0;
        for (const auto& section : upload.sections) {
            const auto phase = static_cast<std::uint8_t>(section.render_phase);
            if (section.material_index == 0 ||
                phase > static_cast<std::uint8_t>(world::MeshingRenderPhase::fluid) ||
                section.index_count == 0 || section.index_count % 3U != 0 ||
                section.first_index != covered_indices ||
                section.index_count > upload.indices.size() - covered_indices) {
                return core::Result<ChunkGpuBatchUploadResult>::failure(
                    "chunk_gpu_cache.invalid_mesh_section",
                    "chunk GPU mesh section material, phase, or index range is invalid");
            }
            covered_indices += section.index_count;
        }
        if (!upload.sections.empty() && covered_indices != upload.indices.size()) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.incomplete_mesh_sections",
                "chunk GPU mesh sections do not cover the complete index buffer");
        }
        if (upload.indices.empty() && !upload.sections.empty()) {
            return core::Result<ChunkGpuBatchUploadResult>::failure(
                "chunk_gpu_cache.sections_without_geometry",
                "empty chunk GPU meshes cannot contain draw sections");
        }
    }

    struct PreparedUpload {
        ChunkGpuMesh mesh;
        std::vector<std::byte> compact_index_bytes;
        std::size_t uploaded_bytes = 0;
    };
    std::vector<PreparedUpload> prepared;
    prepared.resize(uploads.size());
    const auto rollback = [this, &prepared]() {
        for (const auto& candidate : prepared) {
            if (candidate.mesh.vertices.is_valid()) {
                (void)vertex_arena_->retire(candidate.mesh.vertices, 0);
            }
            if (candidate.mesh.indices.is_valid()) {
                (void)index_arena_->retire(candidate.mesh.indices, 0);
            }
        }
        vertex_arena_->collect(0);
        index_arena_->collect(0);
        refresh_current_stats();
    };

    std::vector<rhi::RenderBufferWrite> writes;
    writes.reserve(uploads.size() * 2);
    for (std::size_t index = 0; index < uploads.size(); ++index) {
        const auto& upload = uploads[index];
        auto& candidate = prepared[index];
        candidate.mesh.vertex_count = static_cast<std::uint32_t>(upload.vertices.size());
        candidate.mesh.index_count = static_cast<std::uint32_t>(upload.indices.size());
        if (upload.vertices.empty()) {
            continue;
        }
        if (upload.sections.empty()) {
            const auto material = std::max<std::uint16_t>(upload.vertices.front().material, 1U);
            candidate.mesh.sections.push_back(
                {material, world::MeshingRenderPhase::opaque, 0, candidate.mesh.index_count});
        } else {
            candidate.mesh.sections.assign(upload.sections.begin(), upload.sections.end());
        }
        const auto vertex_bytes = std::as_bytes(upload.vertices);
        std::span<const std::byte> index_bytes;
        const bool uses_uint16 =
            upload.vertices.size() <=
                static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1U &&
            std::ranges::all_of(upload.indices, [](std::uint32_t value) {
                return value <= std::numeric_limits<std::uint16_t>::max();
            });
        if (uses_uint16) {
            candidate.mesh.index_type = rhi::RenderIndexType::uint16;
            const auto raw_bytes = upload.indices.size() * sizeof(std::uint16_t);
            const auto padded_bytes = (raw_bytes + 3U) & ~std::size_t{3U};
            candidate.compact_index_bytes.resize(padded_bytes);
            for (std::size_t element = 0; element < upload.indices.size(); ++element) {
                const auto compact = static_cast<std::uint16_t>(upload.indices[element]);
                std::memcpy(candidate.compact_index_bytes.data() + element * sizeof(std::uint16_t),
                            &compact, sizeof(compact));
            }
            index_bytes = candidate.compact_index_bytes;
        } else {
            candidate.mesh.index_type = rhi::RenderIndexType::uint32;
            index_bytes = std::as_bytes(upload.indices);
        }
        auto vertices =
            vertex_arena_->allocate(vertex_bytes.size(), sizeof(terrain::GpuChunkVertex));
        if (!vertices) {
            rollback();
            ++stats_.failed_upload_count;
            return core::Result<ChunkGpuBatchUploadResult>::failure(vertices.error().code,
                                                                    vertices.error().message);
        }
        candidate.mesh.vertices = vertices.value();
        auto indices = index_arena_->allocate(index_bytes.size(), 4);
        if (!indices) {
            rollback();
            ++stats_.failed_upload_count;
            return core::Result<ChunkGpuBatchUploadResult>::failure(indices.error().code,
                                                                    indices.error().message);
        }
        candidate.mesh.indices = indices.value();
        candidate.uploaded_bytes = vertex_bytes.size() + index_bytes.size();
        writes.push_back({candidate.mesh.vertices.buffer,
                          static_cast<std::size_t>(candidate.mesh.vertices.offset), vertex_bytes});
        writes.push_back({candidate.mesh.indices.buffer,
                          static_cast<std::size_t>(candidate.mesh.indices.offset), index_bytes});
    }

    double cpu_gpu_wait_ms = 0.0;
    if (!writes.empty()) {
        auto written = device_->upload_buffer_batch(writes);
        if (!written) {
            rollback();
            ++stats_.failed_upload_count;
            return core::Result<ChunkGpuBatchUploadResult>::failure(written.error().code,
                                                                    written.error().message);
        }
        cpu_gpu_wait_ms = written.value().cpu_gpu_wait_ms;
    }

    ChunkGpuBatchUploadResult result;
    result.uploads.reserve(uploads.size());
    result.write_count = writes.size();
    result.cpu_gpu_wait_ms = cpu_gpu_wait_ms;
    core::Status retirement_status = core::Status::ok();
    for (std::size_t index = 0; index < uploads.size(); ++index) {
        const auto& upload = uploads[index];
        auto* entry = find(upload.identity);
        const auto old_entry = *entry;
        entry->resident_content_revision = upload.content_revision;
        entry->resident_render_table_revision = upload.render_table_revision;
        entry->resident_dependency_revisions.assign(upload.dependency_revisions.begin(),
                                                    upload.dependency_revisions.end());
        entry->mesh = prepared[index].mesh;
        entry->last_resident_bytes = prepared[index].uploaded_bytes;
        entry->residency_suppressed = false;
        entry->local_bounds = upload.local_bounds;
        entry->state = ChunkGpuState::resident;

        auto status = retire_entry_allocations(old_entry, device_->last_submission_serial());
        if (!status && retirement_status) {
            retirement_status = status;
        }
        ChunkGpuUploadResult upload_result;
        upload_result.uploaded_bytes = prepared[index].uploaded_bytes;
        upload_result.empty = upload.vertices.empty();
        upload_result.replaced_resident_mesh = old_entry.state == ChunkGpuState::resident;
        result.uploaded_bytes += upload_result.uploaded_bytes;
        result.uploads.push_back(upload_result);
    }
    collect_retired();
    stats_.uploaded_chunk_count += uploads.size();
    stats_.uploaded_bytes += result.uploaded_bytes;
    ++stats_.upload_batch_count;
    stats_.last_upload_chunk_count = uploads.size();
    stats_.last_upload_write_count = result.write_count;
    refresh_current_stats();
    if (!retirement_status) {
        return core::Result<ChunkGpuBatchUploadResult>::failure(retirement_status.error().code,
                                                                retirement_status.error().message);
    }
    return core::Result<ChunkGpuBatchUploadResult>::success(std::move(result));
}

ChunkGpuCacheStats ChunkGpuCache::stats() const noexcept {
    return stats_;
}

void ChunkGpuCache::reset_session_stats() noexcept {
    stats_ = {};
    refresh_current_stats();
}

core::Status ChunkGpuCache::retire_entry_allocations(const ChunkGpuEntry& entry,
                                                     std::uint64_t submission_serial) {
    core::Status first_failure = core::Status::ok();
    if (entry.mesh.vertices.is_valid() && vertex_arena_ != nullptr) {
        auto status = vertex_arena_->retire(entry.mesh.vertices, submission_serial);
        if (!status) {
            first_failure = status;
        }
    }
    if (entry.mesh.indices.is_valid() && index_arena_ != nullptr) {
        auto status = index_arena_->retire(entry.mesh.indices, submission_serial);
        if (!status && first_failure) {
            first_failure = status;
        }
    }
    return first_failure;
}

void ChunkGpuCache::collect_retired() noexcept {
    if (vertex_arena_ != nullptr) {
        vertex_arena_->collect(device_->completed_submission_serial());
    }
    if (index_arena_ != nullptr) {
        index_arena_->collect(device_->completed_submission_serial());
    }
}

void ChunkGpuCache::refresh_current_stats() noexcept {
    stats_.entry_count = entries_.size();
    stats_.resident_chunk_count = 0;
    stats_.empty_chunk_count = 0;
    stats_.resident_allocation_count = 0;
    stats_.resident_bytes = 0;
    stats_.uint16_index_chunk_count = 0;
    stats_.uint32_index_chunk_count = 0;
    stats_.resident_section_count = 0;
    for (const auto& [_, entry] : entries_) {
        if (entry.state != ChunkGpuState::resident) {
            continue;
        }
        ++stats_.resident_chunk_count;
        stats_.resident_bytes += entry.resident_bytes();
        stats_.resident_allocation_count +=
            static_cast<std::size_t>(entry.mesh.vertices.is_valid()) +
            static_cast<std::size_t>(entry.mesh.indices.is_valid());
        if (!entry.has_drawable_mesh()) {
            ++stats_.empty_chunk_count;
        } else if (entry.mesh.index_type == rhi::RenderIndexType::uint16) {
            ++stats_.uint16_index_chunk_count;
        } else {
            ++stats_.uint32_index_chunk_count;
        }
        stats_.resident_section_count += entry.mesh.sections.size();
    }
    stats_.vertex_arena = vertex_arena_ == nullptr ? GpuBufferArenaStats{} : vertex_arena_->stats();
    stats_.index_arena = index_arena_ == nullptr ? GpuBufferArenaStats{} : index_arena_->stats();
    stats_.resident_buffer_count =
        stats_.vertex_arena.buffer_count + stats_.index_arena.buffer_count;
}

} // namespace heartstead::renderer
