#pragma once

#include "engine/core/result.hpp"
#include "engine/save/save_database.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/simulation/simulation_lod.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/regions/region_graph.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_state.hpp"
#include "engine/world/worldgen/terrain_generator.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace heartstead::world {

class IChunkEditDeltaSource {
  public:
    virtual ~IChunkEditDeltaSource() = default;

    [[nodiscard]] virtual core::Result<std::optional<save::ChunkEditSaveRecord>>
    read_chunk_delta(ChunkCoord coord) const = 0;
};

class FileSaveChunkEditDeltaSource final : public IChunkEditDeltaSource {
  public:
    explicit FileSaveChunkEditDeltaSource(save::FileSaveDatabase database) noexcept;

    [[nodiscard]] core::Result<std::optional<save::ChunkEditSaveRecord>>
    read_chunk_delta(ChunkCoord coord) const override;

  private:
    save::FileSaveDatabase database_;
};

class IChunkEditDeltaSink {
  public:
    virtual ~IChunkEditDeltaSink() = default;

    [[nodiscard]] virtual core::Status
    write_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const = 0;
};

class FileSaveChunkEditDeltaSink final : public IChunkEditDeltaSink {
  public:
    explicit FileSaveChunkEditDeltaSink(const save::FileSaveDatabase& database) noexcept;

    [[nodiscard]] core::Status
    write_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const override;

  private:
    const save::FileSaveDatabase* database_ = nullptr;
};

class IChunkReplicationDeltaSink {
  public:
    virtual ~IChunkReplicationDeltaSink() = default;

    [[nodiscard]] virtual core::Status
    replicate_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const = 0;
};

enum class ChunkStreamLoadSource {
    already_loaded,
    generated,
    generated_with_saved_delta,
};

struct ChunkStreamLoadReport {
    ChunkCoord coord;
    ChunkIdentity identity;
    ChunkStreamLoadSource source = ChunkStreamLoadSource::already_loaded;
    bool chunk_was_already_loaded = false;
    bool generated_chunk_inserted = false;
    bool saved_delta_applied = false;
    std::size_t saved_edit_count = 0;
};

struct ChunkStreamInterestPolicy {
    static constexpr std::uint16_t max_load_radius_chunks = 32;

    std::uint16_t load_horizontal_radius_chunks = 1;
    std::uint16_t load_vertical_radius_chunks = 1;
    std::uint16_t retain_horizontal_radius_chunks = 2;
    std::uint16_t retain_vertical_radius_chunks = 2;

    [[nodiscard]] core::Status validate() const;
};

struct ChunkStreamInterestPlan {
    std::vector<ChunkCoord> load_requests;
    std::vector<ChunkCoord> evictable_chunks;
    std::vector<ChunkCoord> retained_chunks;
    std::vector<ChunkCoord> pinned_dirty_chunks;
    std::size_t viewer_count = 0;
    std::size_t loaded_chunk_count = 0;
    std::size_t desired_chunk_count = 0;
};

struct ChunkStreamEvictionReport {
    std::size_t requested_count = 0;
    std::vector<ChunkCoord> evicted_chunks;
    std::vector<ChunkIdentity> evicted_identities;
    std::vector<ChunkCoord> missing_chunks;
    std::vector<ChunkCoord> retained_dirty_chunks;

    [[nodiscard]] std::size_t evicted_count() const noexcept;
};

struct ChunkStreamSaveFlushReport {
    std::size_t requested_count = 0;
    std::vector<ChunkCoord> written_chunks;
    std::vector<ChunkCoord> clean_chunks;
    std::vector<ChunkCoord> missing_chunks;
    std::vector<ChunkCoord> dirty_without_delta_chunks;
    std::vector<ChunkCoord> stale_chunks;

    [[nodiscard]] std::size_t written_count() const noexcept;
};

struct ChunkStreamReplicationFlushReport {
    std::size_t requested_count = 0;
    std::vector<ChunkCoord> replicated_chunks;
    std::vector<ChunkCoord> clean_chunks;
    std::vector<ChunkCoord> missing_chunks;
    std::vector<ChunkCoord> dirty_without_delta_chunks;
    std::vector<ChunkCoord> stale_chunks;

    [[nodiscard]] std::size_t replicated_count() const noexcept;
};

struct ChunkStreamMaintenanceReport {
    ChunkStreamInterestPlan interest;
    std::vector<ChunkStreamLoadReport> loads;
    std::optional<ChunkStreamSaveFlushReport> save_flush;
    std::optional<ChunkStreamReplicationFlushReport> replication_flush;
    ChunkStreamEvictionReport eviction;

    [[nodiscard]] std::size_t loaded_count() const noexcept;
};

class ChunkStreamer {
  public:
    [[nodiscard]] static core::Result<ChunkStreamLoadReport>
    load_chunk(WorldState& state, ChunkCoord coord, const TerrainGenerationConfig& generation,
               const RegionGraph& regions, const VoxelPalette& palette,
               const IChunkEditDeltaSource* saved_deltas = nullptr);

    [[nodiscard]] static core::Result<ChunkStreamInterestPlan>
    plan_interest(const WorldState& state, const std::vector<simulation::SimulationViewer>& viewers,
                  const ChunkStreamInterestPolicy& policy = {});

    [[nodiscard]] static ChunkStreamEvictionReport
    evict_chunks(WorldState& state, std::span<const ChunkCoord> requested_chunks);

    [[nodiscard]] static core::Result<ChunkStreamSaveFlushReport>
    flush_save_deltas(WorldState& state, std::span<const ChunkCoord> requested_chunks,
                      const IChunkEditDeltaSink& sink);

    [[nodiscard]] static core::Result<ChunkStreamReplicationFlushReport>
    flush_replication_deltas(WorldState& state, std::span<const ChunkCoord> requested_chunks,
                             const IChunkReplicationDeltaSink& sink);

    [[nodiscard]] static core::Result<ChunkStreamMaintenanceReport>
    maintain_interest(WorldState& state, const std::vector<simulation::SimulationViewer>& viewers,
                      const ChunkStreamInterestPolicy& policy = {},
                      const IChunkEditDeltaSink* save_sink = nullptr,
                      const IChunkReplicationDeltaSink* replication_sink = nullptr);

    [[nodiscard]] static core::Result<ChunkStreamMaintenanceReport> maintain_loaded_interest(
        WorldState& state, const std::vector<simulation::SimulationViewer>& viewers,
        const ChunkStreamInterestPolicy& policy, const TerrainGenerationConfig& generation,
        const RegionGraph& regions, const VoxelPalette& palette,
        const IChunkEditDeltaSource* saved_deltas = nullptr,
        const IChunkEditDeltaSink* save_sink = nullptr,
        const IChunkReplicationDeltaSink* replication_sink = nullptr);
};

[[nodiscard]] const char* chunk_stream_load_source_name(ChunkStreamLoadSource source) noexcept;
[[nodiscard]] ChunkCoord
chunk_coord_for_simulation_coord(simulation::SimulationCoord coord) noexcept;

} // namespace heartstead::world
