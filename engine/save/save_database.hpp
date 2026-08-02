#pragma once

#include "engine/core/result.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/save/save_migration.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace heartstead::save {

struct SaveDatabaseStats {
    bool uses_generation_manifest = false;
    std::string active_generation;
    std::size_t committed_generation_count = 0;
    std::size_t staged_generation_count = 0;
    std::size_t stale_generation_count = 0;
    bool has_snapshot = false;
    std::uintmax_t snapshot_bytes = 0;
    std::size_t chunk_delta_count = 0;
    std::uintmax_t chunk_delta_bytes = 0;
    std::size_t journal_entry_count = 0;
    std::uintmax_t journal_bytes = 0;
    std::uint64_t journal_checkpoint_sequence = 0;
    std::uint64_t journal_highest_sequence = 0;
};

struct SaveJournalReceipt {
    std::uint64_t sequence = 0;
    std::size_t encoded_bytes = 0;
};

struct SaveJournalCompactionResult {
    bool compacted = false;
    std::uint64_t compacted_sequence = 0;
    std::size_t removed_entry_count = 0;
};

struct SaveJournalRecoveryResult {
    std::size_t discarded_temporary_entry_count = 0;
    SaveJournalCompactionResult compaction;

    [[nodiscard]] bool changed() const noexcept;
};

struct SaveDatabaseMaintenancePolicy {
    bool recover_staged_generations = true;
    bool recover_snapshot_journal = true;
    bool prune_stale_generations = false;
    std::size_t keep_stale_generations = 1;
    bool compact_chunk_deltas = false;
};

struct SaveDatabaseMaintenanceResult {
    SaveDatabaseStats before;
    SaveDatabaseStats after;
    std::size_t recovered_staged_generation_count = 0;
    SaveJournalRecoveryResult journal_recovery;
    std::size_t pruned_stale_generation_count = 0;
    std::size_t compacted_chunk_delta_count = 0;

    [[nodiscard]] bool changed() const noexcept;
};

struct SaveDatabaseMigrationResult {
    SaveDatabaseStats before;
    SaveDatabaseStats after;
    SaveMigrationResult migration;
    bool wrote_snapshot = false;

    [[nodiscard]] bool changed() const noexcept;
};

class FileSaveDatabase {
  public:
    explicit FileSaveDatabase(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    [[nodiscard]] core::Status write_snapshot(const SaveSnapshot& snapshot) const;
    [[nodiscard]] core::Result<SaveSnapshot> read_snapshot() const;
    [[nodiscard]] core::Result<SaveSnapshot>
    read_validated_snapshot(const modding::PrototypeRegistry& prototypes) const;

    // Appends a checksummed, immutable snapshot record and does not return until that record has
    // reached the platform's stable-storage boundary. Serialization and this blocking call belong
    // on a save worker. Readers automatically prefer an accepted record newer than the checkpoint.
    [[nodiscard]] core::Result<SaveJournalReceipt>
    journal_snapshot(const SaveSnapshot& snapshot) const;
    [[nodiscard]] core::Result<SaveJournalCompactionResult>
    compact_snapshot_journal() const;
    [[nodiscard]] core::Result<SaveJournalRecoveryResult>
    recover_snapshot_journal() const;

    [[nodiscard]] core::Status write_chunk_delta(const ChunkEditSaveRecord& chunk_delta) const;
    [[nodiscard]] core::Status
    write_chunk_deltas(std::span<const ChunkEditSaveRecord> chunk_deltas) const;
    [[nodiscard]] core::Result<ChunkEditSaveRecord> read_chunk_delta(world::ChunkCoord coord) const;
    [[nodiscard]] core::Result<std::vector<ChunkEditSaveRecord>> read_chunk_deltas() const;

    [[nodiscard]] core::Result<std::size_t> compact_chunk_deltas() const;
    [[nodiscard]] core::Status prune_stale_generations(std::size_t keep_stale_generations) const;
    [[nodiscard]] core::Result<std::size_t> recover_staged_generations() const;
    [[nodiscard]] core::Result<SaveDatabaseMaintenanceResult>
    maintain(const SaveDatabaseMaintenancePolicy& policy) const;
    [[nodiscard]] core::Result<SaveDatabaseMigrationResult>
    migrate_to_schema(const SaveMigrationRegistry& registry,
                      std::uint32_t target_schema_version) const;

    [[nodiscard]] core::Result<SaveDatabaseStats> stats() const;

  private:
    std::filesystem::path root_;
};

} // namespace heartstead::save
