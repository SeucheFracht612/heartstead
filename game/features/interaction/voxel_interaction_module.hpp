#pragma once

#include "engine/movement/player_controller_store.hpp"
#include "game/features/interaction/voxel_commands.hpp"
#include "game/framework/gameplay_module.hpp"

#include <functional>

namespace heartstead::game::interaction {

using AuthoritativePlayerResolver =
    std::function<const movement::PlayerControllerRecord*(core::NetId)>;

class VoxelInteractionModule final : public IGameplayModule {
  public:
    VoxelInteractionModule(AuthoritativePlayerResolver resolve_player,
                           VoxelPlacementValidator validate_placement);

    [[nodiscard]] std::string_view module_id() const noexcept override;
    [[nodiscard]] core::Status register_commands(GameplayRegistrationContext& context) override;
    [[nodiscard]] core::Status register_serializers(SerializationRegistry& registry) override;
    [[nodiscard]] core::Status register_replication(ReplicationRegistry& registry) override;

  private:
    AuthoritativePlayerResolver resolve_player_;
    VoxelPlacementValidator validate_placement_;
};

} // namespace heartstead::game::interaction
