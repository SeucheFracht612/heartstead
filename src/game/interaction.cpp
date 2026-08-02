#include "heartstead/game/interaction.hpp"

#include "heartstead/voxel/block_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace heartstead::game {

std::optional<BlockRayHit> raycast_block(
    const ChunkWorld& world,
    Float3 origin,
    Float3 direction,
    float maximum_distance) noexcept {
    Int3 previous{};
    Int3 last_sample{std::numeric_limits<std::int32_t>::min(), 0, 0};
    bool has_previous = false;
    for (float distance = 0.0F; distance <= maximum_distance; distance += 0.04F) {
        const auto sample = origin + direction * distance;
        const Int3 block{
            static_cast<std::int32_t>(std::floor(sample.x)),
            static_cast<std::int32_t>(std::floor(sample.y)),
            static_cast<std::int32_t>(std::floor(sample.z)),
        };
        if (block == last_sample) continue;
        last_sample = block;
        if (world.get_block(block) != air_block) {
            if (has_previous) return BlockRayHit{block, previous, sample};
            return std::nullopt;
        }
        previous = block;
        has_previous = true;
    }
    return std::nullopt;
}

std::optional<BlockRayHit> interaction_raycast(
    const ChunkWorld& world,
    const Camera& camera,
    const Player& player,
    CameraMode mode) noexcept {
    if (mode == CameraMode::first_person)
        return raycast_block(world, camera.position, camera.forward(), 6.0F);

    const auto camera_forward = camera.forward();
    const auto camera_hit = raycast_block(world, camera.position, camera_forward, 64.0F);
    const auto aim_point = camera_hit
        ? camera_hit->point
        : camera.position + camera_forward * 64.0F;
    const auto player_eye = player.position + Float3{0.0F, Player::eye_height, 0.0F};
    const auto player_ray = normalize(aim_point - player_eye);
    return raycast_block(world, player_eye, player_ray, 6.0F);
}

bool block_intersects_player(Int3 block, const Player& player) noexcept {
    const auto overlaps = [](float minimum_a, float maximum_a, float minimum_b, float maximum_b) {
        return minimum_a < maximum_b && maximum_a > minimum_b;
    };
    return overlaps(static_cast<float>(block.x), static_cast<float>(block.x + 1),
               player.position.x - Player::radius, player.position.x + Player::radius) &&
        overlaps(static_cast<float>(block.y), static_cast<float>(block.y + 1),
            player.position.y, player.position.y + Player::height) &&
        overlaps(static_cast<float>(block.z), static_cast<float>(block.z + 1),
            player.position.z - Player::radius, player.position.z + Player::radius);
}

} // namespace heartstead::game
