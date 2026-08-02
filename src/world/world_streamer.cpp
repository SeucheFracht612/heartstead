#include "heartstead/world/world_streamer.hpp"

#include "heartstead/core/parallel.hpp"
#include "heartstead/voxel/block_registry.hpp"
#include "heartstead/voxel/greedy_mesher.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace heartstead::world {

[[nodiscard]] heartstead::Int3 desired_world_center(
    const game::Camera& camera,
    const heartstead::VideoSettings& settings) noexcept {
    const auto forward = camera.forward();
    const auto look_ahead_chunks = std::min(8, std::max(0, settings.render_distance_chunks / 2 - 2));
    return {
        heartstead::floor_div(static_cast<std::int32_t>(std::floor(camera.position.x)), heartstead::Chunk::edge) +
            static_cast<std::int32_t>(std::round(forward.x * static_cast<float>(look_ahead_chunks))),
        0,
        heartstead::floor_div(static_cast<std::int32_t>(std::floor(camera.position.z)), heartstead::Chunk::edge) +
            static_cast<std::int32_t>(std::round(forward.z * static_cast<float>(look_ahead_chunks))),
    };
}

[[nodiscard]] WorldArea world_area_for(
    heartstead::Int3 center_chunk,
    const heartstead::VideoSettings& settings) noexcept {
    return {
        .chunks_per_axis = settings.render_distance_chunks,
        .center_chunk = center_chunk,
    };
}

[[nodiscard]] SceneBuildResult build_scene(
    heartstead::Int3 center_chunk,
    const heartstead::VideoSettings& settings,
    const WorldEdits& edits,
    std::uint64_t revision) {
    using Clock = std::chrono::steady_clock;
    SceneBuildResult result;
    result.area = world_area_for(center_chunk, settings);
    auto generation_area = result.area;
    generation_area.chunks_per_axis += 2;
    const auto start = Clock::now();
    result.world = make_world(generation_area, edits);
    result.mesh = mesh_world(result.world, result.area);
    result.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    result.revision = revision;
    return result;
}

[[nodiscard]] StreamBatch build_stream_batch(
    WorldArea old_area,
    heartstead::Int3 center_chunk,
    const heartstead::VideoSettings& settings,
    const WorldEdits& edits,
    std::uint64_t revision) {
    using namespace heartstead;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    StreamBatch result;
    result.area = world_area_for(center_chunk, settings);
    result.revision = revision;

    const auto old_visible = world_coordinates(old_area);
    const auto new_visible = world_coordinates(result.area);
    const std::unordered_set<Int3> old_visible_set(old_visible.begin(), old_visible.end());
    const std::unordered_set<Int3> new_visible_set(new_visible.begin(), new_visible.end());
    std::vector<Int3> mesh_coordinates;
    mesh_coordinates.reserve(new_visible.size());
    for (const auto coordinate : new_visible) {
        if (!old_visible_set.contains(coordinate)) mesh_coordinates.push_back(coordinate);
    }
    result.mesh_removals.reserve(old_visible.size());
    for (const auto coordinate : old_visible) {
        if (!new_visible_set.contains(coordinate)) result.mesh_removals.push_back(coordinate);
    }

    auto old_storage_area = old_area;
    auto new_storage_area = result.area;
    old_storage_area.chunks_per_axis += 2;
    new_storage_area.chunks_per_axis += 2;
    const auto old_storage = world_coordinates(old_storage_area);
    const auto new_storage = world_coordinates(new_storage_area);
    const std::unordered_set<Int3> old_storage_set(old_storage.begin(), old_storage.end());
    const std::unordered_set<Int3> new_storage_set(new_storage.begin(), new_storage.end());
    std::vector<Int3> storage_coordinates;
    storage_coordinates.reserve(new_storage.size());
    for (const auto coordinate : new_storage) {
        if (!old_storage_set.contains(coordinate)) storage_coordinates.push_back(coordinate);
    }
    result.storage_removals.reserve(old_storage.size());
    for (const auto coordinate : old_storage) {
        if (!new_storage_set.contains(coordinate)) result.storage_removals.push_back(coordinate);
    }

    std::unordered_set<Int3> generation_set(storage_coordinates.begin(), storage_coordinates.end());
    generation_set.reserve(generation_set.size() + mesh_coordinates.size() * 5U);
    for (const auto coordinate : mesh_coordinates) {
        generation_set.insert(coordinate);
        generation_set.insert({coordinate.x - 1, coordinate.y, coordinate.z});
        generation_set.insert({coordinate.x + 1, coordinate.y, coordinate.z});
        generation_set.insert({coordinate.x, coordinate.y, coordinate.z - 1});
        generation_set.insert({coordinate.x, coordinate.y, coordinate.z + 1});
    }
    std::vector<Int3> generation_coordinates(generation_set.begin(), generation_set.end());
    ChunkWorld generated_world;
    std::vector<Chunk*> generated_chunks;
    generated_chunks.reserve(generation_coordinates.size());
    for (const auto coordinate : generation_coordinates)
        generated_chunks.push_back(&generated_world.ensure_chunk(coordinate));
    parallel_for(generated_chunks.size(), [&](std::size_t index) {
        *generated_chunks[index] = generate_chunk(generation_coordinates[index], edits);
    });

    const auto blocks = BlockRegistry::defaults();
    result.mesh_updates.resize(mesh_coordinates.size());
    parallel_for(mesh_coordinates.size(), [&](std::size_t index) {
        const auto coordinate = mesh_coordinates[index];
        const auto* chunk = generated_world.find_chunk(coordinate);
        if (chunk == nullptr) return;
        const ChunkNeighbors neighbors{
            .negative_x = generated_world.find_chunk({coordinate.x - 1, coordinate.y, coordinate.z}),
            .positive_x = generated_world.find_chunk({coordinate.x + 1, coordinate.y, coordinate.z}),
            .negative_y = nullptr,
            .positive_y = nullptr,
            .negative_z = generated_world.find_chunk({coordinate.x, coordinate.y, coordinate.z - 1}),
            .positive_z = generated_world.find_chunk({coordinate.x, coordinate.y, coordinate.z + 1}),
        };
        result.mesh_updates[index] = {coordinate, GreedyMesher::build(*chunk, blocks, neighbors)};
    });

    result.storage_updates.reserve(storage_coordinates.size());
    for (const auto coordinate : storage_coordinates) {
        auto chunk = generated_world.take_chunk(coordinate);
        if (chunk) result.storage_updates.push_back({coordinate, std::move(*chunk)});
    }
    result.milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return result;
}



} // namespace heartstead::world

