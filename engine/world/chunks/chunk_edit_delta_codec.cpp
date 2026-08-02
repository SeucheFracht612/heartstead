#include "engine/world/chunks/chunk_edit_delta_codec.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace heartstead::world {

namespace {

constexpr std::string_view chunk_delta_magic_v1 = "heartstead.chunk_edit_delta.v1";
constexpr std::string_view chunk_delta_magic_v2 = "heartstead.chunk_edit_delta.v2";

[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(start));
            break;
        }
        result.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

[[nodiscard]] core::Result<std::int64_t> parse_i64(std::string_view value,
                                                   std::string_view field_name) {
    std::int64_t parsed = 0;
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || ptr != end) {
        return core::Result<std::int64_t>::failure("world_snapshot.invalid_number",
                                                   "invalid numeric chunk delta field: " +
                                                       std::string(field_name));
    }
    return core::Result<std::int64_t>::success(parsed);
}

[[nodiscard]] core::Result<std::uint16_t> parse_u16(std::string_view value,
                                                    std::string_view field_name) {
    auto parsed = parse_i64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint16_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() < 0 || parsed.value() > std::numeric_limits<std::uint16_t>::max()) {
        return core::Result<std::uint16_t>::failure("world_snapshot.number_out_of_range",
                                                    "numeric chunk delta field is out of range: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint16_t>::success(static_cast<std::uint16_t>(parsed.value()));
}

[[nodiscard]] core::Result<std::uint8_t> parse_u8(std::string_view value,
                                                  std::string_view field_name) {
    auto parsed = parse_i64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint8_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() < 0 || parsed.value() > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<std::uint8_t>::failure("world_snapshot.number_out_of_range",
                                                   "numeric chunk delta field is out of range: " +
                                                       std::string(field_name));
    }
    return core::Result<std::uint8_t>::success(static_cast<std::uint8_t>(parsed.value()));
}

[[nodiscard]] core::Result<std::uint32_t> parse_u32(std::string_view value,
                                                    std::string_view field_name) {
    auto parsed = parse_i64(value, field_name);
    if (!parsed) {
        return core::Result<std::uint32_t>::failure(parsed.error().code, parsed.error().message);
    }
    if (parsed.value() < 0 ||
        static_cast<std::uint64_t>(parsed.value()) > std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<std::uint32_t>::failure("world_snapshot.number_out_of_range",
                                                    "numeric chunk delta field is out of range: " +
                                                        std::string(field_name));
    }
    return core::Result<std::uint32_t>::success(static_cast<std::uint32_t>(parsed.value()));
}

} // namespace

std::string ChunkEditDeltaTextCodec::encode(ChunkCoord coord,
                                            const std::vector<const VoxelEditRecord*>& edits) {
    std::ostringstream output;
    output << chunk_delta_magic_v2 << '\n';
    output << "coord=" << coord.x << '|' << coord.y << '|' << coord.z << '\n';
    for (const auto* edit : edits) {
        output << "edit=" << edit->voxel_coord.x << '|' << edit->voxel_coord.y << '|'
               << edit->voxel_coord.z << '|' << edit->previous.type << '|'
               << static_cast<unsigned int>(edit->previous.light) << '|'
               << edit->previous.state_bits << '|' << edit->previous.metadata_handle << '|'
               << edit->next.type << '|' << static_cast<unsigned int>(edit->next.light) << '|'
               << edit->next.state_bits << '|' << edit->next.metadata_handle << '\n';
    }
    output << "end\n";
    return output.str();
}

core::Result<std::vector<VoxelEditRecord>>
ChunkEditDeltaTextCodec::decode(ChunkCoord expected_coord, std::string_view text) {
    bool saw_magic = false;
    bool legacy_v1 = false;
    bool saw_coord = false;
    bool saw_end = false;
    ChunkCoord actual_coord;
    std::vector<VoxelEditRecord> edits;

    std::size_t line_start = 0;
    while (line_start <= text.size()) {
        const auto line_end = text.find('\n', line_start);
        auto line = line_end == std::string_view::npos
                        ? text.substr(line_start)
                        : text.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (!saw_magic) {
            if (line != chunk_delta_magic_v1 && line != chunk_delta_magic_v2) {
                return core::Result<std::vector<VoxelEditRecord>>::failure(
                    "world_snapshot.invalid_chunk_delta_magic",
                    "chunk delta does not start with expected magic");
            }
            legacy_v1 = line == chunk_delta_magic_v1;
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
            break;
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos) {
                return core::Result<std::vector<VoxelEditRecord>>::failure(
                    "world_snapshot.invalid_chunk_delta_line",
                    "chunk delta line is missing key/value separator");
            }
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);

            if (key == "coord") {
                const auto parts = split(value, '|');
                if (parts.size() != 3) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.invalid_chunk_delta_coord",
                        "chunk delta coord must contain x, y, z");
                }
                auto x = parse_i64(parts[0], "chunk_x");
                auto y = parse_i64(parts[1], "chunk_y");
                auto z = parse_i64(parts[2], "chunk_z");
                if (!x || !y || !z) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.invalid_chunk_delta_coord",
                        "chunk delta coord contains invalid fields");
                }
                actual_coord = {x.value(), y.value(), z.value()};
                if (actual_coord != expected_coord) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.chunk_delta_coord_mismatch",
                        "chunk delta payload coord does not match the save record coord");
                }
                saw_coord = true;
            } else if (key == "edit") {
                if (!saw_coord) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.chunk_delta_edit_before_coord",
                        "chunk delta edit records must appear after coord");
                }
                const auto parts = split(value, '|');
                const auto expected_parts = legacy_v1 ? std::size_t{7} : std::size_t{11};
                if (parts.size() != expected_parts) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.invalid_chunk_delta_edit",
                        "chunk delta edit must contain voxel coord and previous/next cells");
                }
                auto x = parse_u16(parts[0], "voxel_x");
                auto y = parse_u16(parts[1], "voxel_y");
                auto z = parse_u16(parts[2], "voxel_z");
                auto previous_type = parse_u16(parts[3], "previous_type");
                auto previous_light = parse_u8(parts[4], "previous_light");
                auto previous_state = legacy_v1 ? core::Result<std::uint16_t>::success(0)
                                                : parse_u16(parts[5], "previous_state");
                auto previous_metadata = legacy_v1 ? core::Result<std::uint32_t>::success(0)
                                                   : parse_u32(parts[6], "previous_metadata");
                const auto next_offset = legacy_v1 ? std::size_t{5} : std::size_t{7};
                auto next_type = parse_u16(parts[next_offset], "next_type");
                auto next_light = parse_u8(parts[next_offset + 1], "next_light");
                auto next_state = legacy_v1 ? core::Result<std::uint16_t>::success(0)
                                            : parse_u16(parts[next_offset + 2], "next_state");
                auto next_metadata = legacy_v1 ? core::Result<std::uint32_t>::success(0)
                                               : parse_u32(parts[next_offset + 3], "next_metadata");
                if (!x || !y || !z || !previous_type || !previous_light || !previous_state ||
                    !previous_metadata || !next_type || !next_light || !next_state ||
                    !next_metadata) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.invalid_chunk_delta_edit",
                        "chunk delta edit contains invalid fields");
                }
                if (x.value() >= VoxelChunk::edge_length || y.value() >= VoxelChunk::edge_length ||
                    z.value() >= VoxelChunk::edge_length) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.chunk_delta_voxel_out_of_bounds",
                        "chunk delta edit local voxel coordinate is outside the chunk");
                }
                if (edits.size() == VoxelChunk::total_cells) {
                    return core::Result<std::vector<VoxelEditRecord>>::failure(
                        "world_snapshot.too_many_chunk_delta_edits",
                        "chunk delta contains more edit records than chunk cells");
                }
                edits.push_back(VoxelEditRecord{
                    actual_coord,
                    {x.value(), y.value(), z.value()},
                    {previous_type.value(), previous_light.value(), previous_state.value(),
                     previous_metadata.value()},
                    {next_type.value(), next_light.value(), next_state.value(),
                     next_metadata.value()},
                });
            } else {
                return core::Result<std::vector<VoxelEditRecord>>::failure(
                    "world_snapshot.unknown_chunk_delta_key",
                    "unknown chunk delta key: " + std::string(key));
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }

    if (!saw_magic || !saw_coord || !saw_end || edits.empty()) {
        return core::Result<std::vector<VoxelEditRecord>>::failure(
            "world_snapshot.incomplete_chunk_delta", "chunk delta is missing required records");
    }
    return core::Result<std::vector<VoxelEditRecord>>::success(std::move(edits));
}

} // namespace heartstead::world
