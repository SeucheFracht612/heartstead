#include "heartstead/game/player.hpp"

#include "heartstead/voxel/block_registry.hpp"
#include "heartstead/voxel/chunk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace heartstead::game {

bool player_collides(const ChunkWorld& world, Float3 position) noexcept {
    constexpr float epsilon = 0.001F;
    const auto blocks = BlockRegistry::defaults();
    const auto minimum_x = static_cast<std::int32_t>(std::floor(position.x - Player::radius + epsilon));
    const auto maximum_x = static_cast<std::int32_t>(std::floor(position.x + Player::radius - epsilon));
    const auto minimum_y = static_cast<std::int32_t>(std::floor(position.y + epsilon));
    const auto maximum_y = static_cast<std::int32_t>(std::floor(position.y + Player::height - epsilon));
    const auto minimum_z = static_cast<std::int32_t>(std::floor(position.z - Player::radius + epsilon));
    const auto maximum_z = static_cast<std::int32_t>(std::floor(position.z + Player::radius - epsilon));
    for (auto y = minimum_y; y <= maximum_y; ++y) {
        for (auto z = minimum_z; z <= maximum_z; ++z) {
            for (auto x = minimum_x; x <= maximum_x; ++x) {
                if (blocks.is_solid(world.get_block({x, y, z}))) return true;
            }
        }
    }
    return false;
}

float surface_height(const ChunkWorld& world, float x, float z) noexcept {
    const auto blocks = BlockRegistry::defaults();
    const auto block_x = static_cast<std::int32_t>(std::floor(x));
    const auto block_z = static_cast<std::int32_t>(std::floor(z));
    for (auto y = Chunk::edge - 1; y >= 0; --y) {
        if (blocks.is_solid(world.get_block({block_x, y, block_z})))
            return static_cast<float>(y + 1) + 0.001F;
    }
    return 40.0F;
}

void toggle_flight(Player& player) noexcept {
    player.movement_mode = player.movement_mode == MovementMode::walking
        ? MovementMode::flying
        : MovementMode::walking;
    player.velocity = {};
    player.grounded = false;
}

void update_player(
    Player& player,
    const Camera& camera,
    const ChunkWorld& world,
    const PlayerInput& input,
    float delta_seconds,
    CameraMode mode) {
    const Float3 horizontal_forward = normalize({std::cos(camera.yaw), 0.0F, std::sin(camera.yaw)});
    const auto right = normalize(cross(horizontal_forward, {0.0F, 1.0F, 0.0F}));
    if (player.movement_mode == MovementMode::flying) {
        auto movement = camera.forward() * input.forward + right * input.right;
        movement.y += (input.jump ? 1.0F : 0.0F) - (input.descend ? 1.0F : 0.0F);
        if (dot(movement, movement) > 0.0001F) movement = normalize(movement);
        const auto speed = input.sprint ? 24.0F : 12.0F;
        player.position = player.position + movement * (speed * delta_seconds);
        player.velocity = {};
        player.grounded = false;
        player.yaw = camera.yaw;
        return;
    }

    auto movement = horizontal_forward * input.forward + right * input.right;
    const auto movement_length_squared = dot(movement, movement);
    if (movement_length_squared > 0.0001F) {
        movement = normalize(movement);
        player.yaw = std::atan2(movement.z, movement.x);
    } else if (mode == CameraMode::first_person) {
        player.yaw = camera.yaw;
    }

    player.grounded = player_collides(world, player.position + Float3{0.0F, -0.06F, 0.0F});
    if (player.grounded && input.jump) player.velocity.y = 9.0F;
    player.velocity.y = std::max(player.velocity.y - 28.0F * delta_seconds, -45.0F);
    const auto speed = input.sprint ? 10.5F : 6.5F;
    const auto horizontal_distance = speed * delta_seconds;
    const auto vertical_distance = player.velocity.y * delta_seconds;
    const auto substeps = std::max(1, static_cast<std::int32_t>(std::ceil(
        std::max(horizontal_distance, std::abs(vertical_distance)) / 0.24F)));
    const auto horizontal_step = movement * (horizontal_distance / static_cast<float>(substeps));
    auto vertical_step = vertical_distance / static_cast<float>(substeps);
    for (std::int32_t step = 0; step < substeps; ++step) {
        const auto move_axis = [&](float Float3::* axis, float amount) {
            if (std::abs(amount) < 0.00001F) return;
            auto candidate = player.position;
            candidate.*axis += amount;
            if (!player_collides(world, candidate)) {
                player.position = candidate;
                return;
            }
            if (player.grounded) {
                candidate.y += 1.001F;
                if (!player_collides(world, candidate)) player.position = candidate;
            }
        };
        move_axis(&Float3::x, horizontal_step.x);
        move_axis(&Float3::z, horizontal_step.z);
        if (std::abs(vertical_step) > 0.00001F) {
            auto candidate = player.position;
            candidate.y += vertical_step;
            if (!player_collides(world, candidate)) {
                player.position = candidate;
            } else {
                player.velocity.y = 0.0F;
                vertical_step = 0.0F;
            }
        }
    }
    player.grounded = player_collides(world, player.position + Float3{0.0F, -0.06F, 0.0F});
    if (player.position.y < -16.0F) {
        player.position.y = surface_height(world, player.position.x, player.position.z);
        player.velocity = {};
    }
}

void update_camera_pose(
    Camera& camera,
    const Player& player,
    const ChunkWorld& world,
    CameraMode mode) {
    const auto target = player.position + Float3{0.0F, Player::eye_height, 0.0F};
    if (mode == CameraMode::first_person) {
        camera.position = target;
        return;
    }

    const auto forward = camera.forward();
    const auto right = normalize(cross(forward, {0.0F, 1.0F, 0.0F}));
    const auto desired = target - forward * 5.5F + right * 0.65F;
    const auto ray = desired - target;
    const auto distance = std::sqrt(dot(ray, ray));
    const auto direction = distance > 0.001F ? ray * (1.0F / distance) : Float3{};
    const auto blocks = BlockRegistry::defaults();
    camera.position = desired;
    for (float traveled = 0.25F; traveled < distance; traveled += 0.25F) {
        const auto sample = target + direction * traveled;
        const Int3 block_position{
            static_cast<std::int32_t>(std::floor(sample.x)),
            static_cast<std::int32_t>(std::floor(sample.y)),
            static_cast<std::int32_t>(std::floor(sample.z)),
        };
        if (!blocks.is_solid(world.get_block(block_position))) continue;
        camera.position = target + direction * std::max(0.0F, traveled - 0.35F);
        break;
    }
}

} // namespace heartstead::game
