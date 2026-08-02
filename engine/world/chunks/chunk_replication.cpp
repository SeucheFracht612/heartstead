#include "engine/world/chunks/chunk_replication.hpp"

#include "engine/net/binary_message.hpp"

#include <charconv>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace heartstead::world {

namespace {

constexpr std::string_view magic = "heartstead.chunk_snapshot_slice.v1";
constexpr std::size_t cells_per_slice =
    static_cast<std::size_t>(VoxelChunk::edge_length) * VoxelChunk::edge_length;

template <typename T>
[[nodiscard]] core::Result<T> parse_number(std::string_view text, std::string_view field) {
    T result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return core::Result<T>::failure("chunk_snapshot.invalid_number",
                                        "chunk snapshot field is invalid: " +
                                            std::string(field));
    }
    return core::Result<T>::success(result);
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(delimiter, start);
        result.push_back(text.substr(start, end == std::string_view::npos ? text.size() - start
                                                                         : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return result;
}

[[nodiscard]] core::Result<VoxelCell> decode_cell(std::span<const std::string_view> fields) {
    auto type = parse_number<std::uint16_t>(fields[0], "type");
    auto light = parse_number<std::uint16_t>(fields[1], "light");
    auto state = parse_number<std::uint16_t>(fields[2], "state");
    auto metadata = parse_number<std::uint32_t>(fields[3], "metadata");
    if (!type || !light || !state || !metadata ||
        light.value() > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<VoxelCell>::failure("chunk_snapshot.invalid_cell",
                                                "chunk snapshot cell fields are invalid");
    }
    return core::Result<VoxelCell>::success(
        {type.value(), static_cast<std::uint8_t>(light.value()), state.value(), metadata.value()});
}

} // namespace

core::Status ChunkSnapshotSlice::validate() const {
    if (!identity.is_valid() || content_revision == 0 || slice_y >= VoxelChunk::edge_length ||
        cells.size() != cells_per_slice) {
        return core::Status::failure(
            "chunk_snapshot.invalid_slice",
            "chunk snapshot slice needs a valid identity, revision, y coordinate, and cell count");
    }
    return core::Status::ok();
}

core::Result<std::vector<ChunkSnapshotSlice>> make_chunk_snapshot_slices(const VoxelChunk& chunk) {
    if (!chunk.identity().is_valid() || chunk.content_revision() == 0) {
        return core::Result<std::vector<ChunkSnapshotSlice>>::failure(
            "chunk_snapshot.invalid_chunk", "resident chunk identity or revision is invalid");
    }
    std::vector<ChunkSnapshotSlice> result;
    result.reserve(VoxelChunk::edge_length);
    for (std::uint16_t y = 0; y < VoxelChunk::edge_length; ++y) {
        ChunkSnapshotSlice slice;
        slice.identity = chunk.identity();
        slice.content_revision = chunk.content_revision();
        slice.slice_y = y;
        slice.cells.reserve(cells_per_slice);
        for (std::uint16_t z = 0; z < VoxelChunk::edge_length; ++z) {
            for (std::uint16_t x = 0; x < VoxelChunk::edge_length; ++x) {
                auto cell = chunk.get({x, y, z});
                if (!cell) {
                    return core::Result<std::vector<ChunkSnapshotSlice>>::failure(
                        cell.error().code, cell.error().message);
                }
                slice.cells.push_back(cell.value());
            }
        }
        result.push_back(std::move(slice));
    }
    return core::Result<std::vector<ChunkSnapshotSlice>>::success(std::move(result));
}

std::string ChunkSnapshotSliceTextCodec::encode(const ChunkSnapshotSlice& slice) {
    std::ostringstream output;
    output << magic << '\n';
    output << "coord=" << slice.identity.coordinate.x << '|' << slice.identity.coordinate.y << '|'
           << slice.identity.coordinate.z << '\n';
    output << "generation=" << slice.identity.load_generation << '\n';
    output << "revision=" << slice.content_revision << '\n';
    output << "slice_y=" << slice.slice_y << '\n';
    for (std::size_t first = 0; first < slice.cells.size();) {
        std::size_t count = 1;
        while (first + count < slice.cells.size() &&
               slice.cells[first + count] == slice.cells[first]) {
            ++count;
        }
        const auto& cell = slice.cells[first];
        output << "run=" << cell.type << '|' << static_cast<unsigned>(cell.light) << '|'
               << cell.state_bits << '|' << cell.metadata_handle << '|' << count << '\n';
        first += count;
    }
    output << "end\n";
    return output.str();
}

core::Result<ChunkSnapshotSlice>
ChunkSnapshotSliceTextCodec::decode(std::string_view payload) {
    ChunkSnapshotSlice result;
    bool saw_magic = false;
    bool saw_coord = false;
    bool saw_generation = false;
    bool saw_revision = false;
    bool saw_slice = false;
    bool saw_end = false;
    std::size_t start = 0;
    while (start <= payload.size()) {
        const auto line_end = payload.find('\n', start);
        auto line = payload.substr(start, line_end == std::string_view::npos
                                              ? payload.size() - start
                                              : line_end - start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (!saw_magic) {
            if (line != magic) {
                return core::Result<ChunkSnapshotSlice>::failure(
                    "chunk_snapshot.invalid_magic", "chunk snapshot magic is invalid");
            }
            saw_magic = true;
        } else if (line == "end") {
            saw_end = true;
            break;
        } else if (!line.empty()) {
            const auto separator = line.find('=');
            if (separator == std::string_view::npos) {
                return core::Result<ChunkSnapshotSlice>::failure(
                    "chunk_snapshot.invalid_line", "chunk snapshot line has no separator");
            }
            const auto key = line.substr(0, separator);
            const auto value = line.substr(separator + 1);
            if (key == "coord") {
                const auto fields = split(value, '|');
                if (saw_coord || fields.size() != 3) {
                    return core::Result<ChunkSnapshotSlice>::failure(
                        "chunk_snapshot.invalid_coord", "chunk snapshot coordinate is invalid");
                }
                auto x = parse_number<std::int64_t>(fields[0], "chunk_x");
                auto y = parse_number<std::int64_t>(fields[1], "chunk_y");
                auto z = parse_number<std::int64_t>(fields[2], "chunk_z");
                if (!x || !y || !z) {
                    return core::Result<ChunkSnapshotSlice>::failure(
                        "chunk_snapshot.invalid_coord", "chunk snapshot coordinate is invalid");
                }
                result.identity.coordinate = {x.value(), y.value(), z.value()};
                saw_coord = true;
            } else if (key == "generation") {
                auto parsed = parse_number<std::uint64_t>(value, key);
                if (!parsed || saw_generation) {
                    return core::Result<ChunkSnapshotSlice>::failure(
                        "chunk_snapshot.invalid_generation", "chunk snapshot generation is invalid");
                }
                result.identity.load_generation = parsed.value();
                saw_generation = true;
            } else if (key == "revision") {
                auto parsed = parse_number<std::uint64_t>(value, key);
                if (!parsed || saw_revision) {
                    return core::Result<ChunkSnapshotSlice>::failure(
                        "chunk_snapshot.invalid_revision", "chunk snapshot revision is invalid");
                }
                result.content_revision = parsed.value();
                saw_revision = true;
            } else if (key == "slice_y") {
                auto parsed = parse_number<std::uint16_t>(value, key);
                if (!parsed || saw_slice) {
                    return core::Result<ChunkSnapshotSlice>::failure(
                        "chunk_snapshot.invalid_slice", "chunk snapshot y slice is invalid");
                }
                result.slice_y = parsed.value();
                saw_slice = true;
            } else if (key == "run") {
                const auto fields = split(value, '|');
                if (fields.size() != 5) {
                    return core::Result<ChunkSnapshotSlice>::failure(
                        "chunk_snapshot.invalid_run", "chunk snapshot run is invalid");
                }
                auto cell = decode_cell(std::span<const std::string_view>(fields.data(), 4));
                auto count = parse_number<std::uint32_t>(fields[4], "run_count");
                if (!cell || !count || count.value() == 0 ||
                    count.value() > cells_per_slice - result.cells.size()) {
                    return core::Result<ChunkSnapshotSlice>::failure(
                        "chunk_snapshot.invalid_run", "chunk snapshot run exceeds its slice");
                }
                result.cells.insert(result.cells.end(), count.value(), cell.value());
            } else {
                return core::Result<ChunkSnapshotSlice>::failure(
                    "chunk_snapshot.unknown_field", "chunk snapshot field is unknown");
            }
        }
        if (line_end == std::string_view::npos) {
            break;
        }
        start = line_end + 1;
    }
    if (!saw_magic || !saw_coord || !saw_generation || !saw_revision || !saw_slice || !saw_end) {
        return core::Result<ChunkSnapshotSlice>::failure(
            "chunk_snapshot.incomplete", "chunk snapshot slice is incomplete");
    }
    auto status = result.validate();
    if (!status) {
        return core::Result<ChunkSnapshotSlice>::failure(status.error().code,
                                                         status.error().message);
    }
    return core::Result<ChunkSnapshotSlice>::success(std::move(result));
}

std::string ChunkSnapshotSliceBinaryCodec::encode(const ChunkSnapshotSlice& slice) {
    net::BinaryMessageWriter writer;
    writer.u32(0x31534348U);
    writer.var_i64(slice.identity.coordinate.x);
    writer.var_i64(slice.identity.coordinate.y);
    writer.var_i64(slice.identity.coordinate.z);
    writer.var_u64(slice.identity.load_generation);
    writer.var_u64(slice.content_revision);
    writer.var_u64(slice.slice_y);
    std::uint64_t run_count = 0;
    for (std::size_t first = 0; first < slice.cells.size();) {
        std::size_t count = 1;
        while (first + count < slice.cells.size() &&
               slice.cells[first + count] == slice.cells[first]) {
            ++count;
        }
        ++run_count;
        first += count;
    }
    writer.var_u64(run_count);
    for (std::size_t first = 0; first < slice.cells.size();) {
        std::size_t count = 1;
        while (first + count < slice.cells.size() &&
               slice.cells[first + count] == slice.cells[first]) {
            ++count;
        }
        const auto& cell = slice.cells[first];
        writer.var_u64(cell.type);
        writer.u8(cell.light);
        writer.var_u64(cell.state_bits);
        writer.var_u64(cell.metadata_handle);
        writer.var_u64(count);
        first += count;
    }
    return writer.take();
}

core::Result<ChunkSnapshotSlice>
ChunkSnapshotSliceBinaryCodec::decode(std::string_view payload) {
    net::BinaryMessageReader reader(payload);
    std::uint32_t magic_value = 0;
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t z = 0;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    std::uint64_t slice_y = 0;
    std::uint64_t run_count = 0;
    if (!reader.u32(magic_value) || magic_value != 0x31534348U ||
        !reader.var_i64(x) || !reader.var_i64(y) || !reader.var_i64(z) ||
        !reader.var_u64(generation) || !reader.var_u64(revision) ||
        !reader.var_u64(slice_y) || !reader.var_u64(run_count) ||
        slice_y >= VoxelChunk::edge_length || run_count == 0 ||
        run_count > cells_per_slice) {
        return core::Result<ChunkSnapshotSlice>::failure(
            "chunk_snapshot.invalid_binary_header",
            "chunk snapshot binary header is invalid");
    }
    ChunkSnapshotSlice result;
    result.identity.coordinate = {x, y, z};
    result.identity.load_generation = generation;
    result.content_revision = revision;
    result.slice_y = static_cast<std::uint16_t>(slice_y);
    result.cells.reserve(cells_per_slice);
    for (std::uint64_t run = 0; run < run_count; ++run) {
        std::uint64_t type = 0;
        std::uint8_t light = 0;
        std::uint64_t state_bits = 0;
        std::uint64_t metadata = 0;
        std::uint64_t count = 0;
        if (!reader.var_u64(type) || !reader.u8(light) ||
            !reader.var_u64(state_bits) || !reader.var_u64(metadata) ||
            !reader.var_u64(count) ||
            type > std::numeric_limits<std::uint16_t>::max() ||
            state_bits > std::numeric_limits<std::uint16_t>::max() ||
            metadata > std::numeric_limits<std::uint32_t>::max() || count == 0 ||
            count > cells_per_slice - result.cells.size()) {
            return core::Result<ChunkSnapshotSlice>::failure(
                "chunk_snapshot.invalid_binary_run",
                "chunk snapshot binary run is invalid");
        }
        result.cells.insert(
            result.cells.end(), static_cast<std::size_t>(count),
            VoxelCell{static_cast<std::uint16_t>(type), light,
                      static_cast<std::uint16_t>(state_bits),
                      static_cast<std::uint32_t>(metadata)});
    }
    if (!reader.finished()) {
        return core::Result<ChunkSnapshotSlice>::failure(
            "chunk_snapshot.trailing_binary",
            "chunk snapshot binary payload has trailing bytes");
    }
    auto status = result.validate();
    if (!status) {
        return core::Result<ChunkSnapshotSlice>::failure(status.error().code,
                                                         status.error().message);
    }
    return core::Result<ChunkSnapshotSlice>::success(std::move(result));
}

net::TransportMessage make_chunk_snapshot_slice_message(const ChunkSnapshotSlice& slice,
                                                        std::uint64_t transport_sequence,
                                                        std::int64_t timestamp_ms) {
    return {net::TransportMessageKind::replication, net::TransportChannel::reliable,
            transport_sequence, std::string(chunk_snapshot_slice_payload_type),
            ChunkSnapshotSliceBinaryCodec::encode(slice), timestamp_ms};
}

core::Result<ChunkSnapshotSlice>
chunk_snapshot_slice_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::replication ||
        envelope.message.channel != net::TransportChannel::reliable ||
        (envelope.message.payload_type != chunk_snapshot_slice_payload_type &&
         envelope.message.payload_type != legacy_chunk_snapshot_slice_payload_type)) {
        return core::Result<ChunkSnapshotSlice>::failure(
            "chunk_snapshot.invalid_transport", "transport envelope is not a chunk snapshot slice");
    }
    return envelope.message.payload_type == chunk_snapshot_slice_payload_type
               ? ChunkSnapshotSliceBinaryCodec::decode(envelope.message.payload)
               : ChunkSnapshotSliceTextCodec::decode(envelope.message.payload);
}

net::TransportMessage make_chunk_subscription_removal_message(ChunkSubscriptionRemoval removal,
                                                              std::uint64_t transport_sequence,
                                                              std::int64_t timestamp_ms) {
    net::BinaryMessageWriter writer;
    writer.u32(0x31524348U);
    writer.var_i64(removal.coordinate.x);
    writer.var_i64(removal.coordinate.y);
    writer.var_i64(removal.coordinate.z);
    return {net::TransportMessageKind::replication,
            net::TransportChannel::reliable,
            transport_sequence,
            std::string(chunk_subscription_removal_payload_type),
            writer.take(),
            timestamp_ms};
}

core::Result<ChunkSubscriptionRemoval>
chunk_subscription_removal_from_transport(const net::TransportEnvelope& envelope) {
    if (envelope.message.kind != net::TransportMessageKind::replication ||
        envelope.message.channel != net::TransportChannel::reliable ||
        envelope.message.payload_type != chunk_subscription_removal_payload_type) {
        return core::Result<ChunkSubscriptionRemoval>::failure(
            "chunk_subscription.invalid_removal_transport",
            "transport envelope is not a reliable chunk subscription removal");
    }
    net::BinaryMessageReader reader(envelope.message.payload);
    std::uint32_t magic_value = 0;
    ChunkSubscriptionRemoval removal;
    if (!reader.u32(magic_value) || magic_value != 0x31524348U ||
        !reader.var_i64(removal.coordinate.x) || !reader.var_i64(removal.coordinate.y) ||
        !reader.var_i64(removal.coordinate.z) || !reader.finished()) {
        return core::Result<ChunkSubscriptionRemoval>::failure(
            "chunk_subscription.invalid_removal_payload",
            "chunk subscription removal payload is malformed or has trailing bytes");
    }
    return core::Result<ChunkSubscriptionRemoval>::success(removal);
}

} // namespace heartstead::world
