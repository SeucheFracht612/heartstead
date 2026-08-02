#pragma once

#include "heartstead/game/camera.hpp"
#include "heartstead/render/video_settings.hpp"
#include "heartstead/world/world_generation.hpp"

#include <cstdint>
#include <vector>

namespace heartstead::world {

struct SceneBuildResult {
    WorldArea area;
    ChunkWorld world;
    WorldMesh mesh;
    double milliseconds{};
    std::uint64_t revision{};
};

struct ChunkStorageUpdate {
    Int3 coordinates{};
    Chunk chunk;
};

struct StreamBatch {
    WorldArea area;
    std::vector<ChunkMeshUpdate> mesh_updates;
    std::vector<Int3> mesh_removals;
    std::vector<ChunkStorageUpdate> storage_updates;
    std::vector<Int3> storage_removals;
    double milliseconds{};
    std::uint64_t revision{};
};

[[nodiscard]] Int3 desired_world_center(
    const game::Camera& camera,
    const VideoSettings& settings) noexcept;

[[nodiscard]] WorldArea world_area_for(Int3 center_chunk, const VideoSettings& settings) noexcept;

[[nodiscard]] SceneBuildResult build_scene(
    Int3 center_chunk,
    const VideoSettings& settings,
    const WorldEdits& edits,
    std::uint64_t revision);

[[nodiscard]] StreamBatch build_stream_batch(
    WorldArea old_area,
    Int3 center_chunk,
    const VideoSettings& settings,
    const WorldEdits& edits,
    std::uint64_t revision);

} // namespace heartstead::world
