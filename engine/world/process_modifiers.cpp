#include "engine/world/process_modifiers.hpp"

#include "engine/modding/prototype_registry.hpp"
#include "engine/processes/process_environment.hpp"
#include "engine/processes/process_prototype.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace heartstead::world {

namespace {

[[nodiscard]] std::uint32_t clamp_power_capacity(std::uint64_t capacity) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(capacity, std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] std::uint32_t available_power_capacity_for_owner(const WorldState& state,
                                                               core::SaveId owner_id) noexcept {
    const auto* power = state.networks().find(networks::NetworkKind::power);
    if (power == nullptr) {
        return 0;
    }
    return clamp_power_capacity(power->total_port_capacity_for_owner(owner_id));
}

[[nodiscard]] core::Result<processes::ProcessDefinition>
definition_for_process(const modding::PrototypeRegistry& registry,
                       const processes::ProcessInstance& process) {
    auto prototype_status =
        registry.require_kind(process.prototype_id, modding::PrototypeKinds::process);
    if (!prototype_status) {
        return core::Result<processes::ProcessDefinition>::failure(
            prototype_status.error().code, prototype_status.error().message);
    }

    const auto* prototype = registry.find(process.prototype_id);
    if (prototype == nullptr) {
        return core::Result<processes::ProcessDefinition>::failure(
            "world_command.missing_process_prototype", "process prototype is missing");
    }

    auto definition = processes::process_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Result<processes::ProcessDefinition>::failure(definition.error().code,
                                                                   definition.error().message);
    }
    return core::Result<processes::ProcessDefinition>::success(std::move(definition).value());
}

} // namespace

core::Result<processes::ProcessModifiers>
resolve_authoritative_process_modifiers(const WorldState& state,
                                        const modding::PrototypeRegistry& registry,
                                        const processes::ProcessInstance& process) {
    auto definition = definition_for_process(registry, process);
    if (!definition) {
        return core::Result<processes::ProcessModifiers>::failure(definition.error().code,
                                                                  definition.error().message);
    }

    processes::ProcessEnvironmentDesc desc;
    desc.owner_id = process.owner_id;
    desc.room_graph = &state.rooms();
    desc.requires_room = definition.value().requires_room;
    desc.requires_power = definition.value().requires_power;
    desc.available_power_capacity = available_power_capacity_for_owner(state, process.owner_id);
    desc.required_power_capacity = definition.value().required_power_capacity;
    desc.base_quality_rate_per_mille = definition.value().base_quality_rate_per_mille;

    auto resolved = processes::ProcessEnvironmentResolver::resolve(desc);
    if (!resolved) {
        return core::Result<processes::ProcessModifiers>::failure(resolved.error().code,
                                                                  resolved.error().message);
    }
    return core::Result<processes::ProcessModifiers>::success(resolved.value().modifiers);
}

} // namespace heartstead::world
