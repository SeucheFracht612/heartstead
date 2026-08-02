#include "engine/save/save_binary_codec.hpp"
#include "engine/save/save_database.hpp"

#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(std::string_view label) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("heartstead_" + std::string(label) + "_" + std::to_string(nonce) + "_" +
                     std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                return;
            }
        }
        assert(false && "could not create chunk delta journal test directory");
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

[[nodiscard]] heartstead::save::SaveSnapshot
make_snapshot(std::uint64_t world_seed,
              std::vector<heartstead::save::ChunkEditSaveRecord> chunk_edits) {
    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "save-chunk-delta-journal-test";
    snapshot.metadata.world_seed = world_seed;
    snapshot.chunk_edits = std::move(chunk_edits);
    return snapshot;
}

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    assert(output);
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    assert(output);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    assert(output);
}

[[nodiscard]] std::string read_required(heartstead::save::FileChunkDeltaReader& reader,
                                        heartstead::world::ChunkCoord coord) {
    auto loaded = reader.read_chunk_delta(coord);
    assert(loaded && loaded.value().has_value());
    return loaded.value()->encoded_edit_delta;
}

void test_append_overlay_and_pinned_readers() {
    TemporaryDirectory temporary("chunk_delta_journal_overlay");
    heartstead::save::FileSaveDatabase database(temporary.path());
    const heartstead::world::ChunkCoord coord_a{1, 0, 0};
    const heartstead::world::ChunkCoord coord_b{2, 0, 0};
    const heartstead::world::ChunkCoord coord_c{3, 0, 0};
    assert(database.write_snapshot(make_snapshot(1, {{coord_a, "base-a"}, {coord_b, "base-b"}})));

    auto base_opened = database.open_chunk_delta_reader();
    assert(base_opened);
    auto base_reader = std::move(base_opened).value();

    auto opened_writer = database.open_chunk_delta_writer();
    assert(opened_writer);
    auto writer = std::move(opened_writer).value();
    assert(writer.stats().effective_chunk_delta_count == 2);
    auto first = writer.write_chunk_delta({coord_a, "journal-a-1"});
    assert(first && first.value().sequence == 1);

    auto middle_opened = database.open_chunk_delta_reader();
    assert(middle_opened);
    auto middle_reader = std::move(middle_opened).value();
    assert(middle_reader.stats().base_indexed_chunk_delta_count == 2);
    assert(middle_reader.stats().journal_entry_count == 1);
    assert(read_required(middle_reader, coord_a) == "journal-a-1");

    auto second = writer.write_chunk_delta({coord_c, "journal-c"});
    auto third = writer.write_chunk_delta({coord_a, "journal-a-2"});
    assert(second && second.value().sequence == 2);
    assert(third && third.value().sequence == 3);
    assert(writer.stats().effective_chunk_delta_count == 3);
    assert(writer.stats().effective_payload_bytes == std::string_view("journal-a-2").size() +
                                                         std::string_view("base-b").size() +
                                                         std::string_view("journal-c").size());
    assert(writer.stats().journal_entry_count == 3);
    assert(writer.stats().highest_sequence == 3);

    // Readers retain the exact base+journal end mark captured when they opened.
    assert(read_required(base_reader, coord_a) == "base-a");
    auto base_missing = base_reader.read_chunk_delta(coord_c);
    assert(base_missing && !base_missing.value().has_value());
    assert(read_required(middle_reader, coord_a) == "journal-a-1");
    auto middle_missing = middle_reader.read_chunk_delta(coord_c);
    assert(middle_missing && !middle_missing.value().has_value());

    heartstead::save::FileSaveDatabase restarted(temporary.path());
    auto current_opened = restarted.open_chunk_delta_reader();
    assert(current_opened);
    auto current_reader = std::move(current_opened).value();
    assert(current_reader.stats().journal_entry_count == 3);
    assert(current_reader.stats().indexed_chunk_delta_count == 3);
    assert(read_required(current_reader, coord_a) == "journal-a-2");
    assert(read_required(current_reader, coord_b) == "base-b");
    assert(read_required(current_reader, coord_c) == "journal-c");

    auto loaded = restarted.read_snapshot();
    assert(loaded && loaded.value().chunk_edits.size() == 3);
    assert(loaded.value().chunk_edits[0].encoded_edit_delta == "journal-a-2");
    assert(loaded.value().chunk_edits[1].encoded_edit_delta == "base-b");
    assert(loaded.value().chunk_edits[2].encoded_edit_delta == "journal-c");

    auto stats = restarted.stats();
    assert(stats);
    assert(stats.value().chunk_delta_count == 3);
    assert(stats.value().chunk_delta_journal_entry_count == 3);
    assert(stats.value().chunk_delta_journal_highest_sequence == 3);
}

void test_bulk_export_validates_base_payloads_shadowed_by_the_journal() {
    TemporaryDirectory temporary("chunk_delta_journal_bulk_integrity");
    heartstead::save::FileSaveDatabase database(temporary.path());
    const heartstead::world::ChunkCoord coord{1, 0, 0};
    assert(database.write_snapshot(make_snapshot(13, {{coord, "base"}})));
    assert(database.write_chunk_delta({coord, "journal"}));

    const auto base_payload = temporary.path() / "generations" / "generation_1" / "chunks" /
                              "c_1_0_0.delta";
    write_text(base_payload, "");

    // A retained streaming view uses the newer durable journal authority for this coordinate.
    auto effective = database.read_chunk_delta(coord);
    assert(effective && effective.value().encoded_edit_delta == "journal");

    // Bulk export is also an integrity read and must not hide corruption in dormant base records.
    auto bulk = database.read_chunk_deltas();
    assert(!bulk && bulk.error().code == "save_database.empty_chunk_delta");
}

void test_stale_writer_and_snapshot_authority_fail_closed() {
    TemporaryDirectory temporary("chunk_delta_journal_stale_writer");
    heartstead::save::FileSaveDatabase database(temporary.path());
    const heartstead::world::ChunkCoord coord{4, 0, 0};
    const heartstead::world::ChunkCoord pending_coord{5, 0, 0};
    assert(database.write_snapshot(make_snapshot(2, {{coord, "base"}})));

    auto first_opened = database.open_chunk_delta_writer();
    auto stale_opened = database.open_chunk_delta_writer();
    assert(first_opened && stale_opened);
    auto first_writer = std::move(first_opened).value();
    auto stale_writer = std::move(stale_opened).value();
    assert(first_writer.write_chunk_delta({coord, "first"}));
    auto rejected = stale_writer.write_chunk_delta({coord, "stale"});
    assert(!rejected);
    assert(rejected.error().code == "save_database.chunk_delta_writer_stale");

    auto generation_writer_opened = database.open_chunk_delta_writer();
    assert(generation_writer_opened);
    auto generation_writer = std::move(generation_writer_opened).value();
    auto accepted_snapshot = database.journal_snapshot(
        make_snapshot(3, {{coord, "snapshot"}, {pending_coord, "pending-extra"}}));
    assert(accepted_snapshot);
    auto pending_reader_opened = database.open_chunk_delta_reader();
    assert(pending_reader_opened);
    auto pending_reader = std::move(pending_reader_opened).value();
    assert(pending_reader.stats().storage_kind ==
           heartstead::save::FileChunkDeltaStorageKind::inline_snapshot);
    assert(read_required(pending_reader, coord) == "snapshot");
    assert(read_required(pending_reader, pending_coord) == "pending-extra");
    auto stats = database.stats();
    assert(stats);
    assert(stats.value().chunk_delta_count == 2);
    assert(stats.value().chunk_delta_bytes ==
           std::string_view("snapshot").size() + std::string_view("pending-extra").size());

    auto pending_writer = database.open_chunk_delta_writer();
    assert(!pending_writer);
    assert(pending_writer.error().code == "save_database.snapshot_journal_pending");
    const std::vector<heartstead::save::ChunkEditSaveRecord> replacement{{coord, "replacement"}};
    auto bulk_replacement = database.write_chunk_deltas(replacement);
    assert(!bulk_replacement);
    assert(bulk_replacement.error().code == "save_database.snapshot_journal_pending");
    auto stale_compaction = database.compact_chunk_delta_journal();
    assert(!stale_compaction);
    assert(stale_compaction.error().code == "save_database.snapshot_journal_pending");
    auto stale_recovery = database.recover_chunk_delta_journal();
    assert(!stale_recovery);
    assert(stale_recovery.error().code == "save_database.snapshot_journal_pending");

    rejected = generation_writer.write_chunk_delta({coord, "during-snapshot"});
    assert(!rejected);
    assert(rejected.error().code == "save_database.snapshot_journal_pending");

    auto compacted_snapshot = database.compact_snapshot_journal();
    assert(compacted_snapshot && compacted_snapshot.value().compacted);
    rejected = generation_writer.write_chunk_delta({coord, "old-generation"});
    assert(!rejected);
    assert(rejected.error().code == "save_database.chunk_delta_writer_stale");
    auto loaded = database.read_chunk_delta(coord);
    assert(loaded && loaded.value().encoded_edit_delta == "snapshot");
}

void test_pending_snapshot_supersedes_corrupt_stale_chunk_journal_during_maintenance() {
    TemporaryDirectory temporary("chunk_delta_journal_snapshot_authority");
    heartstead::save::FileSaveDatabase database(temporary.path());
    const heartstead::world::ChunkCoord coord{6, 0, 0};
    const heartstead::world::ChunkCoord replacement_coord{7, 0, 0};
    assert(database.write_snapshot(make_snapshot(7, {{coord, "base"}})));
    assert(database.write_chunk_delta({coord, "stale-journal"}));

    const auto stale_entry = temporary.path() / "generations" / "generation_1" / "chunk_journal" /
                             "entry_00000000000000000001.hcdj";
    std::fstream stream(stale_entry, std::ios::binary | std::ios::in | std::ios::out);
    assert(stream);
    stream.seekp(-1, std::ios::end);
    const char corrupted = '\xff';
    stream.write(&corrupted, 1);
    stream.close();
    assert(stream);

    auto accepted =
        database.journal_snapshot(make_snapshot(8, {{replacement_coord, "authoritative"}}));
    assert(accepted);
    auto stats = database.stats();
    assert(stats);
    assert(stats.value().chunk_delta_count == 1);
    assert(stats.value().chunk_delta_bytes == std::string_view("authoritative").size());
    assert(stats.value().chunk_delta_journal_entry_count == 1);

    heartstead::save::SaveDatabaseMaintenancePolicy policy;
    auto maintained = database.maintain(policy);
    assert(maintained);
    assert(maintained.value().journal_recovery.compaction.compacted);
    assert(maintained.value().after.active_generation == "generation_2");
    assert(maintained.value().after.chunk_delta_journal_entry_count == 0);
    auto loaded = database.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 8);
    assert(loaded.value().chunk_edits.size() == 1);
    assert(loaded.value().chunk_edits.front().coord == replacement_coord);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "authoritative");
}

void test_checkpoint_and_recovery_are_restart_safe() {
    TemporaryDirectory temporary("chunk_delta_journal_checkpoint");
    heartstead::save::FileSaveDatabase database(temporary.path());
    const heartstead::world::ChunkCoord coord_a{-1, 2, 3};
    const heartstead::world::ChunkCoord coord_b{9, -8, 7};
    assert(database.write_snapshot(make_snapshot(4, {{coord_a, "base"}})));
    const auto save_root = temporary.path() / "generations" / "generation_1";
    const auto abandoned = save_root / "chunk_journal" / "entry_00000000000000000003.hcdj.tmp";
    {
        auto writer_opened = database.open_chunk_delta_writer();
        assert(writer_opened);
        auto writer = std::move(writer_opened).value();
        assert(writer.write_chunk_delta({coord_a, "updated"}));
        assert(writer.write_chunk_delta({coord_b, "added"}));
        write_text(abandoned, "interrupted");

        auto busy_recovery = database.recover_chunk_delta_journal();
        assert(!busy_recovery && busy_recovery.error().code == "save_database.busy");
        auto busy_checkpoint = database.compact_chunk_delta_journal();
        assert(!busy_checkpoint && busy_checkpoint.error().code == "save_database.busy");
    }

    auto recovered = database.recover_chunk_delta_journal();
    assert(recovered && recovered.value().changed());
    assert(recovered.value().discarded_temporary_entry_count == 1);
    assert(!recovered.value().discarded_compacted_directory);
    assert(!std::filesystem::exists(abandoned));

    auto compacted = database.compact_chunk_delta_journal();
    assert(compacted && compacted.value().compacted);
    assert(compacted.value().merged_entry_count == 2);
    assert(compacted.value().removed_entry_count == 2);
    assert(!std::filesystem::exists(save_root / "chunk_journal"));
    assert(!std::filesystem::exists(save_root / "chunk_journal.compacted"));

    heartstead::save::FileSaveDatabase restarted(temporary.path());
    {
        auto reader_opened = restarted.open_chunk_delta_reader();
        assert(reader_opened);
        auto reader = std::move(reader_opened).value();
        assert(reader.stats().storage_kind ==
               heartstead::save::FileChunkDeltaStorageKind::external_table);
        assert(reader.stats().journal_entry_count == 0);
        assert(read_required(reader, coord_a) == "updated");
        assert(read_required(reader, coord_b) == "added");
    }

    auto no_work = restarted.compact_chunk_delta_journal();
    assert(no_work && !no_work.value().compacted);

    write_text(save_root / "chunk_journal.compacted" / "checkpointed", "ignored");
    recovered = restarted.recover_chunk_delta_journal();
    assert(recovered && recovered.value().changed());
    assert(recovered.value().discarded_compacted_directory);
    assert(!std::filesystem::exists(save_root / "chunk_journal.compacted"));
    assert(restarted.read_chunk_delta(coord_a));
}

void test_finalized_corruption_fails_closed() {
    TemporaryDirectory temporary("chunk_delta_journal_corruption");
    heartstead::save::FileSaveDatabase database(temporary.path());
    const heartstead::world::ChunkCoord coord{7, 7, 7};
    assert(database.write_snapshot(make_snapshot(5, {{coord, "base"}})));
    const auto entry = temporary.path() / "generations" / "generation_1" / "chunk_journal" /
                       "entry_00000000000000000001.hcdj";
    {
        auto base_opened = database.open_chunk_delta_reader();
        assert(base_opened);
        auto base_reader = std::move(base_opened).value();
        assert(database.write_chunk_delta({coord, "accepted"}));

        std::fstream stream(entry, std::ios::binary | std::ios::in | std::ios::out);
        assert(stream);
        stream.seekp(-1, std::ios::end);
        const char corrupted = '\xff';
        stream.write(&corrupted, 1);
        stream.close();
        assert(stream);

        // A reader that predates the append still has a valid immutable base view.
        assert(read_required(base_reader, coord) == "base");
    }

    auto opened = database.open_chunk_delta_reader();
    assert(!opened);
    assert(opened.error().code == "save_database.chunk_delta_journal_checksum_mismatch");
    auto writer = database.open_chunk_delta_writer();
    assert(!writer);
    assert(writer.error().code == "save_database.chunk_delta_journal_checksum_mismatch");
    auto loaded = database.read_snapshot();
    assert(!loaded);
    assert(loaded.error().code == "save_database.chunk_delta_journal_checksum_mismatch");
    auto compacted = database.compact_chunk_delta_journal();
    assert(!compacted);
    assert(compacted.error().code == "save_database.chunk_delta_journal_checksum_mismatch");
    auto recovered = database.recover_chunk_delta_journal();
    assert(!recovered);
    assert(recovered.error().code == "save_database.chunk_delta_journal_checksum_mismatch");
}

void test_legacy_snapshot_overlay_compacts_to_external_table() {
    TemporaryDirectory temporary("chunk_delta_journal_legacy");
    const heartstead::world::ChunkCoord coord_a{10, 0, 0};
    const heartstead::world::ChunkCoord coord_b{11, 0, 0};
    auto encoded =
        heartstead::save::SaveBinaryCodec::encode_snapshot(make_snapshot(6, {{coord_a, "legacy"}}));
    assert(encoded);
    write_bytes(temporary.path() / "snapshot.hssb", encoded.value());

    heartstead::save::FileSaveDatabase database(temporary.path());
    {
        auto writer_opened = database.open_chunk_delta_writer();
        assert(writer_opened);
        auto writer = std::move(writer_opened).value();
        assert(writer.write_chunk_delta({coord_b, "journal"}));

        auto reader_opened = database.open_chunk_delta_reader();
        assert(reader_opened);
        auto reader = std::move(reader_opened).value();
        assert(reader.stats().storage_kind ==
               heartstead::save::FileChunkDeltaStorageKind::inline_snapshot);
        assert(reader.stats().journal_entry_count == 1);
        assert(read_required(reader, coord_a) == "legacy");
        assert(read_required(reader, coord_b) == "journal");

        auto busy_checkpoint = database.compact_chunk_delta_journal();
        assert(!busy_checkpoint && busy_checkpoint.error().code == "save_database.busy");
    }

    auto compacted = database.compact_chunk_delta_journal();
    assert(compacted && compacted.value().compacted);
    auto reader_opened = database.open_chunk_delta_reader();
    assert(reader_opened);
    auto reader = std::move(reader_opened).value();
    assert(reader.stats().storage_kind ==
           heartstead::save::FileChunkDeltaStorageKind::external_table);
    assert(reader.stats().journal_entry_count == 0);
    assert(read_required(reader, coord_a) == "legacy");
    assert(read_required(reader, coord_b) == "journal");
}

void test_live_reader_gates_destructive_maintenance_across_instances() {
    TemporaryDirectory temporary("chunk_delta_journal_live_reader_gate");
    heartstead::save::FileSaveDatabase database(temporary.path());
    heartstead::save::FileSaveDatabase alias(temporary.path() / ".");
    const heartstead::world::ChunkCoord old_coord{21, 0, 0};
    const heartstead::world::ChunkCoord new_coord{22, 0, 0};
    assert(database.write_snapshot(make_snapshot(10, {{old_coord, "base"}})));
    assert(database.write_chunk_delta({old_coord, "overlay"}));

    {
        auto opened = database.open_chunk_delta_reader();
        assert(opened);
        auto reader = std::move(opened).value();
        assert(read_required(reader, old_coord) == "overlay");

        auto checkpoint = alias.compact_chunk_delta_journal();
        assert(!checkpoint && checkpoint.error().code == "save_database.busy");
        const std::vector<heartstead::save::ChunkEditSaveRecord> replacement{
            {old_coord, "replacement"}};
        auto replaced = alias.write_chunk_deltas(replacement);
        assert(!replaced && replaced.error().code == "save_database.busy");
        auto recovered = alias.recover_chunk_delta_journal();
        assert(!recovered && recovered.error().code == "save_database.busy");
        auto compacted_files = alias.compact_chunk_deltas();
        assert(!compacted_files && compacted_files.error().code == "save_database.busy");
        auto recovered_generations = alias.recover_staged_generations();
        assert(!recovered_generations &&
               recovered_generations.error().code == "save_database.busy");
        auto pruned = alias.prune_stale_generations(0);
        assert(!pruned && pruned.error().code == "save_database.busy");

        // Publishing a new immutable generation is safe while the old generation is pinned.
        assert(alias.write_snapshot(make_snapshot(11, {{new_coord, "new-generation"}})));
        assert(read_required(reader, old_coord) == "overlay");
        auto current = alias.read_chunk_delta(new_coord);
        assert(current && current.value().encoded_edit_delta == "new-generation");
    }

    assert(alias.prune_stale_generations(0));
    assert(!std::filesystem::exists(temporary.path() / "generations" / "generation_1"));
    auto checkpoint = alias.compact_chunk_delta_journal();
    assert(checkpoint && !checkpoint.value().compacted);
}

void test_parallel_instances_serialize_append_sequences() {
    TemporaryDirectory temporary("chunk_delta_journal_parallel_writers");
    heartstead::save::FileSaveDatabase database(temporary.path());
    assert(database.write_snapshot(make_snapshot(12, {})));

    constexpr std::size_t writer_count = 8;
    std::barrier start_line(static_cast<std::ptrdiff_t>(writer_count));
    std::atomic_size_t appended_count = 0;
    std::vector<std::jthread> threads;
    threads.reserve(writer_count);
    for (std::size_t index = 0; index < writer_count; ++index) {
        threads.emplace_back([&, index] {
            heartstead::save::FileSaveDatabase writer_database(temporary.path() / ".");
            start_line.arrive_and_wait();

            bool appended = false;
            for (std::size_t attempt = 0; attempt < 1'000 && !appended; ++attempt) {
                const auto coord =
                    heartstead::world::ChunkCoord{static_cast<std::int64_t>(100 + index), 0, 0};
                auto status =
                    writer_database.write_chunk_delta({coord, "parallel-" + std::to_string(index)});
                if (status) {
                    appended = true;
                    appended_count.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                assert(status.error().code == "save_database.busy" ||
                       status.error().code == "save_database.chunk_delta_writer_stale");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            assert(appended);
        });
    }
    threads.clear();
    assert(appended_count.load(std::memory_order_relaxed) == writer_count);

    {
        auto opened = database.open_chunk_delta_reader();
        assert(opened);
        auto reader = std::move(opened).value();
        assert(reader.stats().journal_entry_count == writer_count);
        assert(reader.stats().indexed_chunk_delta_count == writer_count);
        for (std::size_t index = 0; index < writer_count; ++index) {
            const auto coord =
                heartstead::world::ChunkCoord{static_cast<std::int64_t>(100 + index), 0, 0};
            assert(read_required(reader, coord) == "parallel-" + std::to_string(index));
        }
    }

    auto checkpoint = database.compact_chunk_delta_journal();
    assert(checkpoint && checkpoint.value().compacted);
    assert(checkpoint.value().merged_entry_count == writer_count);
    auto stats = database.stats();
    assert(stats && stats.value().chunk_delta_count == writer_count);
    assert(stats.value().chunk_delta_journal_entry_count == 0);
}

} // namespace

int main() {
    test_append_overlay_and_pinned_readers();
    test_bulk_export_validates_base_payloads_shadowed_by_the_journal();
    test_stale_writer_and_snapshot_authority_fail_closed();
    test_pending_snapshot_supersedes_corrupt_stale_chunk_journal_during_maintenance();
    test_checkpoint_and_recovery_are_restart_safe();
    test_finalized_corruption_fails_closed();
    test_legacy_snapshot_overlay_compacts_to_external_table();
    test_live_reader_gates_destructive_maintenance_across_instances();
    test_parallel_instances_serialize_append_sequences();
    return 0;
}
