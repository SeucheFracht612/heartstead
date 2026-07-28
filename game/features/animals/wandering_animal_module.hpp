#pragma once

#include "engine/entities/entity_world.hpp"
#include "game/framework/gameplay_module.hpp"

#include <cstdint>
#include <string_view>

namespace heartstead::game::animals {

struct WanderingAnimalConfig {
    core::PrototypeId prototype_id;
    world::WorldPosition spawn{11.5, 1.0, 9.5};
    std::uint64_t seed = 0x4845415254535445ULL;
    double movement_speed = 1.4;
    double wander_radius = 4.0;
    std::uint32_t segment_ticks = 90;

    [[nodiscard]] core::Status validate() const;
};

struct WanderingAnimalComponent {
    world::WorldPosition origin;
    std::uint64_t seed = 0;
    std::uint32_t heading = 0;
    std::uint32_t ticks_remaining = 0;
    bool moving = true;
};

class WanderingAnimalModule final : public IGameplayModule {
  public:
    explicit WanderingAnimalModule(WanderingAnimalConfig config = {});

    [[nodiscard]] std::string_view module_id() const noexcept override;
    [[nodiscard]] core::Status
    validate_content(const modding::PrototypeRegistry& content) const override;
    [[nodiscard]] core::Status register_components(ComponentRegistry& registry) override;
    [[nodiscard]] core::Status register_systems(GameplayRegistrationContext& context) override;

  private:
    [[nodiscard]] core::Status update(simulation::SimulationContext& context);
    [[nodiscard]] core::Status spawn(simulation::SimulationContext& context);

    WanderingAnimalConfig config_;
    entities::EntityWorld* entities_ = nullptr;
    entities::EntityId entity_id_;
    core::RuntimeHandle runtime_handle_;
};

} // namespace heartstead::game::animals
