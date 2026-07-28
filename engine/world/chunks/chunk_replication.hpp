#pragma once

#include "engine/core/result.hpp"
#include "engine/net/transport.hpp"
#include "engine/world/chunks/chunk_identity.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::world {

inline constexpr std::string_view legacy_chunk_snapshot_slice_payload_type =
    "chunk.snapshot_slice.v1";
inline constexpr std::string_view chunk_snapshot_slice_payload_type =
    "chunk.snapshot_slice.bin.v1";

struct ChunkSnapshotSlice {
    ChunkIdentity identity;
    std::uint64_t content_revision = 0;
    std::uint16_t slice_y = 0;
    std::vector<VoxelCell> cells;

    [[nodiscard]] core::Status validate() const;
};

[[nodiscard]] core::Result<std::vector<ChunkSnapshotSlice>>
make_chunk_snapshot_slices(const VoxelChunk& chunk);

class ChunkSnapshotSliceTextCodec {
  public:
    [[nodiscard]] static std::string encode(const ChunkSnapshotSlice& slice);
    [[nodiscard]] static core::Result<ChunkSnapshotSlice> decode(std::string_view payload);
};

class ChunkSnapshotSliceBinaryCodec {
  public:
    [[nodiscard]] static std::string encode(const ChunkSnapshotSlice& slice);
    [[nodiscard]] static core::Result<ChunkSnapshotSlice> decode(std::string_view payload);
};

[[nodiscard]] net::TransportMessage
make_chunk_snapshot_slice_message(const ChunkSnapshotSlice& slice,
                                  std::uint64_t transport_sequence,
                                  std::int64_t timestamp_ms);
[[nodiscard]] core::Result<ChunkSnapshotSlice>
chunk_snapshot_slice_from_transport(const net::TransportEnvelope& envelope);

} // namespace heartstead::world
