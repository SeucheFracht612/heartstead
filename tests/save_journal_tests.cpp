#include "engine/save/save_database.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace {

[[nodiscard]] std::filesystem::path make_temp_root() {
    const auto parent = std::filesystem::temp_directory_path();
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
        const auto root = parent / ("heartstead_save_journal_" + std::to_string(nonce) + "_" +
                                    std::to_string(attempt));
        std::error_code error;
        if (std::filesystem::create_directory(root, error)) {
            return root;
        }
    }
    assert(false && "could not create save journal test directory");
    return {};
}

[[nodiscard]] heartstead::save::SaveSnapshot snapshot(std::uint64_t seed,
                                                      std::string encoded_delta) {
    heartstead::save::SaveSnapshot result;
    result.metadata.game_version = "0.1.0";
    result.metadata.world_seed = seed;
    result.metadata.enabled_mods.push_back({"base", "0.0.1", "hash"});
    result.chunk_edits.push_back({{1, -2, 3}, std::move(encoded_delta)});
    result.mod_states.push_back({"base", "journal_test", std::to_string(seed)});
    return result;
}

void cleanup(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    assert(!error);
}

void test_durable_entry_is_immediately_recoverable_then_compacts() {
    const auto root = make_temp_root();
    heartstead::save::FileSaveDatabase database(root);

    auto accepted = database.journal_snapshot(snapshot(11, "first"));
    assert(accepted);
    assert(accepted.value().sequence == 1);
    assert(accepted.value().encoded_bytes > 0);
    assert(!std::filesystem::exists(root / "current.txt"));

    auto stats = database.stats();
    assert(stats);
    assert(stats.value().journal_entry_count == 1);
    assert(stats.value().journal_checkpoint_sequence == 0);
    assert(stats.value().journal_highest_sequence == 1);
    assert(stats.value().journal_bytes > accepted.value().encoded_bytes);

    // A fresh database instance models process restart after durable acceptance but before the
    // background checkpoint. Normal reads recover directly from the journal without mutation.
    heartstead::save::FileSaveDatabase restarted(root);
    auto loaded = restarted.read_snapshot();
    assert(loaded);
    assert(loaded.value().metadata.world_seed == 11);
    assert(loaded.value().chunk_edits.front().encoded_edit_delta == "first");

    const auto abandoned = root / "journal" / "entry_00000000000000000002.hsj.tmp";
    {
        std::ofstream output(abandoned, std::ios::binary | std::ios::trunc);
        assert(output);
        output << "interrupted";
    }
    auto recovered = restarted.recover_snapshot_journal();
    assert(recovered);
    assert(recovered.value().changed());
    assert(recovered.value().discarded_temporary_entry_count == 1);
    assert(recovered.value().compaction.compacted);
    assert(recovered.value().compaction.compacted_sequence == 1);
    assert(recovered.value().compaction.removed_entry_count == 1);
    assert(!std::filesystem::exists(abandoned));
    assert(std::filesystem::exists(root / "current.txt"));

    stats = restarted.stats();
    assert(stats);
    assert(stats.value().journal_entry_count == 0);
    assert(stats.value().journal_checkpoint_sequence == 1);
    assert(stats.value().journal_highest_sequence == 1);
    assert(stats.value().active_generation == "generation_1");
    loaded = restarted.read_snapshot();
    assert(loaded && loaded.value().metadata.world_seed == 11);

    auto no_work = restarted.recover_snapshot_journal();
    assert(no_work);
    assert(!no_work.value().changed());
    cleanup(root);
}

void test_finalized_corruption_fails_closed() {
    const auto root = make_temp_root();
    heartstead::save::FileSaveDatabase database(root);
    assert(database.write_snapshot(snapshot(21, "base")));

    auto accepted = database.journal_snapshot(snapshot(22, "newer"));
    assert(accepted && accepted.value().sequence == 2);
    const auto entry = root / "journal" / "entry_00000000000000000002.hsj";
    assert(std::filesystem::exists(entry));
    {
        std::fstream stream(entry, std::ios::binary | std::ios::in | std::ios::out);
        assert(stream);
        stream.seekp(-1, std::ios::end);
        const char corrupted = '\xff';
        stream.write(&corrupted, 1);
        stream.close();
        assert(stream);
    }

    const auto loaded = database.read_snapshot();
    assert(!loaded);
    assert(loaded.error().code == "save_database.journal_checksum_mismatch");
    const auto compacted = database.compact_snapshot_journal();
    assert(!compacted);
    assert(compacted.error().code == "save_database.journal_checksum_mismatch");
    cleanup(root);
}

void test_invalid_snapshot_is_not_durably_accepted() {
    const auto root = make_temp_root();
    heartstead::save::FileSaveDatabase database(root);
    auto invalid = snapshot(31, "valid");
    invalid.chunk_edits.push_back({{9, 9, 9}, ""});
    const auto accepted = database.journal_snapshot(invalid);
    assert(!accepted);
    assert(accepted.error().code == "save_database.empty_chunk_delta");
    auto stats = database.stats();
    assert(stats);
    assert(stats.value().journal_entry_count == 0);
    assert(stats.value().journal_checkpoint_sequence == 0);
    cleanup(root);
}

} // namespace

int main() {
    test_durable_entry_is_immediately_recoverable_then_compacts();
    test_finalized_corruption_fails_closed();
    test_invalid_snapshot_is_not_durably_accepted();
    return 0;
}
