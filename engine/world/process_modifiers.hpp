#pragma once

#include "engine/core/result.hpp"
#include "engine/processes/process.hpp"

namespace heartstead::modding {
class PrototypeRegistry;
}

namespace heartstead::world {

class WorldState;

[[nodiscard]] core::Result<processes::ProcessModifiers>
resolve_authoritative_process_modifiers(const WorldState& state,
                                        const modding::PrototypeRegistry& registry,
                                        const processes::ProcessInstance& process);

} // namespace heartstead::world
