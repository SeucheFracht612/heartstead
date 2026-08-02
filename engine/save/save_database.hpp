#pragma once

#include "engine/core/result.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/save/save_migration.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

namespace heartstead::save {

struct FileSaveDatabaseCoordinator;

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
    std::size_t chunk_delta_journal_entry_count = 0;
    std::uintmax_t chunk_delta_journal_bytes = 0;
    std::uint64_t chunk_delta_journal_highest_sequence = 0;
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

struct ChunkDeltaJournalReceipt {
    std::uint64_t sequence = 0;
    std::size_t encoded_bytes = 0;
};

struct ChunkDeltaJournalCompactionResult {
    bool compacted = false;
    std::size_t merged_entry_count = 0;
    std::size_t removed_entry_count = 0;
};

struct ChunkDeltaJournalRecoveryResult {
    std::size_t discarded_temporary_entry_count = 0;
    bool discarded_compacted_directory = false;

    [[nodiscard]] bool changed() const noexcept;
};

struct SaveDatabaseMaintenancePolicy {
    bool recover_staged_generations = true;
    bool recover_snapshot_journal = true;
    bool recover_chunk_delta_journal = true;
    bool prune_stale_generations = false;
    std::size_t keep_stale_generations = 1;
    bool compact_chunk_deltas = false;
    bool compact_chunk_delta_journal = false;
};

struct SaveDatabaseMaintenanceResult {
    SaveDatabaseStats before;
    SaveDatabaseStats after;
    std::size_t recovered_staged_generation_count = 0;
    SaveJournalRecoveryResult journal_recovery;
    ChunkDeltaJournalRecoveryResult chunk_delta_journal_recovery;
    std::size_t pruned_stale_generation_count = 0;
    std::size_t compacted_chunk_delta_count = 0;
    ChunkDeltaJournalCompactionResult chunk_delta_journal_compaction;

    [[nodiscard]] bool changed() const noexcept;
};

struct SaveDatabaseMigrationResult {
    SaveDatabaseStats before;
    SaveDatabaseStats after;
    SaveMigrationResult migration;
    bool wrote_snapshot = false;

    [[nodiscard]] bool changed() const noexcept;
};

enum class FileChunkDeltaStorageKind {
    none,
    external_table,
    inline_snapshot,
};

struct FileChunkDeltaReaderStats {
    FileChunkDeltaStorageKind storage_kind = FileChunkDeltaStorageKind::none;
    std::filesystem::path selected_save_root;
    std::string active_generation;
    std::size_t base_indexed_chunk_delta_count = 0;
    std::size_t journal_entry_count = 0;
    std::size_t indexed_chunk_delta_count = 0;
};

// A generation-scoped, immutable chunk-index view. Opening selects the authoritative full
// snapshot, validates the base index and current chunk journal, and pins that journal end mark.
// Later appends are not visible. Process-local destructive maintenance returns save_database.busy
// while the view is alive. Callers reopen after compaction or generation publication; coordinating
// another process that can mutate the same save root remains an external responsibility.
class FileChunkDeltaReader {
  public:
    FileChunkDeltaReader(const FileChunkDeltaReader&) = delete;
    FileChunkDeltaReader& operator=(const FileChunkDeltaReader&) = delete;
    FileChunkDeltaReader(FileChunkDeltaReader&&) noexcept = default;
    FileChunkDeltaReader& operator=(FileChunkDeltaReader&&) noexcept = default;

    [[nodiscard]] core::Result<std::optional<ChunkEditSaveRecord>>
    read_chunk_delta(world::ChunkCoord coord) const;
    [[nodiscard]] const FileChunkDeltaReaderStats& stats() const noexcept;

  private:
    enum class PayloadKind : std::uint8_t {
        external_file,
        inline_payload,
        journal_entry,
    };

    struct Entry {
        world::ChunkCoord coord;
        PayloadKind payload_kind = PayloadKind::inline_payload;
        std::filesystem::path path;
        std::string value;
        std::uint64_t sequence = 0;
    };

    FileChunkDeltaReader() = default;

    FileChunkDeltaReaderStats stats_;
    std::vector<Entry> entries_;
    std::shared_ptr<FileSaveDatabaseCoordinator> coordinator_;
    std::shared_lock<std::shared_mutex> table_lease_;

    friend class FileSaveDatabase;
};

struct FileChunkDeltaWriterStats {
    std::filesystem::path selected_save_root;
    std::string active_generation;
    std::size_t effective_chunk_delta_count = 0;
    std::size_t effective_payload_bytes = 0;
    std::size_t journal_entry_count = 0;
    std::size_t journal_bytes = 0;
    std::uint64_t highest_sequence = 0;
};

// A generation-scoped append session. Each accepted update is an immutable, checksummed journal
// entry. Instances that resolve to the same save root share a process-local mutation coordinator;
// append operations serialize, and destructive maintenance returns save_database.busy while this
// session is alive. Reopen after publishing a new generation. Cross-process exclusion is external.
class FileChunkDeltaWriter {
  public:
    FileChunkDeltaWriter(const FileChunkDeltaWriter&) = delete;
    FileChunkDeltaWriter& operator=(const FileChunkDeltaWriter&) = delete;
    FileChunkDeltaWriter(FileChunkDeltaWriter&&) noexcept = default;
    FileChunkDeltaWriter& operator=(FileChunkDeltaWriter&&) noexcept = default;

    [[nodiscard]] core::Result<ChunkDeltaJournalReceipt>
    write_chunk_delta(const ChunkEditSaveRecord& chunk_delta);
    [[nodiscard]] const FileChunkDeltaWriterStats& stats() const noexcept;

  private:
    struct Entry {
        world::ChunkCoord coord;
        std::size_t payload_bytes = 0;
    };

    FileChunkDeltaWriter() = default;

    std::filesystem::path database_root_;
    FileChunkDeltaWriterStats stats_;
    std::vector<Entry> entries_;
    std::shared_ptr<FileSaveDatabaseCoordinator> coordinator_;
    std::shared_lock<std::shared_mutex> table_lease_;

    friend class FileSaveDatabase;
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
    [[nodiscard]] core::Result<SaveJournalCompactionResult> compact_snapshot_journal() const;
    [[nodiscard]] core::Result<SaveJournalRecoveryResult> recover_snapshot_journal() const;

    [[nodiscard]] core::Status write_chunk_delta(const ChunkEditSaveRecord& chunk_delta) const;
    [[nodiscard]] core::Status
    write_chunk_deltas(std::span<const ChunkEditSaveRecord> chunk_deltas) const;
    // Prefer a retained reader for repeated or concurrent streaming reads. This convenience API
    // opens a fresh generation-scoped reader for each call.
    [[nodiscard]] core::Result<ChunkEditSaveRecord> read_chunk_delta(world::ChunkCoord coord) const;
    [[nodiscard]] core::Result<std::vector<ChunkEditSaveRecord>> read_chunk_deltas() const;
    [[nodiscard]] core::Result<FileChunkDeltaReader> open_chunk_delta_reader() const;
    [[nodiscard]] core::Result<FileChunkDeltaWriter> open_chunk_delta_writer() const;

    [[nodiscard]] core::Result<std::size_t> compact_chunk_deltas() const;
    [[nodiscard]] core::Result<ChunkDeltaJournalCompactionResult>
    compact_chunk_delta_journal() const;
    [[nodiscard]] core::Result<ChunkDeltaJournalRecoveryResult> recover_chunk_delta_journal() const;
    [[nodiscard]] core::Status prune_stale_generations(std::size_t keep_stale_generations) const;
    [[nodiscard]] core::Result<std::size_t> recover_staged_generations() const;
    [[nodiscard]] core::Result<SaveDatabaseMaintenanceResult>
    maintain(const SaveDatabaseMaintenancePolicy& policy) const;
    [[nodiscard]] core::Result<SaveDatabaseMigrationResult>
    migrate_to_schema(const SaveMigrationRegistry& registry,
                      std::uint32_t target_schema_version) const;

    [[nodiscard]] core::Result<SaveDatabaseStats> stats() const;

  private:
    [[nodiscard]] core::Result<FileChunkDeltaWriter>
    open_chunk_delta_writer_under_lock(std::shared_lock<std::shared_mutex> table_lease) const;

    std::filesystem::path root_;
    std::shared_ptr<FileSaveDatabaseCoordinator> coordinator_;
};

} // namespace heartstead::save
