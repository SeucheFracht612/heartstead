#include "game/features/interaction/voxel_interaction_module.hpp"

#include "engine/world/voxel_change.hpp"
#include "game/features/interaction/voxel_commands.hpp"

#include <string>
#include <utility>

namespace heartstead::game::interaction {

VoxelInteractionModule::VoxelInteractionModule(AuthoritativePlayerResolver resolve_player,
                                               VoxelPlacementValidator validate_placement)
    : resolve_player_(std::move(resolve_player)),
      validate_placement_(std::move(validate_placement)) {}

std::string_view VoxelInteractionModule::module_id() const noexcept {
    return "base.voxel_interaction";
}

core::Status VoxelInteractionModule::register_commands(GameplayRegistrationContext& context) {
    if (!resolve_player_ || !validate_placement_) {
        return core::Status::failure("voxel_interaction.missing_player_resolver",
                                     "voxel interaction requires authoritative player and "
                                     "placement validation");
    }
    auto status = context.commands.register_command(net::CommandDescriptor{
        std::string(place_voxel_command_type),
        true,
        true,
        [resolve_player = resolve_player_, validate_placement = validate_placement_](
            const net::CommandEnvelope& command,
            const net::CommandExecutionContext& command_context, world::WorldOperation& operation) {
            const auto* player = resolve_player(command.sender);
            if (player == nullptr) {
                return core::Status::failure("server_runtime.player_not_connected",
                                             "voxel command sender has no active player");
            }
            auto decoded = VoxelCommandTextCodec::decode_place(command.payload);
            if (!decoded) {
                return core::Status::failure(decoded.error().code, decoded.error().message);
            }
            return execute_place_voxel(decoded.value(), player->state, command, command_context,
                                       operation, validate_placement);
        },
    });
    if (!status) {
        return status;
    }
    return context.commands.register_command(net::CommandDescriptor{
        std::string(remove_voxel_command_type),
        true,
        true,
        [resolve_player = resolve_player_](const net::CommandEnvelope& command,
                                           const net::CommandExecutionContext& command_context,
                                           world::WorldOperation& operation) {
            const auto* player = resolve_player(command.sender);
            if (player == nullptr) {
                return core::Status::failure("server_runtime.player_not_connected",
                                             "voxel command sender has no active player");
            }
            auto decoded = VoxelCommandTextCodec::decode_remove(command.payload);
            if (!decoded) {
                return core::Status::failure(decoded.error().code, decoded.error().message);
            }
            return execute_remove_voxel(decoded.value(), player->state, player->save_id, command,
                                        command_context, operation);
        },
    });
}

core::Status VoxelInteractionModule::register_serializers(SerializationRegistry& registry) {
    auto status = registry.register_schema({std::string(place_voxel_command_type), 1});
    return status ? registry.register_schema({std::string(remove_voxel_command_type), 1}) : status;
}

core::Status VoxelInteractionModule::register_replication(ReplicationRegistry& registry) {
    auto status = registry.register_replication(
        {std::string(world::voxel_changed_event_type), 1, true, true, {}});
    return status ? registry.register_replication(
                        {std::string(voxel_resource_granted_event_type), 1, true, true, {}})
                  : status;
}

} // namespace heartstead::game::interaction
