#include "engine/save/save_database.hpp"

#include "engine/core/filesystem.hpp"
#include "engine/core/hash.hpp"
#include "engine/save/save_binary_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace heartstead::save {

namespace {

constexpr std::string_view chunk_index_magic = "heartstead.save_database_chunks.v1";
constexpr std::string_view current_generation_magic = "heartstead.save_database_current.v1";
constexpr std::string_view generation_prefix = "generation_";
constexpr std::size_t max_snapshot_file_bytes = 512U * 1024U * 1024U;
constexpr std::size_t max_chunk_index_file_bytes = 64U * 1024U * 1024U;
constexpr std::size_t max_generation_manifest_file_bytes = 64U * 1024U;
constexpr std::size_t max_chunk_delta_file_bytes = 16U * 1024U * 1024U;
constexpr std::size_t max_chunk_delta_table_bytes = 512U * 1024U * 1024U;
constexpr std::size_t max_chunk_delta_count = 1'000'000U;
constexpr std::string_view chunk_delta_journal_magic = "HSTDCDLT";
constexpr std::uint32_t chunk_delta_journal_version = 1;
constexpr std::string_view chunk_delta_journal_entry_prefix = "entry_";
constexpr std::string_view chunk_delta_journal_entry_suffix = ".hcdj";
constexpr std::size_t chunk_delta_journal_header_bytes = 8U + 4U + 8U + 8U + 8U + 8U + 8U + 8U;
constexpr std::size_t max_chunk_delta_journal_entry_count = 65'536U;
constexpr std::uintmax_t max_chunk_delta_journal_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::string_view journal_magic = "HSTDJRNL";
constexpr std::uint32_t journal_version = 1;
constexpr std::string_view journal_entry_prefix = "entry_";
constexpr std::string_view journal_entry_suffix = ".hsj";
constexpr std::string_view journal_checkpoint_magic = "heartstead.save_journal_checkpoint.v1";
constexpr std::size_t journal_header_bytes = 8U + 4U + 8U + 8U + 8U;
constexpr std::size_t max_journal_entry_count = 8U;
constexpr std::uintmax_t max_journal_bytes = 1024ULL * 1024ULL * 1024ULL;

struct ChunkIndexEntry {
    world::ChunkCoord coord;
    std::string filename;
};

struct GenerationDirectoryStats {
    std::size_t committed_count = 0;
    std::size_t staged_count = 0;
};

struct CommittedGenerationEntry {
    std::uint64_t number = 0;
    std::string name;
    std::filesystem::path path;
};

struct StagedGenerationEntry {
    std::uint64_t number = 0;
    std::filesystem::path path;
};

struct JournalEntry {
    std::uint64_t sequence = 0;
    std::filesystem::path path;
    std::uintmax_t bytes = 0;
};

struct JournalState {
    std::uint64_t checkpoint_sequence = 0;
    std::vector<JournalEntry> entries;
    std::uintmax_t bytes = 0;
};

struct ChunkDeltaJournalEntry {
    std::uint64_t sequence = 0;
    std::filesystem::path path;
    std::uintmax_t bytes = 0;
    world::ChunkCoord coord;
    std::size_t payload_bytes = 0;
    std::uint64_t expected_hash = 0;
};

struct ChunkDeltaJournalState {
    std::vector<ChunkDeltaJournalEntry> entries;
    std::uintmax_t bytes = 0;
};

[[nodiscard]] std::filesystem::path snapshot_path(const std::filesystem::path& root) {
    return root / "snapshot.hssb";
}

[[nodiscard]] std::filesystem::path generations_directory(const std::filesystem::path& root) {
    return root / "generations";
}

[[nodiscard]] std::filesystem::path current_generation_path(const std::filesystem::path& root) {
    return root / "current.txt";
}

[[nodiscard]] std::filesystem::path chunk_directory(const std::filesystem::path& root) {
    return root / "chunks";
}

[[nodiscard]] std::filesystem::path staged_chunk_directory(const std::filesystem::path& root) {
    return root / "chunks.tmp";
}

[[nodiscard]] std::filesystem::path backup_chunk_directory(const std::filesystem::path& root) {
    return root / "chunks.backup";
}

[[nodiscard]] std::filesystem::path
chunk_delta_journal_directory(const std::filesystem::path& root) {
    return root / "chunk_journal";
}

[[nodiscard]] std::filesystem::path
compacted_chunk_delta_journal_directory(const std::filesystem::path& root) {
    return root / "chunk_journal.compacted";
}

[[nodiscard]] std::filesystem::path journal_directory(const std::filesystem::path& root) {
    return root / "journal";
}

[[nodiscard]] std::filesystem::path journal_checkpoint_path(const std::filesystem::path& root) {
    return journal_directory(root) / "checkpoint.txt";
}

[[nodiscard]] std::string journal_entry_filename(std::uint64_t sequence) {
    std::ostringstream output;
    output << journal_entry_prefix << std::setw(20) << std::setfill('0') << sequence
           << journal_entry_suffix;
    return output.str();
}

[[nodiscard]] std::string chunk_delta_journal_entry_filename(std::uint64_t sequence) {
    std::ostringstream output;
    output << chunk_delta_journal_entry_prefix << std::setw(20) << std::setfill('0') << sequence
           << chunk_delta_journal_entry_suffix;
    return output.str();
}

[[nodiscard]] std::string chunk_filename(world::ChunkCoord coord) {
    return "c_" + std::to_string(coord.x) + "_" + std::to_string(coord.y) + "_" +
           std::to_string(coord.z) + ".delta";
}

[[nodiscard]] bool same_coord(world::ChunkCoord left, world::ChunkCoord right) noexcept {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

[[nodiscard]] bool is_generation_name(std::string_view name) noexcept {
    if (!name.starts_with(generation_prefix) || name.size() == generation_prefix.size()) {
        return false;
    }
    const auto digits = name.substr(generation_prefix.size());
    if (digits.front() < '1' || digits.front() > '9') {
        return false;
    }
    for (const auto character : digits) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_staged_generation_name(std::string_view name) noexcept {
    constexpr std::string_view staged_suffix = ".tmp";
    return name.ends_with(staged_suffix) &&
           is_generation_name(name.substr(0, name.size() - staged_suffix.size()));
}

[[nodiscard]] core::Result<std::uint64_t> parse_generation_number(std::string_view name) {
    if (!is_generation_name(name)) {
        return core::Result<std::uint64_t>::failure("save_database.invalid_generation_name",
                                                    "save generation name is invalid");
    }

    std::uint64_t parsed = 0;
    const auto digits = name.substr(generation_prefix.size());
    const auto* begin = digits.data();
    const auto* end = digits.data() + digits.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end || parsed == 0) {
        return core::Result<std::uint64_t>::failure("save_database.invalid_generation_name",
                                                    "save generation number is invalid");
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] std::string generation_name(std::uint64_t generation) {
    return std::string(generation_prefix) + std::to_string(generation);
}

[[nodiscard]] core::Status filesystem_failure(std::string code, const std::error_code& error) {
    return core::Status::failure(std::move(code), error.message());
}

[[nodiscard]] core::Result<std::filesystem::path>
readable_chunk_directory(const std::filesystem::path& root) {
    const auto target = chunk_directory(root);
    std::error_code error;
    const bool has_target = std::filesystem::exists(target, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure("save_database.read_failed",
                                                            error.message());
    }
    if (has_target) {
        return core::Result<std::filesystem::path>::success(target);
    }

    const auto backup = backup_chunk_directory(root);
    const bool has_backup = std::filesystem::exists(backup, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure("save_database.read_failed",
                                                            error.message());
    }
    return core::Result<std::filesystem::path>::success(has_backup ? backup : target);
}

[[nodiscard]] core::Status ensure_parent_directory(const std::filesystem::path& path) {
    const auto parent = path.parent_path();
    std::error_code error;
    const bool created = std::filesystem::create_directories(parent, error);
    if (error) {
        return filesystem_failure("save_database.create_directory_failed", error);
    }
    if (created) {
        if (auto flush_error = core::flush_directory_to_disk(parent)) {
            return filesystem_failure("save_database.flush_directory_failed", flush_error);
        }
        if (auto flush_error = core::flush_directory_to_disk(parent.parent_path())) {
            return filesystem_failure("save_database.flush_directory_failed", flush_error);
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status remove_tree(const std::filesystem::path& path, std::string code) {
    std::error_code error;
    std::filesystem::remove_all(path, error);
    if (error) {
        return filesystem_failure(std::move(code), error);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status rename_path(const std::filesystem::path& from,
                                       const std::filesystem::path& to, std::string code) {
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (error) {
        return filesystem_failure(std::move(code), error);
    }
    if (auto flush_error = core::flush_directory_to_disk(to.parent_path())) {
        return filesystem_failure(std::move(code), flush_error);
    }
    if (from.parent_path() != to.parent_path()) {
        if (auto flush_error = core::flush_directory_to_disk(from.parent_path())) {
            return filesystem_failure(std::move(code), flush_error);
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status write_bytes_atomic(const std::filesystem::path& path,
                                              std::span<const std::uint8_t> bytes) {
    auto status = ensure_parent_directory(path);
    if (!status) {
        return status;
    }

    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return core::Status::failure("save_database.write_failed",
                                         "failed to open save database file for writing: " +
                                             temporary);
        }
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            return core::Status::failure("save_database.write_failed",
                                         "failed to write save database file: " + temporary);
        }
    }

    const auto error = core::replace_file_durable(temporary, path);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return filesystem_failure("save_database.rename_failed", error);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status write_text_atomic(const std::filesystem::path& path,
                                             std::string_view text) {
    return write_bytes_atomic(
        path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()),
                                            text.size()));
}

[[nodiscard]] core::Result<std::vector<std::uint8_t>> read_bytes(const std::filesystem::path& path,
                                                                 std::size_t max_bytes,
                                                                 std::string_view too_large_code,
                                                                 std::string_view description) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            "save_database.read_failed", "failed to open save database file: " + path.string());
    }

    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            "save_database.read_failed", "failed to determine save database file size");
    }
    if (static_cast<std::uintmax_t>(end) > max_bytes) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            std::string(too_large_code),
            std::string(description) + " exceeds the configured safety limit");
    }
    input.seekg(0, std::ios::beg);
    if (!input) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            "save_database.read_failed", "failed to seek save database file: " + path.string());
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        return core::Result<std::vector<std::uint8_t>>::failure(
            "save_database.read_failed", "failed to read save database file: " + path.string());
    }
    return core::Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

void append_u32_le(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64_le(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

[[nodiscard]] std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        value |= static_cast<std::uint32_t>(bytes[offset + shift / 8U]) << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64_le(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::uint32_t shift = 0; shift < 64U; shift += 8U) {
        value |= static_cast<std::uint64_t>(bytes[offset + shift / 8U]) << shift;
    }
    return value;
}

[[nodiscard]] std::uint64_t chunk_coord_bits(std::int64_t value) noexcept {
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::int64_t chunk_coord_from_bits(std::uint64_t value) noexcept {
    return std::bit_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t
chunk_delta_journal_payload_hash(std::uint64_t sequence, world::ChunkCoord coord,
                                 std::span<const std::uint8_t> payload) noexcept {
    core::StableHash64 hash;
    hash.add_u64_le(sequence);
    hash.add_u64_le(chunk_coord_bits(coord.x));
    hash.add_u64_le(chunk_coord_bits(coord.y));
    hash.add_u64_le(chunk_coord_bits(coord.z));
    hash.add_bytes(payload);
    return hash.value();
}

[[nodiscard]] std::vector<std::uint8_t>
encode_chunk_delta_journal_entry(std::uint64_t sequence, const ChunkEditSaveRecord& chunk_delta) {
    const auto payload = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(chunk_delta.encoded_edit_delta.data()),
        chunk_delta.encoded_edit_delta.size());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(chunk_delta_journal_header_bytes + payload.size());
    for (const auto character : chunk_delta_journal_magic) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    append_u32_le(bytes, chunk_delta_journal_version);
    append_u64_le(bytes, sequence);
    append_u64_le(bytes, chunk_coord_bits(chunk_delta.coord.x));
    append_u64_le(bytes, chunk_coord_bits(chunk_delta.coord.y));
    append_u64_le(bytes, chunk_coord_bits(chunk_delta.coord.z));
    append_u64_le(bytes, static_cast<std::uint64_t>(payload.size()));
    append_u64_le(bytes, chunk_delta_journal_payload_hash(sequence, chunk_delta.coord, payload));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

[[nodiscard]] core::Result<ChunkDeltaJournalEntry>
decode_chunk_delta_journal_header(std::span<const std::uint8_t> header,
                                  std::uint64_t expected_sequence,
                                  const std::filesystem::path& path, std::uintmax_t file_bytes) {
    if (header.size() != chunk_delta_journal_header_bytes ||
        file_bytes < chunk_delta_journal_header_bytes) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.chunk_delta_journal_truncated",
            "chunk delta journal entry header is truncated");
    }
    for (std::size_t index = 0; index < chunk_delta_journal_magic.size(); ++index) {
        if (header[index] != static_cast<std::uint8_t>(chunk_delta_journal_magic[index])) {
            return core::Result<ChunkDeltaJournalEntry>::failure(
                "save_database.invalid_chunk_delta_journal_magic",
                "chunk delta journal entry magic is invalid");
        }
    }

    constexpr std::size_t version_offset = 8U;
    constexpr std::size_t sequence_offset = version_offset + 4U;
    constexpr std::size_t x_offset = sequence_offset + 8U;
    constexpr std::size_t y_offset = x_offset + 8U;
    constexpr std::size_t z_offset = y_offset + 8U;
    constexpr std::size_t size_offset = z_offset + 8U;
    constexpr std::size_t hash_offset = size_offset + 8U;
    const auto version = read_u32_le(header, version_offset);
    const auto sequence = read_u64_le(header, sequence_offset);
    const world::ChunkCoord coord{chunk_coord_from_bits(read_u64_le(header, x_offset)),
                                  chunk_coord_from_bits(read_u64_le(header, y_offset)),
                                  chunk_coord_from_bits(read_u64_le(header, z_offset))};
    const auto payload_size = read_u64_le(header, size_offset);
    const auto expected_hash = read_u64_le(header, hash_offset);
    if (version != chunk_delta_journal_version) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.unsupported_chunk_delta_journal_version",
            "chunk delta journal entry version is unsupported");
    }
    if (sequence == 0 || sequence != expected_sequence) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.invalid_chunk_delta_journal_sequence",
            "chunk delta journal sequence does not match its canonical filename");
    }
    if (payload_size == 0 || payload_size > max_chunk_delta_file_bytes ||
        payload_size != file_bytes - chunk_delta_journal_header_bytes) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.invalid_chunk_delta_journal_size",
            "chunk delta journal payload size is invalid");
    }

    return core::Result<ChunkDeltaJournalEntry>::success(
        {sequence, path, file_bytes, coord, static_cast<std::size_t>(payload_size), expected_hash});
}

[[nodiscard]] core::Result<ChunkDeltaJournalEntry>
read_chunk_delta_journal_header(const std::filesystem::path& path,
                                std::uint64_t expected_sequence) {
    std::error_code error;
    const auto file_bytes = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.chunk_delta_journal_read_failed", error.message());
    }
    if (file_bytes < chunk_delta_journal_header_bytes ||
        file_bytes > chunk_delta_journal_header_bytes + max_chunk_delta_file_bytes) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.invalid_chunk_delta_journal_size",
            "chunk delta journal entry exceeds its configured size bounds");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.chunk_delta_journal_read_failed",
            "failed to open chunk delta journal entry: " + path.string());
    }
    std::array<std::uint8_t, chunk_delta_journal_header_bytes> header{};
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!input) {
        return core::Result<ChunkDeltaJournalEntry>::failure(
            "save_database.chunk_delta_journal_read_failed",
            "failed to read chunk delta journal entry header: " + path.string());
    }
    return decode_chunk_delta_journal_header(header, expected_sequence, path, file_bytes);
}

[[nodiscard]] core::Result<ChunkEditSaveRecord>
read_chunk_delta_journal_payload(const ChunkDeltaJournalEntry& entry) {
    auto bytes = read_bytes(
        entry.path, chunk_delta_journal_header_bytes + max_chunk_delta_file_bytes,
        "save_database.chunk_delta_journal_entry_too_large", "chunk delta journal entry");
    if (!bytes) {
        return core::Result<ChunkEditSaveRecord>::failure(bytes.error().code,
                                                          bytes.error().message);
    }
    if (bytes.value().size() != entry.bytes) {
        return core::Result<ChunkEditSaveRecord>::failure(
            "save_database.invalid_chunk_delta_journal_size",
            "chunk delta journal entry size changed after its index was opened");
    }
    auto decoded = decode_chunk_delta_journal_header(
        std::span<const std::uint8_t>(bytes.value()).first(chunk_delta_journal_header_bytes),
        entry.sequence, entry.path, entry.bytes);
    if (!decoded) {
        return core::Result<ChunkEditSaveRecord>::failure(decoded.error().code,
                                                          decoded.error().message);
    }
    if (!same_coord(decoded.value().coord, entry.coord) ||
        decoded.value().expected_hash != entry.expected_hash) {
        return core::Result<ChunkEditSaveRecord>::failure(
            "save_database.chunk_delta_journal_header_changed",
            "chunk delta journal header changed after its index was opened");
    }
    const auto payload =
        std::span<const std::uint8_t>(bytes.value()).subspan(chunk_delta_journal_header_bytes);
    if (chunk_delta_journal_payload_hash(entry.sequence, entry.coord, payload) !=
        entry.expected_hash) {
        return core::Result<ChunkEditSaveRecord>::failure(
            "save_database.chunk_delta_journal_checksum_mismatch",
            "chunk delta journal payload checksum does not match");
    }
    return core::Result<ChunkEditSaveRecord>::success(
        {entry.coord, std::string(reinterpret_cast<const char*>(payload.data()), payload.size())});
}

[[nodiscard]] std::uint64_t journal_payload_hash(std::uint64_t sequence,
                                                 std::span<const std::uint8_t> payload) noexcept {
    core::StableHash64 hash;
    hash.add_u64_le(sequence);
    hash.add_bytes(payload);
    return hash.value();
}

[[nodiscard]] std::vector<std::uint8_t>
encode_journal_entry(std::uint64_t sequence, std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(journal_header_bytes + payload.size());
    for (const auto character : journal_magic) {
        bytes.push_back(static_cast<std::uint8_t>(character));
    }
    append_u32_le(bytes, journal_version);
    append_u64_le(bytes, sequence);
    append_u64_le(bytes, static_cast<std::uint64_t>(payload.size()));
    append_u64_le(bytes, journal_payload_hash(sequence, payload));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

[[nodiscard]] core::Result<SaveSnapshot> decode_journal_entry(std::span<const std::uint8_t> bytes,
                                                              std::uint64_t expected_sequence) {
    if (bytes.size() < journal_header_bytes) {
        return core::Result<SaveSnapshot>::failure("save_database.journal_truncated",
                                                   "save journal entry header is truncated");
    }
    for (std::size_t index = 0; index < journal_magic.size(); ++index) {
        if (bytes[index] != static_cast<std::uint8_t>(journal_magic[index])) {
            return core::Result<SaveSnapshot>::failure("save_database.invalid_journal_magic",
                                                       "save journal entry magic is invalid");
        }
    }
    constexpr std::size_t version_offset = 8U;
    constexpr std::size_t sequence_offset = version_offset + 4U;
    constexpr std::size_t size_offset = sequence_offset + 8U;
    constexpr std::size_t hash_offset = size_offset + 8U;
    const auto version = read_u32_le(bytes, version_offset);
    const auto sequence = read_u64_le(bytes, sequence_offset);
    const auto payload_size = read_u64_le(bytes, size_offset);
    const auto expected_hash = read_u64_le(bytes, hash_offset);
    if (version != journal_version) {
        return core::Result<SaveSnapshot>::failure("save_database.unsupported_journal_version",
                                                   "save journal entry version is unsupported");
    }
    if (sequence == 0 || sequence != expected_sequence) {
        return core::Result<SaveSnapshot>::failure(
            "save_database.invalid_journal_sequence",
            "save journal entry sequence does not match its canonical filename");
    }
    if (payload_size > max_snapshot_file_bytes ||
        payload_size != bytes.size() - journal_header_bytes) {
        return core::Result<SaveSnapshot>::failure("save_database.invalid_journal_size",
                                                   "save journal payload size is invalid");
    }
    const auto payload = bytes.subspan(journal_header_bytes);
    if (journal_payload_hash(sequence, payload) != expected_hash) {
        return core::Result<SaveSnapshot>::failure("save_database.journal_checksum_mismatch",
                                                   "save journal payload checksum does not match");
    }
    auto snapshot = SaveBinaryCodec::decode_snapshot(payload);
    if (!snapshot) {
        return core::Result<SaveSnapshot>::failure(snapshot.error().code, snapshot.error().message);
    }
    return snapshot;
}

[[nodiscard]] core::Result<std::uint64_t> parse_u64(std::string_view value, std::string_view code,
                                                    std::string_view description) {
    std::uint64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::uint64_t>::failure(std::string(code), std::string(description));
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint64_t>
parse_chunk_delta_journal_entry_filename(std::string_view name) {
    if (!name.starts_with(chunk_delta_journal_entry_prefix) ||
        !name.ends_with(chunk_delta_journal_entry_suffix)) {
        return core::Result<std::uint64_t>::failure(
            "save_database.invalid_chunk_delta_journal_filename",
            "chunk delta journal filename is invalid");
    }
    const auto digits = name.substr(chunk_delta_journal_entry_prefix.size(),
                                    name.size() - chunk_delta_journal_entry_prefix.size() -
                                        chunk_delta_journal_entry_suffix.size());
    if (digits.size() != 20U) {
        return core::Result<std::uint64_t>::failure(
            "save_database.invalid_chunk_delta_journal_filename",
            "chunk delta journal sequence width is invalid");
    }
    auto parsed = parse_u64(digits, "save_database.invalid_chunk_delta_journal_filename",
                            "chunk delta journal sequence is invalid");
    if (!parsed || parsed.value() == 0 ||
        chunk_delta_journal_entry_filename(parsed.value()) != name) {
        return core::Result<std::uint64_t>::failure(
            "save_database.invalid_chunk_delta_journal_filename",
            "chunk delta journal filename is not canonical");
    }
    return parsed;
}

[[nodiscard]] core::Result<ChunkDeltaJournalState>
read_chunk_delta_journal_state(const std::filesystem::path& save_root) {
    ChunkDeltaJournalState state;
    const auto directory = chunk_delta_journal_directory(save_root);
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        return core::Result<ChunkDeltaJournalState>::failure(
            "save_database.chunk_delta_journal_read_failed", error.message());
    }
    if (!exists) {
        return core::Result<ChunkDeltaJournalState>::success(std::move(state));
    }

    for (const auto& directory_entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            return core::Result<ChunkDeltaJournalState>::failure(
                "save_database.chunk_delta_journal_read_failed", error.message());
        }
        const auto name = directory_entry.path().filename().string();
        if (!std::string_view(name).ends_with(chunk_delta_journal_entry_suffix)) {
            continue;
        }
        const auto status = directory_entry.symlink_status(error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return core::Result<ChunkDeltaJournalState>::failure(
                "save_database.invalid_chunk_delta_journal_entry",
                error ? error.message() : "chunk delta journal entries must be regular files");
        }
        auto sequence = parse_chunk_delta_journal_entry_filename(name);
        if (!sequence) {
            return core::Result<ChunkDeltaJournalState>::failure(sequence.error().code,
                                                                 sequence.error().message);
        }
        auto entry = read_chunk_delta_journal_header(directory_entry.path(), sequence.value());
        if (!entry) {
            return core::Result<ChunkDeltaJournalState>::failure(entry.error().code,
                                                                 entry.error().message);
        }
        if (entry.value().bytes > max_chunk_delta_journal_bytes - state.bytes) {
            return core::Result<ChunkDeltaJournalState>::failure(
                "save_database.chunk_delta_journal_too_large",
                "chunk delta journal exceeds its byte budget");
        }
        state.bytes += entry.value().bytes;
        state.entries.push_back(std::move(entry).value());
        if (state.entries.size() > max_chunk_delta_journal_entry_count) {
            return core::Result<ChunkDeltaJournalState>::failure(
                "save_database.chunk_delta_journal_too_many_entries",
                "chunk delta journal exceeds its entry budget");
        }
    }
    if (error) {
        return core::Result<ChunkDeltaJournalState>::failure(
            "save_database.chunk_delta_journal_read_failed", error.message());
    }

    std::ranges::sort(state.entries,
                      [](const ChunkDeltaJournalEntry& left, const ChunkDeltaJournalEntry& right) {
                          return left.sequence < right.sequence;
                      });
    for (std::size_t index = 1; index < state.entries.size(); ++index) {
        if (state.entries[index - 1U].sequence == state.entries[index].sequence) {
            return core::Result<ChunkDeltaJournalState>::failure(
                "save_database.duplicate_chunk_delta_journal_sequence",
                "chunk delta journal sequence is duplicated");
        }
    }
    return core::Result<ChunkDeltaJournalState>::success(std::move(state));
}

[[nodiscard]] std::vector<ChunkDeltaJournalEntry>
latest_chunk_delta_journal_entries(const ChunkDeltaJournalState& state) {
    auto latest = state.entries;
    std::ranges::sort(latest,
                      [](const ChunkDeltaJournalEntry& left, const ChunkDeltaJournalEntry& right) {
                          if (left.coord != right.coord) {
                              return left.coord < right.coord;
                          }
                          return left.sequence > right.sequence;
                      });
    const auto duplicate_begin =
        std::unique(latest.begin(), latest.end(),
                    [](const ChunkDeltaJournalEntry& left, const ChunkDeltaJournalEntry& right) {
                        return same_coord(left.coord, right.coord);
                    });
    latest.erase(duplicate_begin, latest.end());
    return latest;
}

[[nodiscard]] core::Result<std::uint64_t>
read_journal_checkpoint(const std::filesystem::path& root) {
    const auto path = journal_checkpoint_path(root);
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return core::Result<std::uint64_t>::failure("save_database.journal_read_failed",
                                                    error.message());
    }
    if (!exists) {
        return core::Result<std::uint64_t>::success(0);
    }
    auto bytes =
        read_bytes(path, max_generation_manifest_file_bytes,
                   "save_database.journal_checkpoint_too_large", "save journal checkpoint");
    if (!bytes) {
        return core::Result<std::uint64_t>::failure(bytes.error().code, bytes.error().message);
    }
    const auto text =
        std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    bool saw_magic = false;
    bool saw_sequence = false;
    bool saw_end = false;
    std::uint64_t sequence = 0;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (saw_end) {
            if (!line.empty()) {
                return core::Result<std::uint64_t>::failure(
                    "save_database.invalid_journal_checkpoint",
                    "save journal checkpoint contains trailing data");
            }
        } else if (!saw_magic) {
            if (line != journal_checkpoint_magic) {
                return core::Result<std::uint64_t>::failure(
                    "save_database.invalid_journal_checkpoint",
                    "save journal checkpoint magic is invalid");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
        } else if (!line.empty()) {
            constexpr std::string_view sequence_prefix = "sequence|";
            const auto encoded_sequence = line.starts_with(sequence_prefix)
                                              ? line.substr(sequence_prefix.size())
                                              : std::string_view{};
            if (encoded_sequence.empty() || encoded_sequence.find('|') != std::string_view::npos ||
                saw_sequence) {
                return core::Result<std::uint64_t>::failure(
                    "save_database.invalid_journal_checkpoint",
                    "save journal checkpoint must contain one sequence row");
            }
            auto parsed = parse_u64(encoded_sequence, "save_database.invalid_journal_checkpoint",
                                    "save journal checkpoint sequence is invalid");
            if (!parsed) {
                return parsed;
            }
            sequence = parsed.value();
            saw_sequence = true;
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1U;
    }
    if (!saw_magic || !saw_sequence || !saw_end) {
        return core::Result<std::uint64_t>::failure("save_database.invalid_journal_checkpoint",
                                                    "save journal checkpoint is incomplete");
    }
    return core::Result<std::uint64_t>::success(sequence);
}

[[nodiscard]] core::Status write_journal_checkpoint(const std::filesystem::path& root,
                                                    std::uint64_t sequence) {
    std::ostringstream output;
    output << journal_checkpoint_magic << '\n';
    output << "sequence|" << sequence << '\n';
    output << "end\n";
    return write_text_atomic(journal_checkpoint_path(root), output.str());
}

[[nodiscard]] core::Result<std::uint64_t> parse_journal_entry_filename(std::string_view name) {
    if (!name.starts_with(journal_entry_prefix) || !name.ends_with(journal_entry_suffix)) {
        return core::Result<std::uint64_t>::failure("save_database.invalid_journal_filename",
                                                    "save journal filename is invalid");
    }
    const auto digits =
        name.substr(journal_entry_prefix.size(),
                    name.size() - journal_entry_prefix.size() - journal_entry_suffix.size());
    if (digits.size() != 20U) {
        return core::Result<std::uint64_t>::failure("save_database.invalid_journal_filename",
                                                    "save journal sequence width is invalid");
    }
    auto parsed = parse_u64(digits, "save_database.invalid_journal_filename",
                            "save journal filename sequence is invalid");
    if (!parsed || parsed.value() == 0 || journal_entry_filename(parsed.value()) != name) {
        return core::Result<std::uint64_t>::failure("save_database.invalid_journal_filename",
                                                    "save journal filename is not canonical");
    }
    return parsed;
}

[[nodiscard]] core::Result<JournalState> read_journal_state(const std::filesystem::path& root) {
    auto checkpoint = read_journal_checkpoint(root);
    if (!checkpoint) {
        return core::Result<JournalState>::failure(checkpoint.error().code,
                                                   checkpoint.error().message);
    }
    JournalState state;
    state.checkpoint_sequence = checkpoint.value();
    const auto directory = journal_directory(root);
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        return core::Result<JournalState>::failure("save_database.journal_read_failed",
                                                   error.message());
    }
    if (!exists) {
        return core::Result<JournalState>::success(std::move(state));
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) {
            return core::Result<JournalState>::failure("save_database.journal_read_failed",
                                                       error.message());
        }
        const auto name = entry.path().filename().string();
        if (!std::string_view(name).ends_with(journal_entry_suffix)) {
            continue;
        }
        const auto status = entry.symlink_status(error);
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return core::Result<JournalState>::failure(
                "save_database.invalid_journal_entry",
                error ? error.message() : "save journal entries must be regular files");
        }
        auto sequence = parse_journal_entry_filename(name);
        if (!sequence) {
            return core::Result<JournalState>::failure(sequence.error().code,
                                                       sequence.error().message);
        }
        const auto bytes = entry.file_size(error);
        if (error) {
            return core::Result<JournalState>::failure("save_database.journal_read_failed",
                                                       error.message());
        }
        if (bytes > journal_header_bytes + max_snapshot_file_bytes ||
            bytes > max_journal_bytes - state.bytes) {
            return core::Result<JournalState>::failure("save_database.journal_too_large",
                                                       "save journal exceeds its byte budget");
        }
        state.bytes += bytes;
        state.entries.push_back({sequence.value(), entry.path(), bytes});
        if (state.entries.size() > max_journal_entry_count) {
            return core::Result<JournalState>::failure("save_database.journal_too_many_entries",
                                                       "save journal exceeds its entry budget");
        }
    }
    if (error) {
        return core::Result<JournalState>::failure("save_database.journal_read_failed",
                                                   error.message());
    }
    std::ranges::sort(state.entries, [](const JournalEntry& left, const JournalEntry& right) {
        return left.sequence < right.sequence;
    });
    for (std::size_t index = 1; index < state.entries.size(); ++index) {
        if (state.entries[index - 1U].sequence == state.entries[index].sequence) {
            return core::Result<JournalState>::failure("save_database.duplicate_journal_sequence",
                                                       "save journal sequence is duplicated");
        }
    }
    return core::Result<JournalState>::success(std::move(state));
}

[[nodiscard]] const JournalEntry*
latest_pending_snapshot_entry(const JournalState& state) noexcept {
    const auto pending = std::ranges::find_if(
        state.entries.rbegin(), state.entries.rend(),
        [&state](const JournalEntry& entry) { return entry.sequence > state.checkpoint_sequence; });
    return pending == state.entries.rend() ? nullptr : &*pending;
}

[[nodiscard]] core::Result<SaveSnapshot> read_journal_snapshot(const JournalEntry& entry) {
    auto bytes = read_bytes(entry.path, max_snapshot_file_bytes + journal_header_bytes,
                            "save_database.journal_entry_too_large", "save journal entry");
    if (!bytes) {
        return core::Result<SaveSnapshot>::failure(bytes.error().code, bytes.error().message);
    }
    return decode_journal_entry(bytes.value(), entry.sequence);
}

[[nodiscard]] core::Result<std::int64_t> parse_i64(std::string_view value,
                                                   std::string_view field_name) {
    std::int64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::int64_t>::failure("save_database.invalid_chunk_index",
                                                   "invalid chunk index field: " +
                                                       std::string(field_name));
    }
    return core::Result<std::int64_t>::success(parsed);
}

template <std::size_t FieldCount>
[[nodiscard]] std::optional<std::array<std::string_view, FieldCount>>
split_exact(std::string_view value, char delimiter) {
    static_assert(FieldCount > 0);
    std::array<std::string_view, FieldCount> result;
    std::size_t start = 0;
    for (std::size_t index = 0; index + 1 < FieldCount; ++index) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        result[index] = value.substr(start, end - start);
        start = end + 1;
    }
    if (value.find(delimiter, start) != std::string_view::npos) {
        return std::nullopt;
    }
    result.back() = value.substr(start);
    return result;
}

[[nodiscard]] core::Result<std::vector<ChunkIndexEntry>>
read_chunk_index(const std::filesystem::path& root) {
    auto chunks = readable_chunk_directory(root);
    if (!chunks) {
        return core::Result<std::vector<ChunkIndexEntry>>::failure(chunks.error().code,
                                                                   chunks.error().message);
    }
    const auto path = chunks.value() / "index.txt";
    std::error_code filesystem_error;
    const bool has_index = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return core::Result<std::vector<ChunkIndexEntry>>::failure("save_database.read_failed",
                                                                   filesystem_error.message());
    }
    if (!has_index) {
        return core::Result<std::vector<ChunkIndexEntry>>::success({});
    }

    auto bytes = read_bytes(path, max_chunk_index_file_bytes, "save_database.chunk_index_too_large",
                            "chunk index");
    if (!bytes) {
        return core::Result<std::vector<ChunkIndexEntry>>::failure(bytes.error().code,
                                                                   bytes.error().message);
    }
    const auto text =
        std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    std::vector<ChunkIndexEntry> entries;
    std::unordered_set<std::string> seen_coordinates;
    std::unordered_set<std::string> seen_filenames;
    bool saw_magic = false;
    bool saw_end = false;

    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (saw_end) {
            if (!line.empty()) {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.invalid_chunk_index",
                    "chunk index contains data after its end marker");
            }
        } else if (!saw_magic) {
            if (line != chunk_index_magic) {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.invalid_chunk_index", "chunk index has invalid magic");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
        } else if (!line.empty()) {
            const auto fields = split_exact<5>(line, '|');
            if (!fields || fields->front() != "chunk") {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.invalid_chunk_index",
                    "chunk index row must be chunk|x|y|z|filename");
            }
            auto x = parse_i64((*fields)[1], "x");
            auto y = parse_i64((*fields)[2], "y");
            auto z = parse_i64((*fields)[3], "z");
            if (!x || !y || !z || (*fields)[4].empty() ||
                (*fields)[4].find('/') != std::string_view::npos ||
                (*fields)[4].find('\\') != std::string_view::npos) {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.invalid_chunk_index", "chunk index row contains invalid fields");
            }
            const world::ChunkCoord coord{x.value(), y.value(), z.value()};
            const auto canonical_filename = chunk_filename(coord);
            if (!seen_coordinates.insert(canonical_filename).second) {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.duplicate_chunk_coordinate",
                    "chunk index contains a duplicate chunk coordinate");
            }
            if (!seen_filenames.emplace((*fields)[4]).second) {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.duplicate_chunk_filename",
                    "chunk index contains a duplicate chunk filename");
            }
            if ((*fields)[4] != canonical_filename) {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.noncanonical_chunk_filename",
                    "chunk index filename does not match its chunk coordinate");
            }
            if (entries.size() == max_chunk_delta_count) {
                return core::Result<std::vector<ChunkIndexEntry>>::failure(
                    "save_database.too_many_chunk_deltas",
                    "chunk index exceeds the configured record limit");
            }
            entries.push_back({coord, std::string((*fields)[4])});
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!saw_magic || !saw_end) {
        return core::Result<std::vector<ChunkIndexEntry>>::failure(
            "save_database.incomplete_chunk_index", "chunk index is incomplete");
    }
    std::ranges::sort(entries, [](const ChunkIndexEntry& left, const ChunkIndexEntry& right) {
        if (left.coord.x != right.coord.x) {
            return left.coord.x < right.coord.x;
        }
        if (left.coord.y != right.coord.y) {
            return left.coord.y < right.coord.y;
        }
        return left.coord.z < right.coord.z;
    });
    return core::Result<std::vector<ChunkIndexEntry>>::success(std::move(entries));
}

[[nodiscard]] core::Status
write_chunk_index_to_directory(const std::filesystem::path& directory,
                               const std::vector<ChunkIndexEntry>& entries) {
    std::size_t encoded_size = chunk_index_magic.size() + 1U + std::string_view("end\n").size();
    for (const auto& entry : entries) {
        const auto x = std::to_string(entry.coord.x);
        const auto y = std::to_string(entry.coord.y);
        const auto z = std::to_string(entry.coord.z);
        constexpr std::size_t fixed_row_bytes = std::string_view("chunk||||\n").size();
        const auto row_size =
            fixed_row_bytes + x.size() + y.size() + z.size() + entry.filename.size();
        if (row_size > max_chunk_index_file_bytes - encoded_size) {
            return core::Status::failure("save_database.chunk_index_too_large",
                                         "chunk index exceeds the configured safety limit");
        }
        encoded_size += row_size;
    }

    std::ostringstream output;
    output << chunk_index_magic << '\n';
    for (const auto& entry : entries) {
        output << "chunk|" << entry.coord.x << '|' << entry.coord.y << '|' << entry.coord.z << '|'
               << entry.filename << '\n';
    }
    output << "end\n";
    return write_text_atomic(directory / "index.txt", output.str());
}

[[nodiscard]] core::Result<bool> has_external_chunk_table(const std::filesystem::path& root) {
    auto chunks = readable_chunk_directory(root);
    if (!chunks) {
        return core::Result<bool>::failure(chunks.error().code, chunks.error().message);
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(chunks.value() / "index.txt", error);
    if (error) {
        return core::Result<bool>::failure("save_database.read_failed", error.message());
    }
    return core::Result<bool>::success(exists);
}

[[nodiscard]] core::Status write_current_generation(const std::filesystem::path& root,
                                                    std::string_view name) {
    if (!is_generation_name(name)) {
        return core::Status::failure("save_database.invalid_generation_name",
                                     "save generation name is invalid");
    }

    std::ostringstream output;
    output << current_generation_magic << '\n';
    output << "active|" << name << '\n';
    output << "end\n";
    return write_text_atomic(current_generation_path(root), output.str());
}

[[nodiscard]] core::Result<std::string> read_current_generation(const std::filesystem::path& root) {
    auto bytes =
        read_bytes(current_generation_path(root), max_generation_manifest_file_bytes,
                   "save_database.generation_manifest_too_large", "save generation manifest");
    if (!bytes) {
        return core::Result<std::string>::failure(bytes.error().code, bytes.error().message);
    }

    const auto text =
        std::string_view(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    bool saw_magic = false;
    bool saw_end = false;
    std::string active_generation;

    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (saw_end) {
            if (!line.empty()) {
                return core::Result<std::string>::failure(
                    "save_database.invalid_generation_manifest",
                    "save generation manifest contains data after its end marker");
            }
        } else if (!saw_magic) {
            if (line != current_generation_magic) {
                return core::Result<std::string>::failure(
                    "save_database.invalid_generation_manifest",
                    "save generation manifest has invalid magic");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
        } else if (!line.empty()) {
            const auto fields = split_exact<2>(line, '|');
            if (!fields || fields->front() != "active") {
                return core::Result<std::string>::failure(
                    "save_database.invalid_generation_manifest",
                    "save generation manifest must contain active|generation_number");
            }
            auto generation_number = parse_generation_number((*fields)[1]);
            if (!generation_number) {
                return core::Result<std::string>::failure(
                    "save_database.invalid_generation_manifest",
                    "save generation manifest contains an invalid generation name");
            }
            if (!active_generation.empty()) {
                return core::Result<std::string>::failure(
                    "save_database.invalid_generation_manifest",
                    "save generation manifest declares more than one active generation");
            }
            active_generation = std::string((*fields)[1]);
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!saw_magic || !saw_end || active_generation.empty()) {
        return core::Result<std::string>::failure("save_database.incomplete_generation_manifest",
                                                  "save generation manifest is incomplete");
    }
    return core::Result<std::string>::success(std::move(active_generation));
}

[[nodiscard]] core::Result<std::filesystem::path>
active_save_root(const std::filesystem::path& root) {
    std::error_code error;
    const auto manifest = current_generation_path(root);
    const bool has_manifest = std::filesystem::exists(manifest, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure("save_database.read_failed",
                                                            error.message());
    }
    if (!has_manifest) {
        return core::Result<std::filesystem::path>::success(root);
    }

    auto active = read_current_generation(root);
    if (!active) {
        return core::Result<std::filesystem::path>::failure(active.error().code,
                                                            active.error().message);
    }

    auto generation_root = generations_directory(root) / active.value();
    const bool exists = std::filesystem::is_directory(generation_root, error);
    if (error) {
        return core::Result<std::filesystem::path>::failure("save_database.read_failed",
                                                            error.message());
    }
    if (!exists) {
        return core::Result<std::filesystem::path>::failure(
            "save_database.missing_generation", "active save generation directory is missing");
    }

    return core::Result<std::filesystem::path>::success(std::move(generation_root));
}

[[nodiscard]] core::Result<std::string> next_generation_name(const std::filesystem::path& root) {
    std::uint64_t highest_generation = 0;
    const auto generations = generations_directory(root);

    std::error_code error;
    const bool has_generations = std::filesystem::exists(generations, error);
    if (error) {
        return core::Result<std::string>::failure("save_database.read_failed", error.message());
    }
    if (!has_generations) {
        return core::Result<std::string>::success(generation_name(1));
    }

    for (const auto& entry : std::filesystem::directory_iterator(generations, error)) {
        if (error) {
            return core::Result<std::string>::failure("save_database.read_failed", error.message());
        }
        const bool is_directory = entry.is_directory(error);
        if (error) {
            return core::Result<std::string>::failure("save_database.read_failed", error.message());
        }
        if (!is_directory) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!is_generation_name(name)) {
            continue;
        }
        auto parsed = parse_generation_number(name);
        if (!parsed) {
            return core::Result<std::string>::failure(parsed.error().code, parsed.error().message);
        }
        highest_generation = std::max(highest_generation, parsed.value());
    }
    if (error) {
        return core::Result<std::string>::failure("save_database.read_failed", error.message());
    }

    if (highest_generation == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<std::string>::failure("save_database.generation_exhausted",
                                                  "save generation identifier range is exhausted");
    }
    return core::Result<std::string>::success(generation_name(highest_generation + 1));
}

[[nodiscard]] core::Result<GenerationDirectoryStats>
collect_generation_directory_stats(const std::filesystem::path& root) {
    GenerationDirectoryStats stats;
    const auto generations = generations_directory(root);

    std::error_code error;
    const bool has_generations = std::filesystem::exists(generations, error);
    if (error) {
        return core::Result<GenerationDirectoryStats>::failure("save_database.stats_failed",
                                                               error.message());
    }
    if (!has_generations) {
        return core::Result<GenerationDirectoryStats>::success(stats);
    }

    for (const auto& entry : std::filesystem::directory_iterator(generations, error)) {
        if (error) {
            return core::Result<GenerationDirectoryStats>::failure("save_database.stats_failed",
                                                                   error.message());
        }
        const bool is_directory = entry.is_directory(error);
        if (error) {
            return core::Result<GenerationDirectoryStats>::failure("save_database.stats_failed",
                                                                   error.message());
        }
        if (!is_directory) {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (is_generation_name(name)) {
            ++stats.committed_count;
        } else if (is_staged_generation_name(name)) {
            ++stats.staged_count;
        }
    }
    if (error) {
        return core::Result<GenerationDirectoryStats>::failure("save_database.stats_failed",
                                                               error.message());
    }

    return core::Result<GenerationDirectoryStats>::success(stats);
}

[[nodiscard]] core::Result<std::vector<CommittedGenerationEntry>>
collect_committed_generations(const std::filesystem::path& root) {
    std::vector<CommittedGenerationEntry> result;
    const auto generations = generations_directory(root);

    std::error_code error;
    const bool has_generations = std::filesystem::exists(generations, error);
    if (error) {
        return core::Result<std::vector<CommittedGenerationEntry>>::failure(
            "save_database.prune_failed", error.message());
    }
    if (!has_generations) {
        return core::Result<std::vector<CommittedGenerationEntry>>::success(std::move(result));
    }

    for (const auto& entry : std::filesystem::directory_iterator(generations, error)) {
        if (error) {
            return core::Result<std::vector<CommittedGenerationEntry>>::failure(
                "save_database.prune_failed", error.message());
        }
        const bool is_directory = entry.is_directory(error);
        if (error) {
            return core::Result<std::vector<CommittedGenerationEntry>>::failure(
                "save_database.prune_failed", error.message());
        }
        if (!is_directory) {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (!is_generation_name(name)) {
            continue;
        }
        auto parsed = parse_generation_number(name);
        if (!parsed) {
            return core::Result<std::vector<CommittedGenerationEntry>>::failure(
                parsed.error().code, parsed.error().message);
        }
        result.push_back({parsed.value(), name, entry.path()});
    }
    if (error) {
        return core::Result<std::vector<CommittedGenerationEntry>>::failure(
            "save_database.prune_failed", error.message());
    }

    return core::Result<std::vector<CommittedGenerationEntry>>::success(std::move(result));
}

[[nodiscard]] core::Result<std::vector<StagedGenerationEntry>>
collect_staged_generations(const std::filesystem::path& root) {
    std::vector<StagedGenerationEntry> result;
    const auto generations = generations_directory(root);

    std::error_code error;
    const bool has_generations = std::filesystem::exists(generations, error);
    if (error) {
        return core::Result<std::vector<StagedGenerationEntry>>::failure(
            "save_database.recover_failed", error.message());
    }
    if (!has_generations) {
        return core::Result<std::vector<StagedGenerationEntry>>::success(std::move(result));
    }

    constexpr std::string_view staged_suffix = ".tmp";
    for (const auto& entry : std::filesystem::directory_iterator(generations, error)) {
        if (error) {
            return core::Result<std::vector<StagedGenerationEntry>>::failure(
                "save_database.recover_failed", error.message());
        }
        const bool is_directory = entry.is_directory(error);
        if (error) {
            return core::Result<std::vector<StagedGenerationEntry>>::failure(
                "save_database.recover_failed", error.message());
        }
        if (!is_directory) {
            continue;
        }

        const auto name = entry.path().filename().string();
        if (!is_staged_generation_name(name)) {
            continue;
        }
        auto parsed = parse_generation_number(
            std::string_view(name).substr(0, name.size() - staged_suffix.size()));
        if (!parsed) {
            return core::Result<std::vector<StagedGenerationEntry>>::failure(
                parsed.error().code, parsed.error().message);
        }
        result.push_back({parsed.value(), entry.path()});
    }
    if (error) {
        return core::Result<std::vector<StagedGenerationEntry>>::failure(
            "save_database.recover_failed", error.message());
    }

    return core::Result<std::vector<StagedGenerationEntry>>::success(std::move(result));
}

[[nodiscard]] core::Result<std::string>
read_chunk_delta_payload(const std::filesystem::path& path,
                         std::size_t remaining_table_bytes = max_chunk_delta_table_bytes) {
    std::error_code error;
    const auto file_bytes = std::filesystem::file_size(path, error);
    if (error) {
        return core::Result<std::string>::failure("save_database.read_failed", error.message());
    }
    if (file_bytes > max_chunk_delta_file_bytes) {
        return core::Result<std::string>::failure(
            "save_database.chunk_delta_too_large",
            "chunk delta payload exceeds the configured safety limit");
    }
    if (file_bytes == 0) {
        return core::Result<std::string>::failure("save_database.empty_chunk_delta",
                                                  "chunk delta payload must not be empty");
    }
    if (file_bytes > remaining_table_bytes) {
        return core::Result<std::string>::failure(
            "save_database.chunk_delta_table_too_large",
            "chunk delta table exceeds the configured aggregate safety limit");
    }

    auto bytes = read_bytes(path, max_chunk_delta_file_bytes, "save_database.chunk_delta_too_large",
                            "chunk delta payload");
    if (!bytes) {
        return core::Result<std::string>::failure(bytes.error().code, bytes.error().message);
    }
    return core::Result<std::string>::success(
        std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
}

[[nodiscard]] core::Status write_chunk_delta_payload(const std::filesystem::path& path,
                                                     std::string_view payload) {
    return write_bytes_atomic(
        path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(payload.data()),
                                            payload.size()));
}

[[nodiscard]] core::Result<std::vector<ChunkEditSaveRecord>>
read_chunk_deltas_from_root(const std::filesystem::path& save_root) {
    auto chunks = readable_chunk_directory(save_root);
    if (!chunks) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(chunks.error().code,
                                                                       chunks.error().message);
    }
    auto entries = read_chunk_index(save_root);
    if (!entries) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(entries.error().code,
                                                                       entries.error().message);
    }

    std::vector<ChunkEditSaveRecord> result;
    result.reserve(entries.value().size());
    std::size_t total_payload_bytes = 0;
    for (const auto& entry : entries.value()) {
        auto payload = read_chunk_delta_payload(chunks.value() / entry.filename,
                                                max_chunk_delta_table_bytes - total_payload_bytes);
        if (!payload) {
            return core::Result<std::vector<ChunkEditSaveRecord>>::failure(payload.error().code,
                                                                           payload.error().message);
        }
        if (payload.value().size() > max_chunk_delta_table_bytes - total_payload_bytes) {
            return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
                "save_database.chunk_delta_table_too_large",
                "chunk delta table exceeds the configured aggregate safety limit");
        }
        total_payload_bytes += payload.value().size();
        result.push_back({entry.coord, std::move(payload).value()});
    }
    return core::Result<std::vector<ChunkEditSaveRecord>>::success(std::move(result));
}

[[nodiscard]] core::Status
validate_chunk_deltas_for_storage(std::span<const ChunkEditSaveRecord> chunk_deltas);

[[nodiscard]] core::Result<std::vector<ChunkEditSaveRecord>>
read_effective_chunk_deltas_from_root(const std::filesystem::path& save_root) {
    auto has_chunk_table = has_external_chunk_table(save_root);
    if (!has_chunk_table) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
            has_chunk_table.error().code, has_chunk_table.error().message);
    }
    std::vector<ChunkEditSaveRecord> base;
    if (has_chunk_table.value()) {
        auto loaded = read_chunk_deltas_from_root(save_root);
        if (!loaded) {
            return loaded;
        }
        base = std::move(loaded).value();
    } else {
        std::error_code error;
        const auto path = snapshot_path(save_root);
        const bool has_snapshot = std::filesystem::exists(path, error);
        if (error) {
            return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
                "save_database.read_failed", error.message());
        }
        if (has_snapshot) {
            auto bytes = read_bytes(path, max_snapshot_file_bytes,
                                    "save_database.snapshot_too_large", "binary save snapshot");
            if (!bytes) {
                return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
                    bytes.error().code, bytes.error().message);
            }
            auto snapshot = SaveBinaryCodec::decode_snapshot(bytes.value());
            if (!snapshot) {
                return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
                    snapshot.error().code, snapshot.error().message);
            }
            base = std::move(snapshot).value().chunk_edits;
        }
    }

    auto validation = validate_chunk_deltas_for_storage(base);
    if (!validation) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(validation.error().code,
                                                                       validation.error().message);
    }
    std::ranges::sort(base, [](const ChunkEditSaveRecord& left, const ChunkEditSaveRecord& right) {
        return left.coord < right.coord;
    });

    auto journal = read_chunk_delta_journal_state(save_root);
    if (!journal) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(journal.error().code,
                                                                       journal.error().message);
    }
    auto latest = latest_chunk_delta_journal_entries(journal.value());
    std::vector<ChunkEditSaveRecord> updates;
    updates.reserve(latest.size());
    for (const auto& entry : latest) {
        auto record = read_chunk_delta_journal_payload(entry);
        if (!record) {
            return core::Result<std::vector<ChunkEditSaveRecord>>::failure(record.error().code,
                                                                           record.error().message);
        }
        updates.push_back(std::move(record).value());
    }

    std::vector<ChunkEditSaveRecord> effective;
    effective.reserve(base.size() + updates.size());
    auto base_it = base.begin();
    auto update_it = updates.begin();
    while (base_it != base.end() || update_it != updates.end()) {
        if (update_it == updates.end() ||
            (base_it != base.end() && base_it->coord < update_it->coord)) {
            effective.push_back(std::move(*base_it));
            ++base_it;
        } else if (base_it == base.end() || update_it->coord < base_it->coord) {
            effective.push_back(std::move(*update_it));
            ++update_it;
        } else {
            effective.push_back(std::move(*update_it));
            ++base_it;
            ++update_it;
        }
    }

    validation = validate_chunk_deltas_for_storage(effective);
    if (!validation) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(validation.error().code,
                                                                       validation.error().message);
    }
    return core::Result<std::vector<ChunkEditSaveRecord>>::success(std::move(effective));
}

[[nodiscard]] core::Status
validate_chunk_deltas_for_storage(std::span<const ChunkEditSaveRecord> chunk_deltas) {
    if (chunk_deltas.size() > max_chunk_delta_count) {
        return core::Status::failure("save_database.too_many_chunk_deltas",
                                     "chunk delta table exceeds the configured record limit");
    }
    std::unordered_set<std::string> seen_coordinates;
    seen_coordinates.reserve(chunk_deltas.size());
    std::size_t total_payload_bytes = 0;
    for (const auto& chunk_delta : chunk_deltas) {
        if (chunk_delta.encoded_edit_delta.empty()) {
            return core::Status::failure("save_database.empty_chunk_delta",
                                         "chunk delta payload must not be empty");
        }
        if (chunk_delta.encoded_edit_delta.size() > max_chunk_delta_file_bytes) {
            return core::Status::failure("save_database.chunk_delta_too_large",
                                         "chunk delta payload exceeds the configured safety limit");
        }
        if (chunk_delta.encoded_edit_delta.size() >
            max_chunk_delta_table_bytes - total_payload_bytes) {
            return core::Status::failure(
                "save_database.chunk_delta_table_too_large",
                "chunk delta table exceeds the configured aggregate safety limit");
        }
        total_payload_bytes += chunk_delta.encoded_edit_delta.size();
        if (!seen_coordinates.insert(chunk_filename(chunk_delta.coord)).second) {
            return core::Status::failure(
                "save_database.duplicate_chunk_delta",
                "bulk chunk delta replacement contains a duplicate chunk coordinate");
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status
write_chunk_deltas_to_root(const std::filesystem::path& save_root,
                           std::span<const ChunkEditSaveRecord> chunk_deltas) {
    auto validation = validate_chunk_deltas_for_storage(chunk_deltas);
    if (!validation) {
        return validation;
    }

    std::vector<ChunkIndexEntry> entries;
    entries.reserve(chunk_deltas.size());
    std::unordered_set<std::string> seen_coordinates;
    seen_coordinates.reserve(chunk_deltas.size());

    std::size_t total_payload_bytes = 0;
    for (const auto& chunk_delta : chunk_deltas) {
        if (chunk_delta.encoded_edit_delta.empty()) {
            return core::Status::failure("save_database.empty_chunk_delta",
                                         "chunk delta payload must not be empty");
        }
        if (chunk_delta.encoded_edit_delta.size() > max_chunk_delta_file_bytes) {
            return core::Status::failure("save_database.chunk_delta_too_large",
                                         "chunk delta payload exceeds the configured safety limit");
        }
        if (chunk_delta.encoded_edit_delta.size() >
            max_chunk_delta_table_bytes - total_payload_bytes) {
            return core::Status::failure(
                "save_database.chunk_delta_table_too_large",
                "chunk delta table exceeds the configured aggregate safety limit");
        }
        total_payload_bytes += chunk_delta.encoded_edit_delta.size();
        const auto filename = chunk_filename(chunk_delta.coord);
        if (!seen_coordinates.insert(filename).second) {
            return core::Status::failure(
                "save_database.duplicate_chunk_delta",
                "bulk chunk delta replacement contains a duplicate chunk coordinate");
        }
        entries.push_back({chunk_delta.coord, filename});
    }

    std::ranges::sort(entries, [](const ChunkIndexEntry& left, const ChunkIndexEntry& right) {
        if (left.coord.x != right.coord.x) {
            return left.coord.x < right.coord.x;
        }
        if (left.coord.y != right.coord.y) {
            return left.coord.y < right.coord.y;
        }
        return left.coord.z < right.coord.z;
    });

    const auto target = chunk_directory(save_root);
    const auto staged = staged_chunk_directory(save_root);
    const auto backup = backup_chunk_directory(save_root);

    auto status = remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
    if (!status) {
        return status;
    }

    for (const auto& chunk_delta : chunk_deltas) {
        status = write_chunk_delta_payload(staged / chunk_filename(chunk_delta.coord),
                                           chunk_delta.encoded_edit_delta);
        if (!status) {
            (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
            return status;
        }
    }
    status = write_chunk_index_to_directory(staged, entries);
    if (!status) {
        (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
        return status;
    }

    std::error_code error;
    bool has_target = std::filesystem::exists(target, error);
    if (error) {
        (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
        return filesystem_failure("save_database.commit_chunk_table_failed", error);
    }
    const bool has_backup = std::filesystem::exists(backup, error);
    if (error) {
        (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
        return filesystem_failure("save_database.commit_chunk_table_failed", error);
    }

    if (!has_target && has_backup) {
        status = rename_path(backup, target, "save_database.recover_chunk_table_failed");
        if (!status) {
            (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
            return status;
        }
        has_target = true;
    } else if (has_backup) {
        status = remove_tree(backup, "save_database.remove_chunk_table_backup_failed");
        if (!status) {
            (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
            return status;
        }
    }

    if (has_target) {
        status = rename_path(target, backup, "save_database.stage_chunk_table_backup_failed");
        if (!status) {
            (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
            return status;
        }
    }

    status = rename_path(staged, target, "save_database.commit_chunk_table_failed");
    if (!status) {
        if (has_target) {
            const auto rollback =
                rename_path(backup, target, "save_database.rollback_chunk_table_failed");
            if (!rollback) {
                return core::Status::failure(rollback.error().code,
                                             status.error().message +
                                                 "; rollback failed: " + rollback.error().message);
            }
        }
        (void)remove_tree(staged, "save_database.remove_staged_chunk_table_failed");
        return status;
    }

    if (has_target) {
        (void)remove_tree(backup, "save_database.remove_chunk_table_backup_failed");
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status write_snapshot_generation(const std::filesystem::path& root,
                                                     const SaveSnapshot& snapshot) {
    auto generation = next_generation_name(root);
    if (!generation) {
        return core::Status::failure(generation.error().code, generation.error().message);
    }

    const auto staged_root = generations_directory(root) / (generation.value() + ".tmp");
    const auto committed_root = generations_directory(root) / generation.value();

    auto status = remove_tree(staged_root, "save_database.remove_staged_generation_failed");
    if (!status) {
        return status;
    }

    status = write_chunk_deltas_to_root(staged_root, snapshot.chunk_edits);
    if (!status) {
        (void)remove_tree(staged_root, "save_database.remove_staged_generation_failed");
        return status;
    }

    const auto encoded = SaveBinaryCodec::encode_snapshot(snapshot);
    if (!encoded) {
        (void)remove_tree(staged_root, "save_database.remove_staged_generation_failed");
        return core::Status::failure(encoded.error().code, encoded.error().message);
    }
    if (encoded.value().size() > max_snapshot_file_bytes) {
        (void)remove_tree(staged_root, "save_database.remove_staged_generation_failed");
        return core::Status::failure("save_database.snapshot_too_large",
                                     "binary save snapshot exceeds the configured safety limit");
    }
    status = write_bytes_atomic(snapshot_path(staged_root), encoded.value());
    if (!status) {
        (void)remove_tree(staged_root, "save_database.remove_staged_generation_failed");
        return status;
    }

    status = rename_path(staged_root, committed_root, "save_database.commit_generation_failed");
    if (!status) {
        (void)remove_tree(staged_root, "save_database.remove_staged_generation_failed");
        return status;
    }

    return write_current_generation(root, generation.value());
}

} // namespace

core::Result<std::optional<ChunkEditSaveRecord>>
FileChunkDeltaReader::read_chunk_delta(world::ChunkCoord coord) const {
    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), coord,
        [](const Entry& entry, world::ChunkCoord requested) { return entry.coord < requested; });
    if (found == entries_.end() || !same_coord(found->coord, coord)) {
        return core::Result<std::optional<ChunkEditSaveRecord>>::success(std::nullopt);
    }

    std::optional<ChunkEditSaveRecord> result;
    if (found->payload_kind == PayloadKind::external_file) {
        auto payload = read_chunk_delta_payload(found->path);
        if (!payload) {
            return core::Result<std::optional<ChunkEditSaveRecord>>::failure(
                payload.error().code, payload.error().message);
        }
        result.emplace(ChunkEditSaveRecord{found->coord, std::move(payload).value()});
    } else if (found->payload_kind == PayloadKind::journal_entry) {
        auto metadata = read_chunk_delta_journal_header(found->path, found->sequence);
        if (!metadata) {
            return core::Result<std::optional<ChunkEditSaveRecord>>::failure(
                metadata.error().code, metadata.error().message);
        }
        if (!same_coord(metadata.value().coord, found->coord)) {
            return core::Result<std::optional<ChunkEditSaveRecord>>::failure(
                "save_database.chunk_delta_journal_coord_changed",
                "chunk delta journal coordinate changed after its index was opened");
        }
        auto record = read_chunk_delta_journal_payload(metadata.value());
        if (!record) {
            return core::Result<std::optional<ChunkEditSaveRecord>>::failure(
                record.error().code, record.error().message);
        }
        result.emplace(std::move(record).value());
    } else {
        result.emplace(ChunkEditSaveRecord{found->coord, found->value});
    }
    return core::Result<std::optional<ChunkEditSaveRecord>>::success(std::move(result));
}

const FileChunkDeltaReaderStats& FileChunkDeltaReader::stats() const noexcept {
    return stats_;
}

core::Result<ChunkDeltaJournalReceipt>
FileChunkDeltaWriter::write_chunk_delta(const ChunkEditSaveRecord& chunk_delta) {
    auto snapshot_journal = read_journal_state(database_root_);
    if (!snapshot_journal) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(snapshot_journal.error().code,
                                                               snapshot_journal.error().message);
    }
    if (latest_pending_snapshot_entry(snapshot_journal.value()) != nullptr) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.snapshot_journal_pending",
            "compact the accepted snapshot journal before appending chunk deltas");
    }

    std::error_code manifest_error;
    const bool has_generation_manifest =
        std::filesystem::exists(current_generation_path(database_root_), manifest_error);
    if (manifest_error) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_writer_stale", manifest_error.message());
    }
    if (has_generation_manifest != !stats_.active_generation.empty()) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_writer_stale",
            "active save generation changed after this chunk delta writer was opened");
    }
    if (has_generation_manifest) {
        auto current_generation = read_current_generation(database_root_);
        if (!current_generation) {
            return core::Result<ChunkDeltaJournalReceipt>::failure(
                current_generation.error().code, current_generation.error().message);
        }
        if (current_generation.value() != stats_.active_generation) {
            return core::Result<ChunkDeltaJournalReceipt>::failure(
                "save_database.chunk_delta_writer_stale",
                "active save generation changed after this chunk delta writer was opened");
        }
    }

    if (chunk_delta.encoded_edit_delta.empty()) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.empty_chunk_delta", "chunk delta payload must not be empty");
    }
    if (chunk_delta.encoded_edit_delta.size() > max_chunk_delta_file_bytes) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_too_large",
            "chunk delta payload exceeds the configured safety limit");
    }
    if (stats_.journal_entry_count >= max_chunk_delta_journal_entry_count) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_journal_full",
            "chunk delta journal reached its entry budget and requires compaction");
    }

    const auto found = std::lower_bound(
        entries_.begin(), entries_.end(), chunk_delta.coord,
        [](const Entry& entry, world::ChunkCoord coord) { return entry.coord < coord; });
    const auto old_payload_bytes =
        found != entries_.end() && same_coord(found->coord, chunk_delta.coord)
            ? found->payload_bytes
            : 0U;
    if (old_payload_bytes > stats_.effective_payload_bytes) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_journal_state_invalid",
            "chunk delta writer payload accounting is invalid");
    }
    const auto payload_bytes_without_old = stats_.effective_payload_bytes - old_payload_bytes;
    if (chunk_delta.encoded_edit_delta.size() >
        max_chunk_delta_table_bytes - payload_bytes_without_old) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_table_too_large",
            "effective chunk delta table exceeds the configured aggregate safety limit");
    }
    if (old_payload_bytes == 0 && entries_.size() >= max_chunk_delta_count) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.too_many_chunk_deltas",
            "effective chunk delta table exceeds the configured record limit");
    }
    if (stats_.highest_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_journal_sequence_exhausted",
            "chunk delta journal sequence identifier range is exhausted");
    }

    const auto sequence = stats_.highest_sequence + 1U;
    auto encoded = encode_chunk_delta_journal_entry(sequence, chunk_delta);
    if (encoded.size() > max_chunk_delta_journal_bytes - stats_.journal_bytes) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_journal_full",
            "chunk delta journal reached its byte budget and requires compaction");
    }
    const auto path = chunk_delta_journal_directory(stats_.selected_save_root) /
                      chunk_delta_journal_entry_filename(sequence);
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_journal_write_failed", filesystem_error.message());
    }
    if (exists) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(
            "save_database.chunk_delta_writer_stale",
            "chunk delta journal advanced after this writer was opened");
    }
    auto status = write_bytes_atomic(path, encoded);
    if (!status) {
        return core::Result<ChunkDeltaJournalReceipt>::failure(status.error().code,
                                                               status.error().message);
    }

    if (old_payload_bytes == 0) {
        entries_.insert(found, {chunk_delta.coord, chunk_delta.encoded_edit_delta.size()});
    } else {
        found->payload_bytes = chunk_delta.encoded_edit_delta.size();
    }
    stats_.effective_chunk_delta_count = entries_.size();
    stats_.effective_payload_bytes =
        payload_bytes_without_old + chunk_delta.encoded_edit_delta.size();
    ++stats_.journal_entry_count;
    stats_.journal_bytes += encoded.size();
    stats_.highest_sequence = sequence;
    return core::Result<ChunkDeltaJournalReceipt>::success({sequence, encoded.size()});
}

const FileChunkDeltaWriterStats& FileChunkDeltaWriter::stats() const noexcept {
    return stats_;
}

FileSaveDatabase::FileSaveDatabase(std::filesystem::path root) : root_(std::move(root)) {}

bool SaveDatabaseMaintenanceResult::changed() const noexcept {
    return recovered_staged_generation_count > 0 || pruned_stale_generation_count > 0 ||
           compacted_chunk_delta_count > 0 || chunk_delta_journal_compaction.compacted ||
           chunk_delta_journal_recovery.changed() || journal_recovery.changed();
}

bool SaveDatabaseMigrationResult::changed() const noexcept {
    return wrote_snapshot || !migration.applied_migrations.empty();
}

bool SaveJournalRecoveryResult::changed() const noexcept {
    return discarded_temporary_entry_count > 0 || compaction.compacted;
}

bool ChunkDeltaJournalRecoveryResult::changed() const noexcept {
    return discarded_temporary_entry_count > 0 || discarded_compacted_directory;
}

const std::filesystem::path& FileSaveDatabase::root() const noexcept {
    return root_;
}

core::Status FileSaveDatabase::write_snapshot(const SaveSnapshot& snapshot) const {
    auto accepted = journal_snapshot(snapshot);
    if (!accepted) {
        return core::Status::failure(accepted.error().code, accepted.error().message);
    }
    auto compacted = compact_snapshot_journal();
    if (!compacted) {
        return core::Status::failure(compacted.error().code, compacted.error().message);
    }
    if (!compacted.value().compacted ||
        compacted.value().compacted_sequence < accepted.value().sequence) {
        return core::Status::failure(
            "save_database.journal_compaction_incomplete",
            "durably accepted save journal entry was not selected by compaction");
    }
    return core::Status::ok();
}

core::Result<SaveSnapshot> FileSaveDatabase::read_snapshot() const {
    auto journal = read_journal_state(root_);
    if (!journal) {
        return core::Result<SaveSnapshot>::failure(journal.error().code, journal.error().message);
    }
    if (const auto* pending = latest_pending_snapshot_entry(journal.value())) {
        return read_journal_snapshot(*pending);
    }

    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<SaveSnapshot>::failure(save_root.error().code,
                                                   save_root.error().message);
    }

    auto bytes = read_bytes(snapshot_path(save_root.value()), max_snapshot_file_bytes,
                            "save_database.snapshot_too_large", "binary save snapshot");
    if (!bytes) {
        return core::Result<SaveSnapshot>::failure(bytes.error().code, bytes.error().message);
    }

    auto snapshot = SaveBinaryCodec::decode_snapshot(bytes.value());
    if (!snapshot) {
        return core::Result<SaveSnapshot>::failure(snapshot.error().code, snapshot.error().message);
    }

    auto chunk_deltas = read_effective_chunk_deltas_from_root(save_root.value());
    if (!chunk_deltas) {
        return core::Result<SaveSnapshot>::failure(chunk_deltas.error().code,
                                                   chunk_deltas.error().message);
    }
    snapshot.value().chunk_edits = std::move(chunk_deltas).value();

    return snapshot;
}

core::Result<SaveJournalReceipt>
FileSaveDatabase::journal_snapshot(const SaveSnapshot& snapshot) const {
    auto validation = validate_chunk_deltas_for_storage(snapshot.chunk_edits);
    if (!validation) {
        return core::Result<SaveJournalReceipt>::failure(validation.error().code,
                                                         validation.error().message);
    }
    auto encoded = SaveBinaryCodec::encode_snapshot(snapshot);
    if (!encoded) {
        return core::Result<SaveJournalReceipt>::failure(encoded.error().code,
                                                         encoded.error().message);
    }
    if (encoded.value().size() > max_snapshot_file_bytes) {
        return core::Result<SaveJournalReceipt>::failure(
            "save_database.snapshot_too_large",
            "binary save snapshot exceeds the configured safety limit");
    }

    auto state = read_journal_state(root_);
    if (!state) {
        return core::Result<SaveJournalReceipt>::failure(state.error().code, state.error().message);
    }
    if (state.value().entries.size() >= max_journal_entry_count) {
        return core::Result<SaveJournalReceipt>::failure(
            "save_database.journal_full",
            "save journal reached its entry budget and requires compaction");
    }
    const auto entry_bytes = journal_header_bytes + encoded.value().size();
    if (entry_bytes > max_journal_bytes - state.value().bytes) {
        return core::Result<SaveJournalReceipt>::failure(
            "save_database.journal_full",
            "save journal reached its byte budget and requires compaction");
    }

    std::uint64_t highest_sequence = state.value().checkpoint_sequence;
    if (!state.value().entries.empty()) {
        highest_sequence = std::max(highest_sequence, state.value().entries.back().sequence);
    }
    if (highest_sequence == std::numeric_limits<std::uint64_t>::max()) {
        return core::Result<SaveJournalReceipt>::failure(
            "save_database.journal_sequence_exhausted",
            "save journal sequence identifier range is exhausted");
    }
    const auto sequence = highest_sequence + 1U;
    const auto bytes = encode_journal_entry(sequence, encoded.value());
    auto status =
        write_bytes_atomic(journal_directory(root_) / journal_entry_filename(sequence), bytes);
    if (!status) {
        return core::Result<SaveJournalReceipt>::failure(status.error().code,
                                                         status.error().message);
    }
    return core::Result<SaveJournalReceipt>::success({sequence, encoded.value().size()});
}

core::Result<SaveJournalCompactionResult> FileSaveDatabase::compact_snapshot_journal() const {
    auto state = read_journal_state(root_);
    if (!state) {
        return core::Result<SaveJournalCompactionResult>::failure(state.error().code,
                                                                  state.error().message);
    }
    const auto* pending = latest_pending_snapshot_entry(state.value());

    SaveJournalCompactionResult result;
    if (pending != nullptr) {
        auto snapshot = read_journal_snapshot(*pending);
        if (!snapshot) {
            return core::Result<SaveJournalCompactionResult>::failure(snapshot.error().code,
                                                                      snapshot.error().message);
        }
        auto status = write_snapshot_generation(root_, snapshot.value());
        if (!status) {
            return core::Result<SaveJournalCompactionResult>::failure(status.error().code,
                                                                      status.error().message);
        }
        status = write_journal_checkpoint(root_, pending->sequence);
        if (!status) {
            return core::Result<SaveJournalCompactionResult>::failure(status.error().code,
                                                                      status.error().message);
        }
        result.compacted = true;
        result.compacted_sequence = pending->sequence;
    }

    const auto removable_sequence =
        result.compacted ? result.compacted_sequence : state.value().checkpoint_sequence;
    std::error_code error;
    for (const auto& entry : state.value().entries) {
        if (entry.sequence > removable_sequence) {
            continue;
        }
        const bool removed = std::filesystem::remove(entry.path, error);
        if (error) {
            return core::Result<SaveJournalCompactionResult>::failure(
                "save_database.journal_cleanup_failed", error.message());
        }
        if (removed) {
            ++result.removed_entry_count;
        }
    }
    if (result.removed_entry_count > 0) {
        if (auto flush_error = core::flush_directory_to_disk(journal_directory(root_))) {
            return core::Result<SaveJournalCompactionResult>::failure(
                "save_database.journal_cleanup_failed", flush_error.message());
        }
    }
    return core::Result<SaveJournalCompactionResult>::success(std::move(result));
}

core::Result<SaveJournalRecoveryResult> FileSaveDatabase::recover_snapshot_journal() const {
    SaveJournalRecoveryResult result;
    const auto directory = journal_directory(root_);
    std::error_code error;
    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        return core::Result<SaveJournalRecoveryResult>::failure(
            "save_database.journal_recovery_failed", error.message());
    }
    if (exists) {
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                return core::Result<SaveJournalRecoveryResult>::failure(
                    "save_database.journal_recovery_failed", error.message());
            }
            const auto name = entry.path().filename().string();
            const bool owned_temporary =
                name == "checkpoint.txt.tmp" ||
                (std::string_view(name).starts_with(journal_entry_prefix) &&
                 std::string_view(name).ends_with(".hsj.tmp"));
            if (!owned_temporary) {
                continue;
            }
            const bool removed = std::filesystem::remove(entry.path(), error);
            if (error) {
                return core::Result<SaveJournalRecoveryResult>::failure(
                    "save_database.journal_recovery_failed", error.message());
            }
            if (removed) {
                ++result.discarded_temporary_entry_count;
            }
        }
        if (result.discarded_temporary_entry_count > 0) {
            if (auto flush_error = core::flush_directory_to_disk(directory)) {
                return core::Result<SaveJournalRecoveryResult>::failure(
                    "save_database.journal_recovery_failed", flush_error.message());
            }
        }
    }

    auto compaction = compact_snapshot_journal();
    if (!compaction) {
        return core::Result<SaveJournalRecoveryResult>::failure(compaction.error().code,
                                                                compaction.error().message);
    }
    result.compaction = std::move(compaction).value();
    return core::Result<SaveJournalRecoveryResult>::success(std::move(result));
}

core::Result<SaveSnapshot>
FileSaveDatabase::read_validated_snapshot(const modding::PrototypeRegistry& prototypes) const {
    auto snapshot = read_snapshot();
    if (!snapshot) {
        return core::Result<SaveSnapshot>::failure(snapshot.error().code, snapshot.error().message);
    }

    const auto validation = SaveSnapshotValidator::validate(snapshot.value(), prototypes);
    if (!validation.valid()) {
        const auto& first_issue = validation.issues.front();
        return core::Result<SaveSnapshot>::failure(first_issue.code, first_issue.message);
    }

    return snapshot;
}

core::Status FileSaveDatabase::write_chunk_delta(const ChunkEditSaveRecord& chunk_delta) const {
    auto writer = open_chunk_delta_writer();
    if (!writer) {
        return core::Status::failure(writer.error().code, writer.error().message);
    }
    auto receipt = writer.value().write_chunk_delta(chunk_delta);
    if (!receipt) {
        return core::Status::failure(receipt.error().code, receipt.error().message);
    }
    return core::Status::ok();
}

core::Status
FileSaveDatabase::write_chunk_deltas(std::span<const ChunkEditSaveRecord> chunk_deltas) const {
    auto snapshot_journal = read_journal_state(root_);
    if (!snapshot_journal) {
        return core::Status::failure(snapshot_journal.error().code,
                                     snapshot_journal.error().message);
    }
    if (latest_pending_snapshot_entry(snapshot_journal.value()) != nullptr) {
        return core::Status::failure(
            "save_database.snapshot_journal_pending",
            "compact the accepted snapshot journal before replacing chunk deltas");
    }

    auto compacted = compact_chunk_delta_journal();
    if (!compacted) {
        return core::Status::failure(compacted.error().code, compacted.error().message);
    }
    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Status::failure(save_root.error().code, save_root.error().message);
    }
    return write_chunk_deltas_to_root(save_root.value(), chunk_deltas);
}

core::Result<FileChunkDeltaReader> FileSaveDatabase::open_chunk_delta_reader() const {
    auto snapshot_journal = read_journal_state(root_);
    if (!snapshot_journal) {
        return core::Result<FileChunkDeltaReader>::failure(snapshot_journal.error().code,
                                                           snapshot_journal.error().message);
    }
    const auto* pending_snapshot = latest_pending_snapshot_entry(snapshot_journal.value());
    if (pending_snapshot != nullptr) {
        auto snapshot = read_journal_snapshot(*pending_snapshot);
        if (!snapshot) {
            return core::Result<FileChunkDeltaReader>::failure(snapshot.error().code,
                                                               snapshot.error().message);
        }
        auto validation = validate_chunk_deltas_for_storage(snapshot.value().chunk_edits);
        if (!validation) {
            return core::Result<FileChunkDeltaReader>::failure(validation.error().code,
                                                               validation.error().message);
        }
        std::ranges::sort(snapshot.value().chunk_edits,
                          [](const ChunkEditSaveRecord& left, const ChunkEditSaveRecord& right) {
                              return left.coord < right.coord;
                          });

        FileChunkDeltaReader reader;
        reader.stats_.storage_kind = FileChunkDeltaStorageKind::inline_snapshot;
        reader.stats_.selected_save_root = root_;
        reader.stats_.base_indexed_chunk_delta_count = snapshot.value().chunk_edits.size();
        reader.stats_.indexed_chunk_delta_count = snapshot.value().chunk_edits.size();
        reader.entries_.reserve(snapshot.value().chunk_edits.size());
        for (auto& record : snapshot.value().chunk_edits) {
            reader.entries_.push_back({record.coord,
                                       FileChunkDeltaReader::PayloadKind::inline_payload,
                                       {},
                                       std::move(record.encoded_edit_delta),
                                       0});
        }
        return core::Result<FileChunkDeltaReader>::success(std::move(reader));
    }

    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<FileChunkDeltaReader>::failure(save_root.error().code,
                                                           save_root.error().message);
    }

    FileChunkDeltaReader reader;
    reader.stats_.selected_save_root = save_root.value();
    if (save_root.value().parent_path() == generations_directory(root_)) {
        reader.stats_.active_generation = save_root.value().filename().string();
    }

    auto has_chunk_table = has_external_chunk_table(save_root.value());
    if (!has_chunk_table) {
        return core::Result<FileChunkDeltaReader>::failure(has_chunk_table.error().code,
                                                           has_chunk_table.error().message);
    }
    if (has_chunk_table.value()) {
        auto chunks = readable_chunk_directory(save_root.value());
        if (!chunks) {
            return core::Result<FileChunkDeltaReader>::failure(chunks.error().code,
                                                               chunks.error().message);
        }
        auto entries = read_chunk_index(save_root.value());
        if (!entries) {
            return core::Result<FileChunkDeltaReader>::failure(entries.error().code,
                                                               entries.error().message);
        }

        reader.stats_.storage_kind = FileChunkDeltaStorageKind::external_table;
        reader.entries_.reserve(entries.value().size());
        for (auto& entry : entries.value()) {
            reader.entries_.push_back({entry.coord,
                                       FileChunkDeltaReader::PayloadKind::external_file,
                                       chunks.value() / entry.filename,
                                       {},
                                       0});
        }
    } else {
        const auto path = snapshot_path(save_root.value());
        std::error_code filesystem_error;
        const bool has_snapshot = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            return core::Result<FileChunkDeltaReader>::failure("save_database.read_failed",
                                                               filesystem_error.message());
        }
        if (has_snapshot) {
            auto bytes = read_bytes(path, max_snapshot_file_bytes,
                                    "save_database.snapshot_too_large", "binary save snapshot");
            if (!bytes) {
                return core::Result<FileChunkDeltaReader>::failure(bytes.error().code,
                                                                   bytes.error().message);
            }
            auto snapshot = SaveBinaryCodec::decode_snapshot(bytes.value());
            if (!snapshot) {
                return core::Result<FileChunkDeltaReader>::failure(snapshot.error().code,
                                                                   snapshot.error().message);
            }
            auto& chunk_deltas = snapshot.value().chunk_edits;
            const auto validation = validate_chunk_deltas_for_storage(chunk_deltas);
            if (!validation) {
                return core::Result<FileChunkDeltaReader>::failure(validation.error().code,
                                                                   validation.error().message);
            }
            std::ranges::sort(chunk_deltas, [](const ChunkEditSaveRecord& left,
                                               const ChunkEditSaveRecord& right) {
                return left.coord < right.coord;
            });
            reader.stats_.storage_kind = FileChunkDeltaStorageKind::inline_snapshot;
            reader.entries_.reserve(chunk_deltas.size());
            for (auto& chunk_delta : chunk_deltas) {
                reader.entries_.push_back({chunk_delta.coord,
                                           FileChunkDeltaReader::PayloadKind::inline_payload,
                                           {},
                                           std::move(chunk_delta.encoded_edit_delta),
                                           0});
            }
        }
    }

    reader.stats_.base_indexed_chunk_delta_count = reader.entries_.size();
    auto journal = read_chunk_delta_journal_state(save_root.value());
    if (!journal) {
        return core::Result<FileChunkDeltaReader>::failure(journal.error().code,
                                                           journal.error().message);
    }
    for (const auto& entry : journal.value().entries) {
        auto validated = read_chunk_delta_journal_payload(entry);
        if (!validated) {
            return core::Result<FileChunkDeltaReader>::failure(validated.error().code,
                                                               validated.error().message);
        }
    }
    reader.stats_.journal_entry_count = journal.value().entries.size();
    auto latest = latest_chunk_delta_journal_entries(journal.value());
    std::vector<FileChunkDeltaReader::Entry> updates;
    updates.reserve(latest.size());
    for (auto& entry : latest) {
        updates.push_back({entry.coord,
                           FileChunkDeltaReader::PayloadKind::journal_entry,
                           std::move(entry.path),
                           {},
                           entry.sequence});
    }

    std::vector<FileChunkDeltaReader::Entry> effective;
    effective.reserve(reader.entries_.size() + updates.size());
    auto base_it = std::make_move_iterator(reader.entries_.begin());
    const auto base_end = std::make_move_iterator(reader.entries_.end());
    auto update_it = std::make_move_iterator(updates.begin());
    const auto update_end = std::make_move_iterator(updates.end());
    while (base_it != base_end || update_it != update_end) {
        if (update_it == update_end || (base_it != base_end && base_it->coord < update_it->coord)) {
            effective.push_back(std::move(*base_it));
            ++base_it;
        } else if (base_it == base_end || update_it->coord < base_it->coord) {
            effective.push_back(std::move(*update_it));
            ++update_it;
        } else {
            effective.push_back(std::move(*update_it));
            ++base_it;
            ++update_it;
        }
    }
    if (effective.size() > max_chunk_delta_count) {
        return core::Result<FileChunkDeltaReader>::failure(
            "save_database.too_many_chunk_deltas",
            "effective chunk delta table exceeds the configured record limit");
    }
    reader.entries_ = std::move(effective);
    reader.stats_.indexed_chunk_delta_count = reader.entries_.size();
    return core::Result<FileChunkDeltaReader>::success(std::move(reader));
}

core::Result<FileChunkDeltaWriter> FileSaveDatabase::open_chunk_delta_writer() const {
    auto snapshot_journal = read_journal_state(root_);
    if (!snapshot_journal) {
        return core::Result<FileChunkDeltaWriter>::failure(snapshot_journal.error().code,
                                                           snapshot_journal.error().message);
    }
    if (latest_pending_snapshot_entry(snapshot_journal.value()) != nullptr) {
        return core::Result<FileChunkDeltaWriter>::failure(
            "save_database.snapshot_journal_pending",
            "compact the accepted snapshot journal before opening a chunk delta writer");
    }

    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<FileChunkDeltaWriter>::failure(save_root.error().code,
                                                           save_root.error().message);
    }

    FileChunkDeltaWriter writer;
    writer.database_root_ = root_;
    writer.stats_.selected_save_root = save_root.value();
    if (save_root.value().parent_path() == generations_directory(root_)) {
        writer.stats_.active_generation = save_root.value().filename().string();
    }

    std::vector<FileChunkDeltaWriter::Entry> base;
    auto has_chunk_table = has_external_chunk_table(save_root.value());
    if (!has_chunk_table) {
        return core::Result<FileChunkDeltaWriter>::failure(has_chunk_table.error().code,
                                                           has_chunk_table.error().message);
    }
    if (has_chunk_table.value()) {
        auto chunks = readable_chunk_directory(save_root.value());
        if (!chunks) {
            return core::Result<FileChunkDeltaWriter>::failure(chunks.error().code,
                                                               chunks.error().message);
        }
        auto index = read_chunk_index(save_root.value());
        if (!index) {
            return core::Result<FileChunkDeltaWriter>::failure(index.error().code,
                                                               index.error().message);
        }
        base.reserve(index.value().size());
        std::size_t total_payload_bytes = 0;
        std::error_code filesystem_error;
        for (const auto& entry : index.value()) {
            const auto payload_bytes =
                std::filesystem::file_size(chunks.value() / entry.filename, filesystem_error);
            if (filesystem_error) {
                return core::Result<FileChunkDeltaWriter>::failure(
                    "save_database.chunk_delta_writer_open_failed", filesystem_error.message());
            }
            if (payload_bytes == 0 || payload_bytes > max_chunk_delta_file_bytes) {
                return core::Result<FileChunkDeltaWriter>::failure(
                    payload_bytes == 0 ? "save_database.empty_chunk_delta"
                                       : "save_database.chunk_delta_too_large",
                    "indexed chunk delta payload size is invalid");
            }
            if (payload_bytes > max_chunk_delta_table_bytes - total_payload_bytes) {
                return core::Result<FileChunkDeltaWriter>::failure(
                    "save_database.chunk_delta_table_too_large",
                    "chunk delta table exceeds the configured aggregate safety limit");
            }
            total_payload_bytes += static_cast<std::size_t>(payload_bytes);
            base.push_back({entry.coord, static_cast<std::size_t>(payload_bytes)});
        }
    } else {
        const auto path = snapshot_path(save_root.value());
        std::error_code filesystem_error;
        const bool has_snapshot = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            return core::Result<FileChunkDeltaWriter>::failure(
                "save_database.chunk_delta_writer_open_failed", filesystem_error.message());
        }
        if (has_snapshot) {
            auto bytes = read_bytes(path, max_snapshot_file_bytes,
                                    "save_database.snapshot_too_large", "binary save snapshot");
            if (!bytes) {
                return core::Result<FileChunkDeltaWriter>::failure(bytes.error().code,
                                                                   bytes.error().message);
            }
            auto snapshot = SaveBinaryCodec::decode_snapshot(bytes.value());
            if (!snapshot) {
                return core::Result<FileChunkDeltaWriter>::failure(snapshot.error().code,
                                                                   snapshot.error().message);
            }
            auto validation = validate_chunk_deltas_for_storage(snapshot.value().chunk_edits);
            if (!validation) {
                return core::Result<FileChunkDeltaWriter>::failure(validation.error().code,
                                                                   validation.error().message);
            }
            base.reserve(snapshot.value().chunk_edits.size());
            for (const auto& record : snapshot.value().chunk_edits) {
                base.push_back({record.coord, record.encoded_edit_delta.size()});
            }
            std::ranges::sort(base, [](const FileChunkDeltaWriter::Entry& left,
                                       const FileChunkDeltaWriter::Entry& right) {
                return left.coord < right.coord;
            });
        }
    }

    auto journal = read_chunk_delta_journal_state(save_root.value());
    if (!journal) {
        return core::Result<FileChunkDeltaWriter>::failure(journal.error().code,
                                                           journal.error().message);
    }
    for (const auto& entry : journal.value().entries) {
        auto validated = read_chunk_delta_journal_payload(entry);
        if (!validated) {
            return core::Result<FileChunkDeltaWriter>::failure(validated.error().code,
                                                               validated.error().message);
        }
    }
    writer.stats_.journal_entry_count = journal.value().entries.size();
    writer.stats_.journal_bytes = static_cast<std::size_t>(journal.value().bytes);
    if (!journal.value().entries.empty()) {
        writer.stats_.highest_sequence = journal.value().entries.back().sequence;
    }

    auto latest = latest_chunk_delta_journal_entries(journal.value());
    std::vector<FileChunkDeltaWriter::Entry> updates;
    updates.reserve(latest.size());
    for (const auto& entry : latest) {
        updates.push_back({entry.coord, entry.payload_bytes});
    }

    writer.entries_.reserve(base.size() + updates.size());
    auto base_it = base.begin();
    auto update_it = updates.begin();
    while (base_it != base.end() || update_it != updates.end()) {
        if (update_it == updates.end() ||
            (base_it != base.end() && base_it->coord < update_it->coord)) {
            writer.entries_.push_back(std::move(*base_it));
            ++base_it;
        } else if (base_it == base.end() || update_it->coord < base_it->coord) {
            writer.entries_.push_back(std::move(*update_it));
            ++update_it;
        } else {
            writer.entries_.push_back(std::move(*update_it));
            ++base_it;
            ++update_it;
        }
    }
    if (writer.entries_.size() > max_chunk_delta_count) {
        return core::Result<FileChunkDeltaWriter>::failure(
            "save_database.too_many_chunk_deltas",
            "effective chunk delta table exceeds the configured record limit");
    }
    for (const auto& entry : writer.entries_) {
        if (entry.payload_bytes >
            max_chunk_delta_table_bytes - writer.stats_.effective_payload_bytes) {
            return core::Result<FileChunkDeltaWriter>::failure(
                "save_database.chunk_delta_table_too_large",
                "effective chunk delta table exceeds the configured aggregate safety limit");
        }
        writer.stats_.effective_payload_bytes += entry.payload_bytes;
    }
    writer.stats_.effective_chunk_delta_count = writer.entries_.size();
    return core::Result<FileChunkDeltaWriter>::success(std::move(writer));
}

core::Result<ChunkEditSaveRecord>
FileSaveDatabase::read_chunk_delta(world::ChunkCoord coord) const {
    auto reader = open_chunk_delta_reader();
    if (!reader) {
        return core::Result<ChunkEditSaveRecord>::failure(reader.error().code,
                                                          reader.error().message);
    }
    auto delta = reader.value().read_chunk_delta(coord);
    if (!delta) {
        return core::Result<ChunkEditSaveRecord>::failure(delta.error().code,
                                                          delta.error().message);
    }
    if (!delta.value().has_value()) {
        return core::Result<ChunkEditSaveRecord>::failure(
            "save_database.missing_chunk_delta", "chunk delta is not present in save database");
    }
    return core::Result<ChunkEditSaveRecord>::success(std::move(*delta.value()));
}

core::Result<std::vector<ChunkEditSaveRecord>> FileSaveDatabase::read_chunk_deltas() const {
    auto snapshot_journal = read_journal_state(root_);
    if (!snapshot_journal) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
            snapshot_journal.error().code, snapshot_journal.error().message);
    }
    const auto* pending_snapshot = latest_pending_snapshot_entry(snapshot_journal.value());
    if (pending_snapshot != nullptr) {
        auto snapshot = read_journal_snapshot(*pending_snapshot);
        if (!snapshot) {
            return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
                snapshot.error().code, snapshot.error().message);
        }
        auto validation = validate_chunk_deltas_for_storage(snapshot.value().chunk_edits);
        if (!validation) {
            return core::Result<std::vector<ChunkEditSaveRecord>>::failure(
                validation.error().code, validation.error().message);
        }
        std::ranges::sort(snapshot.value().chunk_edits,
                          [](const ChunkEditSaveRecord& left, const ChunkEditSaveRecord& right) {
                              return left.coord < right.coord;
                          });
        return core::Result<std::vector<ChunkEditSaveRecord>>::success(
            std::move(snapshot).value().chunk_edits);
    }

    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<std::vector<ChunkEditSaveRecord>>::failure(save_root.error().code,
                                                                       save_root.error().message);
    }
    return read_effective_chunk_deltas_from_root(save_root.value());
}

core::Result<ChunkDeltaJournalCompactionResult>
FileSaveDatabase::compact_chunk_delta_journal() const {
    auto snapshot_journal = read_journal_state(root_);
    if (!snapshot_journal) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(
            snapshot_journal.error().code, snapshot_journal.error().message);
    }
    if (latest_pending_snapshot_entry(snapshot_journal.value()) != nullptr) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(
            "save_database.snapshot_journal_pending",
            "compact the accepted snapshot journal before compacting chunk deltas");
    }

    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(save_root.error().code,
                                                                        save_root.error().message);
    }
    auto journal = read_chunk_delta_journal_state(save_root.value());
    if (!journal) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(journal.error().code,
                                                                        journal.error().message);
    }

    ChunkDeltaJournalCompactionResult result;
    if (journal.value().entries.empty()) {
        return core::Result<ChunkDeltaJournalCompactionResult>::success(result);
    }
    auto effective = read_effective_chunk_deltas_from_root(save_root.value());
    if (!effective) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(effective.error().code,
                                                                        effective.error().message);
    }
    auto status = write_chunk_deltas_to_root(save_root.value(), effective.value());
    if (!status) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(status.error().code,
                                                                        status.error().message);
    }

    const auto journal_directory = chunk_delta_journal_directory(save_root.value());
    const auto compacted_directory = compacted_chunk_delta_journal_directory(save_root.value());
    status = remove_tree(compacted_directory,
                         "save_database.remove_compacted_chunk_delta_journal_failed");
    if (!status) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(status.error().code,
                                                                        status.error().message);
    }
    status = rename_path(journal_directory, compacted_directory,
                         "save_database.commit_chunk_delta_journal_checkpoint_failed");
    if (!status) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(status.error().code,
                                                                        status.error().message);
    }

    result.compacted = true;
    result.merged_entry_count = journal.value().entries.size();
    result.removed_entry_count = journal.value().entries.size();
    status = remove_tree(compacted_directory,
                         "save_database.remove_compacted_chunk_delta_journal_failed");
    if (!status) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(status.error().code,
                                                                        status.error().message);
    }
    if (auto flush_error = core::flush_directory_to_disk(save_root.value())) {
        return core::Result<ChunkDeltaJournalCompactionResult>::failure(
            "save_database.remove_compacted_chunk_delta_journal_failed", flush_error.message());
    }
    return core::Result<ChunkDeltaJournalCompactionResult>::success(result);
}

core::Result<ChunkDeltaJournalRecoveryResult>
FileSaveDatabase::recover_chunk_delta_journal() const {
    auto snapshot_journal = read_journal_state(root_);
    if (!snapshot_journal) {
        return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
            snapshot_journal.error().code, snapshot_journal.error().message);
    }
    if (latest_pending_snapshot_entry(snapshot_journal.value()) != nullptr) {
        return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
            "save_database.snapshot_journal_pending",
            "compact the accepted snapshot journal before recovering chunk deltas");
    }

    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<ChunkDeltaJournalRecoveryResult>::failure(save_root.error().code,
                                                                      save_root.error().message);
    }

    ChunkDeltaJournalRecoveryResult result;
    const auto journal_directory = chunk_delta_journal_directory(save_root.value());
    std::error_code error;
    const bool exists = std::filesystem::exists(journal_directory, error);
    if (error) {
        return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
            "save_database.chunk_delta_journal_recovery_failed", error.message());
    }
    if (exists) {
        for (const auto& entry : std::filesystem::directory_iterator(journal_directory, error)) {
            if (error) {
                return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
                    "save_database.chunk_delta_journal_recovery_failed", error.message());
            }
            const auto name = entry.path().filename().string();
            const bool owned_temporary =
                std::string_view(name).starts_with(chunk_delta_journal_entry_prefix) &&
                std::string_view(name).ends_with(std::string(chunk_delta_journal_entry_suffix) +
                                                 ".tmp");
            if (!owned_temporary) {
                continue;
            }
            const bool removed = std::filesystem::remove(entry.path(), error);
            if (error) {
                return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
                    "save_database.chunk_delta_journal_recovery_failed", error.message());
            }
            if (removed) {
                ++result.discarded_temporary_entry_count;
            }
        }
        if (result.discarded_temporary_entry_count > 0) {
            if (auto flush_error = core::flush_directory_to_disk(journal_directory)) {
                return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
                    "save_database.chunk_delta_journal_recovery_failed", flush_error.message());
            }
        }
    }

    const auto compacted_directory = compacted_chunk_delta_journal_directory(save_root.value());
    const bool has_compacted_directory = std::filesystem::exists(compacted_directory, error);
    if (error) {
        return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
            "save_database.chunk_delta_journal_recovery_failed", error.message());
    }
    if (has_compacted_directory) {
        auto status =
            remove_tree(compacted_directory, "save_database.chunk_delta_journal_recovery_failed");
        if (!status) {
            return core::Result<ChunkDeltaJournalRecoveryResult>::failure(status.error().code,
                                                                          status.error().message);
        }
        if (auto flush_error = core::flush_directory_to_disk(save_root.value())) {
            return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
                "save_database.chunk_delta_journal_recovery_failed", flush_error.message());
        }
        result.discarded_compacted_directory = true;
    }

    auto journal = read_chunk_delta_journal_state(save_root.value());
    if (!journal) {
        return core::Result<ChunkDeltaJournalRecoveryResult>::failure(journal.error().code,
                                                                      journal.error().message);
    }
    for (const auto& entry : journal.value().entries) {
        auto validated = read_chunk_delta_journal_payload(entry);
        if (!validated) {
            return core::Result<ChunkDeltaJournalRecoveryResult>::failure(
                validated.error().code, validated.error().message);
        }
    }
    return core::Result<ChunkDeltaJournalRecoveryResult>::success(result);
}

core::Result<std::size_t> FileSaveDatabase::compact_chunk_deltas() const {
    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<std::size_t>::failure(save_root.error().code,
                                                  save_root.error().message);
    }

    auto entries = read_chunk_index(save_root.value());
    if (!entries) {
        return core::Result<std::size_t>::failure(entries.error().code, entries.error().message);
    }

    std::unordered_set<std::string> referenced_files;
    referenced_files.reserve(entries.value().size());
    for (const auto& entry : entries.value()) {
        referenced_files.insert(entry.filename);
    }

    auto chunks = readable_chunk_directory(save_root.value());
    if (!chunks) {
        return core::Result<std::size_t>::failure(chunks.error().code, chunks.error().message);
    }
    std::error_code error;
    const bool has_chunks = std::filesystem::exists(chunks.value(), error);
    if (error) {
        return core::Result<std::size_t>::failure("save_database.compact_failed", error.message());
    }
    if (!has_chunks) {
        return core::Result<std::size_t>::success(0);
    }

    std::size_t removed_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(chunks.value(), error)) {
        if (error) {
            return core::Result<std::size_t>::failure("save_database.compact_failed",
                                                      error.message());
        }
        const bool is_regular_file = entry.is_regular_file(error);
        if (error) {
            return core::Result<std::size_t>::failure("save_database.compact_failed",
                                                      error.message());
        }
        if (!is_regular_file) {
            continue;
        }

        const auto filename = entry.path().filename().string();
        if (entry.path().extension() != ".delta" || referenced_files.contains(filename)) {
            continue;
        }

        std::filesystem::remove(entry.path(), error);
        if (error) {
            return core::Result<std::size_t>::failure("save_database.compact_failed",
                                                      error.message());
        }
        ++removed_count;
    }
    if (error) {
        return core::Result<std::size_t>::failure("save_database.compact_failed", error.message());
    }

    return core::Result<std::size_t>::success(removed_count);
}

core::Status FileSaveDatabase::prune_stale_generations(std::size_t keep_stale_generations) const {
    std::error_code error;
    const bool has_manifest = std::filesystem::exists(current_generation_path(root_), error);
    if (error) {
        return filesystem_failure("save_database.prune_failed", error);
    }
    if (!has_manifest) {
        return core::Status::ok();
    }

    auto active = read_current_generation(root_);
    if (!active) {
        return core::Status::failure(active.error().code, active.error().message);
    }

    auto active_root = active_save_root(root_);
    if (!active_root) {
        return core::Status::failure(active_root.error().code, active_root.error().message);
    }

    auto committed_generations = collect_committed_generations(root_);
    if (!committed_generations) {
        return core::Status::failure(committed_generations.error().code,
                                     committed_generations.error().message);
    }

    std::vector<CommittedGenerationEntry> stale_generations;
    stale_generations.reserve(committed_generations.value().size());
    for (const auto& generation : committed_generations.value()) {
        if (generation.name != active.value()) {
            stale_generations.push_back(generation);
        }
    }

    std::ranges::sort(stale_generations, [](const CommittedGenerationEntry& left,
                                            const CommittedGenerationEntry& right) {
        return left.number > right.number;
    });

    for (std::size_t index = keep_stale_generations; index < stale_generations.size(); ++index) {
        auto status = remove_tree(stale_generations[index].path, "save_database.prune_failed");
        if (!status) {
            return status;
        }
    }

    return core::Status::ok();
}

core::Result<std::size_t> FileSaveDatabase::recover_staged_generations() const {
    std::error_code error;
    const bool has_manifest = std::filesystem::exists(current_generation_path(root_), error);
    if (error) {
        return core::Result<std::size_t>::failure("save_database.recover_failed", error.message());
    }
    if (has_manifest) {
        auto active_root = active_save_root(root_);
        if (!active_root) {
            return core::Result<std::size_t>::failure(active_root.error().code,
                                                      active_root.error().message);
        }
    }

    auto staged_generations = collect_staged_generations(root_);
    if (!staged_generations) {
        return core::Result<std::size_t>::failure(staged_generations.error().code,
                                                  staged_generations.error().message);
    }

    std::ranges::sort(staged_generations.value(),
                      [](const StagedGenerationEntry& left, const StagedGenerationEntry& right) {
                          return left.number < right.number;
                      });

    std::size_t removed_count = 0;
    for (const auto& staged_generation : staged_generations.value()) {
        auto status = remove_tree(staged_generation.path, "save_database.recover_failed");
        if (!status) {
            return core::Result<std::size_t>::failure(status.error().code, status.error().message);
        }
        ++removed_count;
    }

    return core::Result<std::size_t>::success(removed_count);
}

core::Result<SaveDatabaseMaintenanceResult>
FileSaveDatabase::maintain(const SaveDatabaseMaintenancePolicy& policy) const {
    auto before = stats();
    if (!before) {
        return core::Result<SaveDatabaseMaintenanceResult>::failure(before.error().code,
                                                                    before.error().message);
    }

    SaveDatabaseMaintenanceResult result;
    result.before = before.value();

    if (policy.recover_staged_generations) {
        auto recovered = recover_staged_generations();
        if (!recovered) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(recovered.error().code,
                                                                        recovered.error().message);
        }
        result.recovered_staged_generation_count = recovered.value();
    }

    if (policy.recover_snapshot_journal) {
        auto recovered = recover_snapshot_journal();
        if (!recovered) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(recovered.error().code,
                                                                        recovered.error().message);
        }
        result.journal_recovery = std::move(recovered).value();
    }

    // A pending full snapshot supersedes the active generation and its chunk journal. Publish that
    // authority first so stale-generation corruption cannot block recovery of the accepted save.
    if (policy.recover_chunk_delta_journal) {
        auto recovered = recover_chunk_delta_journal();
        if (!recovered) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(recovered.error().code,
                                                                        recovered.error().message);
        }
        result.chunk_delta_journal_recovery = std::move(recovered).value();
    }

    if (policy.prune_stale_generations) {
        auto pre_prune = stats();
        if (!pre_prune) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(pre_prune.error().code,
                                                                        pre_prune.error().message);
        }
        auto status = prune_stale_generations(policy.keep_stale_generations);
        if (!status) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(status.error().code,
                                                                        status.error().message);
        }
        auto post_prune = stats();
        if (!post_prune) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(post_prune.error().code,
                                                                        post_prune.error().message);
        }
        if (pre_prune.value().committed_generation_count >
            post_prune.value().committed_generation_count) {
            result.pruned_stale_generation_count = pre_prune.value().committed_generation_count -
                                                   post_prune.value().committed_generation_count;
        }
    }

    if (policy.compact_chunk_delta_journal) {
        auto compacted = compact_chunk_delta_journal();
        if (!compacted) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(compacted.error().code,
                                                                        compacted.error().message);
        }
        result.chunk_delta_journal_compaction = std::move(compacted).value();
    }

    if (policy.compact_chunk_deltas) {
        auto compacted = compact_chunk_deltas();
        if (!compacted) {
            return core::Result<SaveDatabaseMaintenanceResult>::failure(compacted.error().code,
                                                                        compacted.error().message);
        }
        result.compacted_chunk_delta_count = compacted.value();
    }

    auto after = stats();
    if (!after) {
        return core::Result<SaveDatabaseMaintenanceResult>::failure(after.error().code,
                                                                    after.error().message);
    }
    result.after = after.value();
    return core::Result<SaveDatabaseMaintenanceResult>::success(std::move(result));
}

core::Result<SaveDatabaseMigrationResult>
FileSaveDatabase::migrate_to_schema(const SaveMigrationRegistry& registry,
                                    std::uint32_t target_schema_version) const {
    auto before = stats();
    if (!before) {
        return core::Result<SaveDatabaseMigrationResult>::failure(before.error().code,
                                                                  before.error().message);
    }

    auto snapshot = read_snapshot();
    if (!snapshot) {
        return core::Result<SaveDatabaseMigrationResult>::failure(snapshot.error().code,
                                                                  snapshot.error().message);
    }

    auto migration =
        SaveMigrationRunner::migrate(snapshot.value(), registry, target_schema_version);
    if (!migration) {
        return core::Result<SaveDatabaseMigrationResult>::failure(migration.error().code,
                                                                  migration.error().message);
    }

    SaveDatabaseMigrationResult result;
    result.before = before.value();
    result.migration = std::move(migration).value();

    if (!result.migration.applied_migrations.empty()) {
        auto status = write_snapshot(snapshot.value());
        if (!status) {
            return core::Result<SaveDatabaseMigrationResult>::failure(status.error().code,
                                                                      status.error().message);
        }
        result.wrote_snapshot = true;
    }

    auto after = stats();
    if (!after) {
        return core::Result<SaveDatabaseMigrationResult>::failure(after.error().code,
                                                                  after.error().message);
    }
    result.after = after.value();
    return core::Result<SaveDatabaseMigrationResult>::success(std::move(result));
}

core::Result<SaveDatabaseStats> FileSaveDatabase::stats() const {
    auto save_root = active_save_root(root_);
    if (!save_root) {
        return core::Result<SaveDatabaseStats>::failure(save_root.error().code,
                                                        save_root.error().message);
    }

    SaveDatabaseStats result;
    std::error_code error;
    result.uses_generation_manifest =
        std::filesystem::exists(current_generation_path(root_), error);
    if (error) {
        return core::Result<SaveDatabaseStats>::failure("save_database.stats_failed",
                                                        error.message());
    }
    if (result.uses_generation_manifest) {
        auto active_generation = read_current_generation(root_);
        if (!active_generation) {
            return core::Result<SaveDatabaseStats>::failure(active_generation.error().code,
                                                            active_generation.error().message);
        }
        result.active_generation = std::move(active_generation).value();
    }

    auto generation_stats = collect_generation_directory_stats(root_);
    if (!generation_stats) {
        return core::Result<SaveDatabaseStats>::failure(generation_stats.error().code,
                                                        generation_stats.error().message);
    }
    result.committed_generation_count = generation_stats.value().committed_count;
    result.staged_generation_count = generation_stats.value().staged_count;
    if (result.uses_generation_manifest && result.committed_generation_count > 0) {
        result.stale_generation_count = result.committed_generation_count - 1;
    }

    const auto snapshot = snapshot_path(save_root.value());
    result.has_snapshot = std::filesystem::exists(snapshot, error);
    if (error) {
        return core::Result<SaveDatabaseStats>::failure("save_database.stats_failed",
                                                        error.message());
    }
    if (result.has_snapshot) {
        result.snapshot_bytes = std::filesystem::file_size(snapshot, error);
        if (error) {
            return core::Result<SaveDatabaseStats>::failure("save_database.stats_failed",
                                                            error.message());
        }
    }

    auto journal = read_journal_state(root_);
    if (!journal) {
        return core::Result<SaveDatabaseStats>::failure(journal.error().code,
                                                        journal.error().message);
    }
    result.journal_entry_count = journal.value().entries.size();
    result.journal_bytes = journal.value().bytes;
    result.journal_checkpoint_sequence = journal.value().checkpoint_sequence;
    result.journal_highest_sequence = result.journal_checkpoint_sequence;
    if (!journal.value().entries.empty()) {
        result.journal_highest_sequence =
            std::max(result.journal_highest_sequence, journal.value().entries.back().sequence);
    }

    auto chunk_delta_journal = read_chunk_delta_journal_state(save_root.value());
    if (!chunk_delta_journal) {
        return core::Result<SaveDatabaseStats>::failure(chunk_delta_journal.error().code,
                                                        chunk_delta_journal.error().message);
    }
    result.chunk_delta_journal_entry_count = chunk_delta_journal.value().entries.size();
    result.chunk_delta_journal_bytes = chunk_delta_journal.value().bytes;
    if (!chunk_delta_journal.value().entries.empty()) {
        result.chunk_delta_journal_highest_sequence =
            chunk_delta_journal.value().entries.back().sequence;
    }

    if (const auto* pending = latest_pending_snapshot_entry(journal.value())) {
        auto pending_snapshot = read_journal_snapshot(*pending);
        if (!pending_snapshot) {
            return core::Result<SaveDatabaseStats>::failure(pending_snapshot.error().code,
                                                            pending_snapshot.error().message);
        }
        auto validation = validate_chunk_deltas_for_storage(pending_snapshot.value().chunk_edits);
        if (!validation) {
            return core::Result<SaveDatabaseStats>::failure(validation.error().code,
                                                            validation.error().message);
        }
        result.chunk_delta_count = pending_snapshot.value().chunk_edits.size();
        for (const auto& chunk_delta : pending_snapshot.value().chunk_edits) {
            result.chunk_delta_bytes += chunk_delta.encoded_edit_delta.size();
        }
    } else {
        auto chunk_delta_writer = open_chunk_delta_writer();
        if (!chunk_delta_writer) {
            return core::Result<SaveDatabaseStats>::failure(chunk_delta_writer.error().code,
                                                            chunk_delta_writer.error().message);
        }
        result.chunk_delta_count = chunk_delta_writer.value().stats().effective_chunk_delta_count;
        result.chunk_delta_bytes = chunk_delta_writer.value().stats().effective_payload_bytes;
    }

    return core::Result<SaveDatabaseStats>::success(result);
}

} // namespace heartstead::save
