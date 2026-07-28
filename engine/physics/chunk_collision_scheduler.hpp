#pragma once

#include "engine/core/result.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/world/collision/chunk_collision.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::physics {

struct ChunkCollisionRequest {
    world::ChunkCollisionSnapshot snapshot;
    std::shared_ptr<const world::ChunkCollisionTableSnapshot> collision_table;
};

enum class ChunkCollisionResultState : std::uint8_t {
    succeeded,
    failed,
    cancelled,
};

struct ChunkCollisionResult {
    world::ChunkIdentity identity{};
    std::uint64_t center_revision = 0;
    std::uint64_t collision_table_revision = 0;
    ChunkCollisionResultState state = ChunkCollisionResultState::failed;
    std::optional<world::ChunkCollisionShape> shape;
    double cooking_ms = 0.0;
    std::string error_code;
    std::string error_message;
};

struct ChunkCollisionSchedulerConfig {
    std::uint32_t worker_count = 1;
    std::size_t max_concurrent_jobs = 2;
    std::size_t max_completed_results = 64;
    std::size_t max_cached_snapshot_buffers = 4;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkCollisionSchedulerStats {
    std::size_t in_flight_jobs = 0;
    std::size_t completed_mailbox_count = 0;
    std::size_t pooled_snapshot_buffers = 0;
    std::size_t pooled_snapshot_capacity_cells = 0;
    std::uint64_t submitted_jobs = 0;
    std::uint64_t completed_jobs = 0;
    std::uint64_t cancelled_jobs = 0;
    std::uint64_t failed_jobs = 0;
};

class ChunkCollisionScheduler {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkCollisionScheduler>>
    create(ChunkCollisionSchedulerConfig config = {});

    ~ChunkCollisionScheduler();

    ChunkCollisionScheduler(const ChunkCollisionScheduler&) = delete;
    ChunkCollisionScheduler& operator=(const ChunkCollisionScheduler&) = delete;

    [[nodiscard]] std::vector<world::VoxelCell>
    acquire_snapshot_cells(std::size_t minimum_capacity);
    [[nodiscard]] core::Status submit(ChunkCollisionRequest request);
    [[nodiscard]] std::vector<ChunkCollisionResult>
    drain_completed(std::size_t maximum_results = static_cast<std::size_t>(-1));

    void cancel(world::ChunkIdentity identity) noexcept;
    void cancel_all() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool has_in_flight(world::ChunkIdentity identity) const noexcept;
    [[nodiscard]] bool has_capacity() const noexcept;
    [[nodiscard]] const ChunkCollisionSchedulerStats& stats() noexcept;

  private:
    struct SharedState;
    struct ActiveJob {
        jobs::JobId job_id{};
        std::uint64_t center_revision = 0;
        std::uint64_t collision_table_revision = 0;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    ChunkCollisionScheduler(ChunkCollisionSchedulerConfig config,
                            std::unique_ptr<jobs::IJobSystem> jobs,
                            std::shared_ptr<SharedState> shared_state);

    void refresh_stats() noexcept;

    ChunkCollisionSchedulerConfig config_{};
    std::unique_ptr<jobs::IJobSystem> jobs_;
    std::shared_ptr<SharedState> shared_state_;
    std::map<world::ChunkIdentity, ActiveJob> active_jobs_;
    ChunkCollisionSchedulerStats stats_{};
};

} // namespace heartstead::physics
