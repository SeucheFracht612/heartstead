#include "engine/save/save_binary_codec.hpp"
#include "engine/save/save_database.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(std::string_view label) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("heartstead_" + std::string(label) + "_" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
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

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    assert(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

[[nodiscard]] heartstead::save::SaveSnapshot
make_snapshot(std::uint64_t world_seed,
              std::vector<heartstead::save::ChunkEditSaveRecord> chunk_edits) {
    heartstead::save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "save-chunk-delta-reader-test";
    snapshot.metadata.world_seed = world_seed;
    snapshot.chunk_edits = std::move(chunk_edits);
    return snapshot;
}

void test_external_generation_index_is_opened_once() {
    TemporaryDirectory temporary("external_chunk_delta_reader");
    heartstead::save::FileSaveDatabase database(temporary.path());

    std::vector<heartstead::save::ChunkEditSaveRecord> generation_one_records;
    constexpr std::size_t record_count = 257;
    generation_one_records.reserve(record_count);
    for (std::size_t index = 0; index < record_count; ++index) {
        generation_one_records.push_back(
            {{static_cast<std::int64_t>(index) - 128, static_cast<std::int64_t>(index % 7U) - 3,
              static_cast<std::int64_t>(index % 11U) - 5},
             "generation-one-" + std::to_string(index)});
    }
    assert(database.write_snapshot(make_snapshot(1, generation_one_records)));

    auto opened = database.open_chunk_delta_reader();
    assert(opened);
    auto reader = std::move(opened).value();
    assert(reader.stats().storage_kind ==
           heartstead::save::FileChunkDeltaStorageKind::external_table);
    assert(reader.stats().active_generation == "generation_1");
    assert(reader.stats().indexed_chunk_delta_count == record_count);
    assert(reader.stats().selected_save_root == temporary.path() / "generations" / "generation_1");

    const auto verify_record = [&reader, &generation_one_records](std::size_t index) {
        auto loaded = reader.read_chunk_delta(generation_one_records[index].coord);
        return loaded && loaded.value().has_value() &&
               loaded.value()->encoded_edit_delta ==
                   generation_one_records[index].encoded_edit_delta;
    };
    assert(verify_record(0));
    assert(verify_record(record_count / 2U));
    assert(verify_record(record_count - 1U));
    auto missing = reader.read_chunk_delta({999, 999, 999});
    assert(missing && !missing.value().has_value());

    const auto index_path =
        temporary.path() / "generations" / "generation_1" / "chunks" / "index.txt";
    const auto valid_index = read_text(index_path);
    write_text(index_path, "invalid-index\n");
    assert(verify_record(record_count / 3U));
    auto rejected_reopen = database.open_chunk_delta_reader();
    assert(!rejected_reopen);
    assert(rejected_reopen.error().code == "save_database.invalid_chunk_index");
    write_text(index_path, valid_index);

    std::atomic<bool> concurrent_reads_passed = true;
    std::vector<std::thread> workers;
    constexpr std::size_t worker_count = 8;
    constexpr std::size_t reads_per_worker = 64;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, worker] {
            for (std::size_t iteration = 0; iteration < reads_per_worker; ++iteration) {
                const auto index = (worker * 31U + iteration * 17U) % record_count;
                if (!verify_record(index)) {
                    concurrent_reads_passed = false;
                    return;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    assert(concurrent_reads_passed.load());

    const std::vector<heartstead::save::ChunkEditSaveRecord> generation_two_records{
        {{600, -2, 9}, "generation-two"}};
    assert(database.write_snapshot(make_snapshot(2, generation_two_records)));
    assert(verify_record(record_count - 7U));

    auto current = database.open_chunk_delta_reader();
    assert(current);
    assert(current.value().stats().active_generation == "generation_2");
    assert(current.value().stats().indexed_chunk_delta_count == 1);
    auto old_coordinate = current.value().read_chunk_delta(generation_one_records.front().coord);
    assert(old_coordinate && !old_coordinate.value().has_value());

    heartstead::world::FileSaveChunkEditDeltaSource source(std::move(current).value());
    auto current_delta = source.read_chunk_delta(generation_two_records.front().coord);
    assert(current_delta && current_delta.value().has_value());
    assert(current_delta.value()->encoded_edit_delta == "generation-two");
    assert(source.stats().active_generation == "generation_2");
}

void test_inline_legacy_snapshot_is_pinned_in_memory() {
    TemporaryDirectory temporary("inline_chunk_delta_reader");
    const std::vector<heartstead::save::ChunkEditSaveRecord> records{{{-4, 2, 1}, "legacy-a"},
                                                                     {{9, -3, 7}, "legacy-b"}};
    auto encoded = heartstead::save::SaveBinaryCodec::encode_snapshot(make_snapshot(3, records));
    assert(encoded);
    write_bytes(temporary.path() / "snapshot.hssb", encoded.value());

    heartstead::save::FileSaveDatabase database(temporary.path());
    auto opened = database.open_chunk_delta_reader();
    assert(opened);
    auto reader = std::move(opened).value();
    assert(reader.stats().storage_kind ==
           heartstead::save::FileChunkDeltaStorageKind::inline_snapshot);
    assert(reader.stats().active_generation.empty());
    assert(reader.stats().indexed_chunk_delta_count == records.size());

    std::filesystem::remove(temporary.path() / "snapshot.hssb");
    auto loaded = reader.read_chunk_delta(records.back().coord);
    assert(loaded && loaded.value().has_value());
    assert(loaded.value()->encoded_edit_delta == "legacy-b");

    auto empty = database.open_chunk_delta_reader();
    assert(empty);
    assert(empty.value().stats().storage_kind == heartstead::save::FileChunkDeltaStorageKind::none);
    assert(empty.value().stats().indexed_chunk_delta_count == 0);
    auto missing = empty.value().read_chunk_delta(records.front().coord);
    assert(missing && !missing.value().has_value());

    auto duplicate_records = records;
    duplicate_records.push_back(records.front());
    encoded = heartstead::save::SaveBinaryCodec::encode_snapshot(
        make_snapshot(4, std::move(duplicate_records)));
    assert(encoded);
    write_bytes(temporary.path() / "snapshot.hssb", encoded.value());
    auto rejected_duplicate = database.open_chunk_delta_reader();
    assert(!rejected_duplicate);
    assert(rejected_duplicate.error().code == "save_database.duplicate_chunk_delta");
}

} // namespace

int main() {
    test_external_generation_index_is_opened_once();
    test_inline_legacy_snapshot_is_pinned_in_memory();
    return 0;
}
