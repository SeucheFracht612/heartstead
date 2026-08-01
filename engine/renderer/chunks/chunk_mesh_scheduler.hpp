#pragma once

#include "engine/core/result.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/meshing/chunk_mesher.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::renderer {

struct MeshPriority {
    bool visible = false;
    bool missing_resident_mesh = false;
    float distance_squared = 0.0F;
    std::uint64_t sequence = 0;
};

struct ChunkMeshRequest {
    world::ChunkIdentity identity{};
    world::ChunkStageTicket stage_ticket{};
    std::uint64_t center_revision = 0;
    std::uint64_t block_render_table_revision = 0;
    world::ChunkNeighborhoodSnapshot neighborhood;
    std::shared_ptr<const world::BlockRenderTableSnapshot> render_table;
    MeshPriority priority{};
};

enum class ChunkMeshResultState {
    succeeded,
    failed,
    cancelled,
};

struct ChunkMeshResult {
    world::ChunkIdentity identity{};
    world::ChunkStageTicket stage_ticket{};
    std::uint64_t center_revision = 0;
    std::uint64_t block_render_table_revision = 0;
    std::vector<world::ChunkDependencyRevision> dependency_revisions;
    MeshPriority priority{};
    ChunkMeshResultState state = ChunkMeshResultState::failed;
    std::optional<world::ChunkMesh> mesh;
    double meshing_ms = 0.0;
    std::string error_code;
    std::string error_message;
};

enum class ChunkMeshingMode : std::uint8_t {
    reference,
    greedy,
};

struct ChunkMeshSchedulerConfig {
    std::uint32_t worker_count = 2;
    std::size_t max_concurrent_jobs = 4;
    std::size_t max_completed_results = 256;
    std::size_t max_cached_snapshot_buffers = 8;
    std::size_t max_cached_mesh_buffers = 8;
    ChunkMeshingMode meshing_mode = ChunkMeshingMode::greedy;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkMeshSchedulerStats {
    std::size_t in_flight_jobs = 0;
    std::size_t completed_mailbox_count = 0;
    std::size_t pooled_snapshot_buffers = 0;
    std::size_t pooled_snapshot_capacity_cells = 0;
    std::size_t pooled_mesh_buffers = 0;
    std::size_t pooled_mesh_vertex_capacity = 0;
    std::size_t pooled_mesh_index_capacity = 0;
    std::size_t pooled_mesh_section_capacity = 0;
    std::uint64_t submitted_jobs = 0;
    std::uint64_t completed_jobs = 0;
    std::uint64_t cancelled_jobs = 0;
    std::uint64_t failed_jobs = 0;
};

class ChunkMeshScheduler {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkMeshScheduler>>
    create(ChunkMeshSchedulerConfig config);

    ~ChunkMeshScheduler();

    ChunkMeshScheduler(const ChunkMeshScheduler&) = delete;
    ChunkMeshScheduler& operator=(const ChunkMeshScheduler&) = delete;

    [[nodiscard]] std::vector<world::VoxelCell>
    acquire_snapshot_cells(std::size_t minimum_capacity);
    [[nodiscard]] core::Status submit(ChunkMeshRequest request);
    [[nodiscard]] std::vector<ChunkMeshResult>
    drain_completed(std::size_t maximum_results = static_cast<std::size_t>(-1));
    void recycle_mesh(world::ChunkMesh mesh) noexcept;

    void cancel(world::ChunkIdentity identity) noexcept;
    void cancel_all() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool has_in_flight(world::ChunkIdentity identity) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    in_flight_revision(world::ChunkIdentity identity) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    in_flight_stage_revision(world::ChunkIdentity identity) const noexcept;
    [[nodiscard]] bool has_capacity() const noexcept;
    [[nodiscard]] const ChunkMeshSchedulerStats& stats() noexcept;

  private:
    struct SharedState;
    struct ActiveJob {
        jobs::JobId job_id{};
        std::uint64_t stage_revision = 0;
        std::uint64_t center_revision = 0;
        std::uint64_t render_table_revision = 0;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    ChunkMeshScheduler(ChunkMeshSchedulerConfig config, std::unique_ptr<jobs::IJobSystem> jobs,
                       std::shared_ptr<SharedState> shared_state);

    void refresh_stats() noexcept;

    ChunkMeshSchedulerConfig config_{};
    std::unique_ptr<jobs::IJobSystem> jobs_;
    std::shared_ptr<SharedState> shared_state_;
    std::map<world::ChunkIdentity, ActiveJob> active_jobs_;
    ChunkMeshSchedulerStats stats_{};
};

} // namespace heartstead::renderer
