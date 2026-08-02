#pragma once

#include "engine/jobs/job_system.hpp"
#include "engine/renderer/terrain/far_terrain_clipmap.hpp"
#include "engine/renderer/terrain/far_terrain_lod_updates.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::renderer {

struct FarTerrainMeshRequest {
    FarTerrainLodUpdateRequest update;
    FarTerrainSurfaceGrid surface;
};

enum class FarTerrainMeshResultState : std::uint8_t {
    succeeded,
    failed,
    cancelled,
};

struct FarTerrainMeshResult {
    FarTerrainLodUpdateRequest update;
    FarTerrainMeshResultState state = FarTerrainMeshResultState::failed;
    std::optional<FarTerrainPatchMesh> mesh;
    double meshing_ms = 0.0;
    std::string error_code;
    std::string error_message;
};

struct FarTerrainMeshSchedulerConfig {
    std::uint32_t worker_count = 1;
    std::size_t maximum_concurrent_jobs = 4;
    std::size_t maximum_completed_results = 16;
    std::size_t maximum_cached_surface_buffers = 8;
    std::size_t maximum_cached_mesh_buffers = 8;

    [[nodiscard]] core::Status validate() const;
};

struct FarTerrainMeshSchedulerStats {
    std::size_t in_flight_jobs = 0;
    std::size_t completed_mailbox_count = 0;
    std::size_t pooled_surface_buffers = 0;
    std::size_t pooled_surface_sample_capacity = 0;
    std::size_t pooled_mesh_buffers = 0;
    std::size_t pooled_mesh_vertex_capacity = 0;
    std::size_t pooled_mesh_index_capacity = 0;
    std::uint64_t submitted_jobs = 0;
    std::uint64_t completed_jobs = 0;
    std::uint64_t cancelled_jobs = 0;
    std::uint64_t failed_jobs = 0;
};

struct FarTerrainMeshTicket {
    FarTerrainPatchKey key;
    std::uint64_t request_revision = 0;
};

// Bounded worker-side topology construction. Requests contain a complete immutable surface grid;
// only the render owner captures grids, submits tickets, drains results, and publishes residency.
class FarTerrainMeshScheduler {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<FarTerrainMeshScheduler>>
    create(FarTerrainClipmap clipmap, FarTerrainMeshSchedulerConfig config = {});

    ~FarTerrainMeshScheduler();

    FarTerrainMeshScheduler(const FarTerrainMeshScheduler&) = delete;
    FarTerrainMeshScheduler& operator=(const FarTerrainMeshScheduler&) = delete;

    [[nodiscard]] std::vector<FarTerrainSurfaceSample>
    acquire_surface_samples(std::size_t minimum_capacity);
    [[nodiscard]] core::Status submit(FarTerrainMeshRequest request);
    [[nodiscard]] std::vector<FarTerrainMeshResult>
    drain_completed(std::size_t maximum_results = static_cast<std::size_t>(-1));
    void recycle_mesh(FarTerrainPatchMesh mesh) noexcept;

    void cancel(const FarTerrainPatchKey& key) noexcept;
    void cancel_all() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool has_in_flight(const FarTerrainPatchKey& key) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    in_flight_request_revision(const FarTerrainPatchKey& key) const noexcept;
    [[nodiscard]] std::vector<FarTerrainMeshTicket> in_flight_tickets() const;
    [[nodiscard]] bool has_capacity() const noexcept;
    [[nodiscard]] const FarTerrainMeshSchedulerStats& stats() noexcept;

  private:
    struct SharedState;
    struct ActiveJob {
        jobs::JobId job_id{};
        std::uint64_t request_revision = 0;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    FarTerrainMeshScheduler(FarTerrainMeshSchedulerConfig config,
                            std::unique_ptr<jobs::IJobSystem> jobs,
                            std::shared_ptr<const FarTerrainClipmap> clipmap,
                            std::shared_ptr<SharedState> shared_state);

    void refresh_stats() noexcept;

    FarTerrainMeshSchedulerConfig config_{};
    std::unique_ptr<jobs::IJobSystem> jobs_;
    std::shared_ptr<const FarTerrainClipmap> clipmap_;
    std::shared_ptr<SharedState> shared_state_;
    std::map<FarTerrainPatchKey, ActiveJob> active_jobs_;
    FarTerrainMeshSchedulerStats stats_{};
};

} // namespace heartstead::renderer
