#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace heartstead::world {

struct ChunkLoadRequestIdTag;
using ChunkLoadRequestId = core::StrongU64Id<ChunkLoadRequestIdTag>;

enum class ChunkLoadResultState : std::uint8_t {
    succeeded,
    failed,
    cancelled,
};

struct ChunkLoadSchedulerConfig {
    std::uint32_t worker_count = 2;
    std::size_t max_concurrent_requests = 4;
    std::size_t max_completed_results = 4;
    std::size_t reservation_bytes_per_request = 64U * 1024U * 1024U;
    std::size_t max_reserved_working_bytes = 256U * 1024U * 1024U;
    std::size_t max_publications_per_update = 2;
    std::uint64_t max_publication_time_us = 500;
    std::uint64_t first_request_id = 1;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkLoadSchedulerContext {
    TerrainGenerationConfig generation;
    RegionGraph regions;
    VoxelPalette palette;
    // Implementations are called concurrently by the configured workers and must be safe for
    // concurrent const access. FileSaveChunkEditDeltaSource owns an immutable database handle.
    std::shared_ptr<const IChunkEditDeltaSource> saved_deltas;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkLoadFailure {
    ChunkLoadRequestId request_id;
    ChunkCoord coord;
    core::Error error;
};

struct ChunkLoadPublicationReport {
    std::size_t collected_worker_results = 0;
    std::vector<ChunkStreamLoadReport> published;
    std::vector<ChunkCoord> cancelled;
    std::vector<ChunkCoord> stale;
    std::vector<ChunkLoadFailure> failures;
    std::uint64_t publication_time_us = 0;
    bool item_budget_exhausted = false;
    bool time_budget_exhausted = false;

    [[nodiscard]] std::size_t processed_count() const noexcept;
};

struct ChunkLoadSchedulerStats {
    std::size_t in_flight_requests = 0;
    std::size_t completed_mailbox_count = 0;
    std::size_t ready_for_publication_count = 0;
    std::size_t reserved_working_bytes = 0;
    std::size_t reserved_working_bytes_high_water = 0;
    std::uint64_t submitted_requests = 0;
    std::uint64_t published_requests = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t cancelled_requests = 0;
    std::uint64_t stale_requests = 0;
    std::uint64_t rejected_requests = 0;
    std::uint64_t duplicate_requests = 0;
    std::uint64_t oldest_queued_request_age_us = 0;
    double last_disk_read_ms = 0.0;
    double last_decode_ms = 0.0;
    double last_generation_ms = 0.0;
    double last_prepare_ms = 0.0;
    double last_worker_ms = 0.0;
    double last_pipeline_latency_ms = 0.0;
    double maximum_pipeline_latency_ms = 0.0;
    std::uint64_t maximum_publication_time_us = 0;
};

class ChunkLoadScheduler {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<ChunkLoadScheduler>>
    create(ChunkLoadSchedulerContext context, ChunkLoadSchedulerConfig config = {});

    ~ChunkLoadScheduler();

    ChunkLoadScheduler(const ChunkLoadScheduler&) = delete;
    ChunkLoadScheduler& operator=(const ChunkLoadScheduler&) = delete;

    [[nodiscard]] core::Result<ChunkLoadRequestId>
    submit(ChunkCoord coord, jobs::JobPriority priority = jobs::JobPriority::normal);
    [[nodiscard]] core::Status cancel(ChunkCoord coord) noexcept;
    std::size_t cancel_all_except(std::span<const ChunkCoord> desired);
    void cancel_all() noexcept;

    [[nodiscard]] ChunkLoadPublicationReport update(WorldState& state);
    [[nodiscard]] bool has_capacity() const noexcept;
    [[nodiscard]] bool has_in_flight() const noexcept;
    [[nodiscard]] bool contains(ChunkCoord coord) const noexcept;
    [[nodiscard]] const ChunkLoadSchedulerStats& stats() noexcept;
    void shutdown() noexcept;

  private:
    struct SharedContext;
    struct SharedState;
    struct ChunkLoadResult {
        ChunkLoadRequestId request_id;
        ChunkCoord coord;
        ChunkLoadResultState state = ChunkLoadResultState::failed;
        ChunkStreamLoadSource source = ChunkStreamLoadSource::generated;
        std::optional<PreparedGeneratedChunk> prepared;
        std::size_t saved_edit_count = 0;
        std::size_t reserved_working_bytes = 0;
        double disk_read_ms = 0.0;
        double decode_ms = 0.0;
        double generation_ms = 0.0;
        double prepare_ms = 0.0;
        double worker_ms = 0.0;
        double pipeline_latency_ms = 0.0;
        std::string error_code;
        std::string error_message;
    };
    struct ActiveRequest {
        jobs::JobId job_id;
        ChunkCoord coord;
        std::size_t reserved_working_bytes = 0;
        std::chrono::steady_clock::time_point submitted_at;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    ChunkLoadScheduler(ChunkLoadSchedulerConfig config, std::unique_ptr<jobs::IJobSystem> jobs,
                       std::shared_ptr<const SharedContext> context,
                       std::shared_ptr<SharedState> shared_state);

    void collect_completed(ChunkLoadPublicationReport& report);
    void finish_request(const ChunkLoadResult& result) noexcept;
    void refresh_stats() noexcept;

    ChunkLoadSchedulerConfig config_;
    std::unique_ptr<jobs::IJobSystem> jobs_;
    std::shared_ptr<const SharedContext> context_;
    std::shared_ptr<SharedState> shared_state_;
    std::map<ChunkLoadRequestId, ActiveRequest> active_requests_;
    std::map<ChunkCoord, ChunkLoadRequestId> active_by_coord_;
    std::deque<ChunkLoadResult> ready_for_publication_;
    std::uint64_t next_request_id_ = 1;
    ChunkLoadSchedulerStats stats_;
};

[[nodiscard]] const char* chunk_load_result_state_name(ChunkLoadResultState state) noexcept;

} // namespace heartstead::world
