#include "game/features/animals/wandering_animal_module.hpp"

#include "engine/entities/entity_prototype.hpp"
#include "engine/world/world_state.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::game::animals {

namespace {

[[nodiscard]] std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] math::Vec3d direction(std::uint32_t heading) noexcept {
    constexpr double diagonal = 0.70710678118654752440;
    constexpr std::array<math::Vec3d, 8> directions{
        math::Vec3d{0.0, 0.0, 1.0},  math::Vec3d{diagonal, 0.0, diagonal},
        math::Vec3d{1.0, 0.0, 0.0},  math::Vec3d{diagonal, 0.0, -diagonal},
        math::Vec3d{0.0, 0.0, -1.0}, math::Vec3d{-diagonal, 0.0, -diagonal},
        math::Vec3d{-1.0, 0.0, 0.0}, math::Vec3d{-diagonal, 0.0, diagonal},
    };
    return directions[heading % directions.size()];
}

void transition_locomotion(animation::ReplicatedLocomotionAnimation& locomotion,
                           animation::LocomotionAnimationKind kind, std::uint64_t tick) noexcept {
    if (locomotion.kind == kind) {
        return;
    }
    locomotion.transition_from = locomotion.kind;
    locomotion.transition_from_phase = locomotion.phase;
    locomotion.transition_tick = tick;
    locomotion.kind = kind;
    locomotion.phase = 0;
}

} // namespace

core::Status WanderingAnimalConfig::validate() const {
    if (!prototype_id.is_valid() || !spawn.is_valid() || !player_offset.is_finite() ||
        !std::isfinite(movement_speed) || !std::isfinite(wander_radius) ||
        movement_speed <= 0.0 || wander_radius <= 0.0 || segment_ticks == 0 ||
        segment_ticks > 60U * 60U) {
        return core::Status::failure(
            "wandering_animal.invalid_config",
            "wandering animal needs a prototype, spawn, positive motion, and bounded segments");
    }
    return core::Status::ok();
}

WanderingAnimalModule::WanderingAnimalModule(WanderingAnimalConfig config)
    : config_(std::move(config)) {
    if (!config_.prototype_id.is_valid()) {
        config_.prototype_id = *core::PrototypeId::parse("base:entities/test_animal");
    }
}

std::string_view WanderingAnimalModule::module_id() const noexcept {
    return "base.wandering_animal";
}

core::Status
WanderingAnimalModule::validate_content(const modding::PrototypeRegistry& content) const {
    auto status = config_.validate();
    if (!status) {
        return status;
    }
    const auto* prototype = content.find(config_.prototype_id);
    if (prototype == nullptr) {
        return core::Status::failure("wandering_animal.missing_prototype",
                                     "wandering animal prototype is not loaded");
    }
    auto definition = entities::entity_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Status::failure(definition.error().code, definition.error().message);
    }
    if (definition.value().kind != entities::EntityKind::animal) {
        return core::Status::failure("wandering_animal.invalid_prototype_kind",
                                     "wandering animal prototype must have animal entity kind");
    }
    return core::Status::ok();
}

core::Status WanderingAnimalModule::register_components(ComponentRegistry& registry) {
    auto status =
        registry.register_component<entities::NetworkIdentityComponent>("engine.network_identity");
    if (status) {
        status =
            registry.register_component<entities::TransformComponent>("engine.world_transform");
    }
    if (status) {
        status = registry.register_component<entities::LocomotionAnimationComponent>(
            "engine.locomotion_animation");
    }
    if (status) {
        status =
            registry.register_component<WanderingAnimalComponent>("base.wandering_animal_state");
    }
    return status;
}

core::Status WanderingAnimalModule::register_systems(GameplayRegistrationContext& context) {
    entities_ = &context.entities;
    return context.scheduler.register_system({
        "base.wandering_animal",
        simulation::SimulationPhase::gameplay,
        {},
        [this](simulation::SimulationContext& simulation) { return update(simulation); },
    });
}

core::Status WanderingAnimalModule::spawn(simulation::SimulationContext& context) {
    if (context.world == nullptr || entities_ == nullptr) {
        return core::Status::failure("wandering_animal.missing_world",
                                     "wandering animal system requires simulation worlds");
    }
    auto runtime_handle = context.world->runtime_handles().reserve();
    auto net_id = context.world->entity_net_ids().reserve();
    if (!runtime_handle || !net_id) {
        const auto& error = !runtime_handle ? runtime_handle.error() : net_id.error();
        return core::Status::failure(error.code, error.message);
    }
    auto spawn = config_.spawn;
    if (config_.place_near_first_player) {
        const auto players = context.world->entities().records();
        const auto player = std::ranges::find_if(players, [](const entities::EntityRecord* record) {
            return record != nullptr && record->kind == entities::EntityKind::player;
        });
        if (player != players.end()) {
            auto relative_spawn = world::WorldPosition::from_anchor(
                (*player)->transform.position.anchor,
                (*player)->transform.position.local_offset + config_.player_offset);
            if (!relative_spawn) {
                return core::Status::failure(relative_spawn.error().code,
                                             relative_spawn.error().message);
            }
            spawn = std::move(relative_spawn).value();
        }
    }

    entities::EntityRecord record;
    record.runtime_handle = runtime_handle.value();
    record.net_id = net_id.value();
    record.prototype_id = config_.prototype_id;
    record.kind = entities::EntityKind::animal;
    record.transform.position = spawn;
    record.transform.scale = {0.72, 0.72, 0.72};
    auto status = context.world->entities().insert(record);
    if (!status) {
        return status;
    }

    auto created = entities_->create_entity(config_.prototype_id);
    if (!created) {
        (void)context.world->entities().erase(runtime_handle.value());
        return core::Status::failure(created.error().code, created.error().message);
    }
    entity_id_ = created.value();
    runtime_handle_ = runtime_handle.value();
    const auto cleanup = [&] {
        (void)entities_->destroy_entity(entity_id_);
        (void)entities_->finalize_destruction(context.tick, context.events);
        (void)context.world->entities().erase(runtime_handle_);
        entity_id_ = {};
        runtime_handle_ = {};
    };
    auto identity =
        entities_->emplace<entities::NetworkIdentityComponent>(entity_id_, net_id.value());
    auto transform = entities_->emplace<entities::TransformComponent>(
        entity_id_, entities::TransformComponent{record.transform, record.transform});
    auto locomotion = entities_->emplace<entities::LocomotionAnimationComponent>(entity_id_);
    auto wandering = entities_->emplace<WanderingAnimalComponent>(
        entity_id_, WanderingAnimalComponent{spawn, config_.seed,
                                             static_cast<std::uint32_t>(mix(config_.seed) % 8U),
                                             config_.segment_ticks, true});
    if (!identity || !transform || !locomotion || !wandering) {
        const auto& error = !identity     ? identity.error()
                            : !transform  ? transform.error()
                            : !locomotion ? locomotion.error()
                                          : wandering.error();
        cleanup();
        return core::Status::failure(error.code, error.message);
    }
    status = entities_->activate_entity(entity_id_, context.events);
    if (!status) {
        cleanup();
        return status;
    }
    return core::Status::ok();
}

core::Status WanderingAnimalModule::update(simulation::SimulationContext& context) {
    if (!entity_id_.is_valid()) {
        auto status = spawn(context);
        if (!status) {
            return status;
        }
    }
    auto* transform = entities_->find_component<entities::TransformComponent>(entity_id_);
    auto* locomotion =
        entities_->find_component<entities::LocomotionAnimationComponent>(entity_id_);
    auto* wandering = entities_->find_component<WanderingAnimalComponent>(entity_id_);
    auto* world_record =
        context.world == nullptr ? nullptr : context.world->entities().find(runtime_handle_);
    if (transform == nullptr || locomotion == nullptr || wandering == nullptr ||
        world_record == nullptr) {
        return core::Status::failure(
            "wandering_animal.missing_state",
            "wandering animal entity lost a required component or world record");
    }

    if (wandering->ticks_remaining == 0) {
        const auto choice = mix(wandering->seed ^ context.tick);
        wandering->heading = static_cast<std::uint32_t>(choice % 8U);
        wandering->moving = ((choice >> 8U) % 3U) != 0;
        wandering->ticks_remaining = config_.segment_ticks;
    }
    transform->previous = transform->current;
    const auto heading = direction(wandering->heading);
    if (wandering->moving) {
        const auto distance = config_.movement_speed * context.fixed_delta_seconds;
        auto position = world::WorldPosition::from_anchor(transform->current.position.anchor,
                                                          transform->current.position.local_offset +
                                                              heading * distance);
        if (!position) {
            return core::Status::failure(position.error().code, position.error().message);
        }
        const auto from_origin =
            position.value().relative_to(wandering->origin.anchor) - wandering->origin.local_offset;
        if (std::hypot(from_origin.x, from_origin.z) > config_.wander_radius) {
            wandering->heading = (wandering->heading + 4U) % 8U;
            const auto reflected = direction(wandering->heading);
            position = world::WorldPosition::from_anchor(transform->current.position.anchor,
                                                         transform->current.position.local_offset +
                                                             reflected * distance);
            if (!position) {
                return core::Status::failure(position.error().code, position.error().message);
            }
        }
        transform->current.position = position.value();
        const auto active_heading = direction(wandering->heading);
        constexpr double radians_to_degrees = 57.2957795130823208768;
        transform->current.rotation_degrees.y =
            std::atan2(active_heading.x, active_heading.z) * radians_to_degrees;
    }
    --wandering->ticks_remaining;

    const auto kind = wandering->moving ? animation::LocomotionAnimationKind::walk
                                        : animation::LocomotionAnimationKind::idle;
    transition_locomotion(locomotion->state, kind, context.tick);
    if (kind == animation::LocomotionAnimationKind::walk) {
        const auto traveled = math::length(
            transform->current.position.relative_to(transform->previous.position.anchor) -
            transform->previous.position.local_offset);
        const auto advance = static_cast<std::uint64_t>(std::llround(traveled * 0.7 * 65'536.0));
        locomotion->state.phase = static_cast<std::uint16_t>(
            (static_cast<std::uint64_t>(locomotion->state.phase) + advance) & 0xFFFFU);
    } else {
        locomotion->state.phase = 0;
    }
    world_record->transform = transform->current;
    return locomotion->state.validate(context.tick);
}

} // namespace heartstead::game::animals
