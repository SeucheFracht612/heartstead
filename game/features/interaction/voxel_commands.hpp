#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/movement/player_controller.hpp"
#include "engine/net/server_command.hpp"
#include "engine/world/coords/world_coords.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace heartstead::game::interaction {

inline constexpr std::string_view place_voxel_command_type = "voxel.place";
inline constexpr std::string_view remove_voxel_command_type = "voxel.remove";

struct PlaceVoxelCommand {
    world::BlockCoord position;
    core::PrototypeId voxel;
};

struct RemoveVoxelCommand {
    world::BlockCoord position;
};

using VoxelPlacementValidator = std::function<core::Status(world::BlockCoord, world::VoxelCell)>;

class VoxelCommandTextCodec {
  public:
    [[nodiscard]] static std::string encode(const PlaceVoxelCommand& command);
    [[nodiscard]] static std::string encode(const RemoveVoxelCommand& command);
    [[nodiscard]] static core::Result<PlaceVoxelCommand> decode_place(std::string_view payload);
    [[nodiscard]] static core::Result<RemoveVoxelCommand> decode_remove(std::string_view payload);
};

[[nodiscard]] core::Status
execute_place_voxel(const PlaceVoxelCommand& command, const movement::PlayerControllerState& player,
                    const net::CommandEnvelope& envelope,
                    const net::CommandExecutionContext& context, world::WorldOperation& operation,
                    const VoxelPlacementValidator& validate_placement = {});

[[nodiscard]] core::Status execute_remove_voxel(const RemoveVoxelCommand& command,
                                                const movement::PlayerControllerState& player,
                                                const net::CommandEnvelope& envelope,
                                                const net::CommandExecutionContext& context,
                                                world::WorldOperation& operation);

} // namespace heartstead::game::interaction
