#pragma once

#include "engine/modding/generic_prototype.hpp"
#include "engine/renderer/particles/particle_system.hpp"

namespace heartstead::renderer {

[[nodiscard]] core::Result<ParticlePrototype>
particle_prototype_from_generic(const modding::GenericPrototype& prototype);

} // namespace heartstead::renderer
