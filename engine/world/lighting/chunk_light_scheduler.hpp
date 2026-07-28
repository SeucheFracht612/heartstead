#pragma once

#include "engine/core/result.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/world/lighting/voxel_light.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::world {

struct ChunkLightRequest {
    std::uint64_t request_id = 0;
    VoxelLightSnapshot snapshot;
    std::shared_ptr<const VoxelLightBlockTable> block_table;
};

enum class ChunkLightResultState : std::uint8_t {
    succeeded,
    failed,
    cancelled,
};

struct ChunkLightResult {
    std::uint64_t request_id = 0;
    std::uint64_t block_table_revision = 0;
    ChunkLightResultState state = ChunkLightResultState::failed;
    std::optional<VoxelLightSolveResult> light;
    double solve_ms = 0.0;
    std::string error_code;
    std::string error_message;
};

struct ChunkLightSchedulerConfig {
    std::uint32_t worker_count = 1;
    std::size_t max_completed_results = 4;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkLightSchedulerStats {
    bool in_flight = false;
    std::size_t completed_mailbox_count = 0;
    std::uint64_t submitted_jobs = 0;
    std::uint64_t completed_jobs = 0;
    std::uint64_t cancelled_jobs = 0;
    std::uint64_t failed_jobs = 0;
};

class ChunkLightScheduler {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkLightScheduler>>
    create(ChunkLightSchedulerConfig config = {});

    ~ChunkLightScheduler();

    ChunkLightScheduler(const ChunkLightScheduler&) = delete;
    ChunkLightScheduler& operator=(const ChunkLightScheduler&) = delete;

    [[nodiscard]] core::Status submit(ChunkLightRequest request);
    [[nodiscard]] std::vector<ChunkLightResult>
    drain_completed(std::size_t maximum_results = static_cast<std::size_t>(-1));

    void cancel() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool has_in_flight() const noexcept;
    [[nodiscard]] const ChunkLightSchedulerStats& stats() noexcept;

  private:
    struct SharedState;
    struct ActiveJob {
        jobs::JobId job_id{};
        std::uint64_t request_id = 0;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    ChunkLightScheduler(ChunkLightSchedulerConfig config, std::unique_ptr<jobs::IJobSystem> jobs,
                        std::shared_ptr<SharedState> shared_state);

    void refresh_stats() noexcept;

    ChunkLightSchedulerConfig config_{};
    std::unique_ptr<jobs::IJobSystem> jobs_;
    std::shared_ptr<SharedState> shared_state_;
    std::optional<ActiveJob> active_job_;
    ChunkLightSchedulerStats stats_{};
};

} // namespace heartstead::world
