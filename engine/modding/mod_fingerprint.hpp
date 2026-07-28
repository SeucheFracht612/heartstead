#pragma once

#include "engine/modding/generic_prototype.hpp"
#include "engine/modding/mod_manifest.hpp"
#include "engine/scripting/script_runtime.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace heartstead::modding {

struct ModPrototypeFingerprint {
    std::string id;
    std::string version;
    std::string prototype_hash;
    std::size_t prototype_count = 0;
    std::size_t patch_count = 0;
    std::size_t script_count = 0;
};

[[nodiscard]] std::vector<ModPrototypeFingerprint>
build_mod_prototype_fingerprints(const std::vector<ModManifest>& mods,
                                 const std::vector<GenericPrototype>& prototypes);
[[nodiscard]] std::vector<ModPrototypeFingerprint>
build_mod_prototype_fingerprints(const std::vector<ModManifest>& mods,
                                 const std::vector<GenericPrototype>& prototypes,
                                 const std::vector<GenericPrototypePatch>& prototype_patches);
[[nodiscard]] std::vector<ModPrototypeFingerprint>
build_mod_prototype_fingerprints(const std::vector<ModManifest>& mods,
                                 const std::vector<GenericPrototype>& prototypes,
                                 const std::vector<GenericPrototypePatch>& prototype_patches,
                                 const std::vector<scripting::ScriptModuleDesc>& script_modules);

} // namespace heartstead::modding
