#include "engine/modding/mod_fingerprint.hpp"

#include "engine/core/hash.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::modding {

namespace {

void add_field(core::StableHash64& hasher, std::string_view key, std::string_view value) noexcept {
    hasher.add_string(key);
    hasher.add_string("\x1F");
    hasher.add_string(value);
    hasher.add_string("\x1E");
}

[[nodiscard]] std::vector<const GenericPrototype*>
prototypes_for_mod(const ModManifest& mod, const std::vector<GenericPrototype>& prototypes) {
    std::vector<const GenericPrototype*> owned;
    for (const auto& prototype : prototypes) {
        if (prototype.id.is_valid() && prototype.id.namespace_id() == mod.id) {
            owned.push_back(&prototype);
        }
    }

    std::ranges::sort(owned, {},
                      [](const GenericPrototype* prototype) { return prototype->id.value(); });
    return owned;
}

[[nodiscard]] std::string patch_field_sort_key(const GenericPrototypePatch& patch) {
    std::string output;
    std::map<std::string, std::string> sorted_fields(patch.set_fields.begin(),
                                                     patch.set_fields.end());
    for (const auto& [key, value] : sorted_fields) {
        output += key;
        output += '\x1F';
        output += value;
        output += '\x1E';
    }
    return output;
}

[[nodiscard]] std::vector<const GenericPrototypePatch*>
patches_for_mod(const ModManifest& mod,
                const std::vector<GenericPrototypePatch>& prototype_patches) {
    std::vector<const GenericPrototypePatch*> owned;
    for (const auto& patch : prototype_patches) {
        if (patch.source_mod_id == mod.id) {
            owned.push_back(&patch);
        }
    }

    std::ranges::sort(owned,
                      [](const GenericPrototypePatch* left, const GenericPrototypePatch* right) {
                          if (left->target_id.value() != right->target_id.value()) {
                              return left->target_id.value() < right->target_id.value();
                          }
                          const auto left_stage = generic_prototype_patch_stage_name(left->stage);
                          const auto right_stage = generic_prototype_patch_stage_name(right->stage);
                          if (left_stage != right_stage) {
                              return left_stage < right_stage;
                          }
                          if (left->source.generic_string() != right->source.generic_string()) {
                              return left->source.generic_string() < right->source.generic_string();
                          }
                          return patch_field_sort_key(*left) < patch_field_sort_key(*right);
                      });
    return owned;
}

[[nodiscard]] std::vector<const scripting::ScriptModuleDesc*>
scripts_for_mod(const ModManifest& mod,
                const std::vector<scripting::ScriptModuleDesc>& script_modules) {
    std::vector<const scripting::ScriptModuleDesc*> owned;
    for (const auto& module : script_modules) {
        if (module.source_mod_id == mod.id) {
            owned.push_back(&module);
        }
    }
    std::ranges::sort(owned, [](const scripting::ScriptModuleDesc* left,
                                const scripting::ScriptModuleDesc* right) {
        if (left->stage != right->stage) {
            return left->stage < right->stage;
        }
        if (left->module_id != right->module_id) {
            return left->module_id < right->module_id;
        }
        return left->source_path.generic_string() < right->source_path.generic_string();
    });
    return owned;
}

[[nodiscard]] std::string
fingerprint_mod(const ModManifest& mod, const std::vector<const GenericPrototype*>& prototypes,
                const std::vector<const GenericPrototypePatch*>& patches,
                const std::vector<const scripting::ScriptModuleDesc*>& scripts) {
    core::StableHash64 hasher;
    add_field(hasher, "mod", mod.id);
    add_field(hasher, "prototype_count", std::to_string(prototypes.size()));
    add_field(hasher, "patch_count", std::to_string(patches.size()));
    // Preserve existing save fingerprints for mods that have no scripts.
    // Script-aware hashes extend the legacy stream only when script content
    // is actually present.
    if (!scripts.empty()) {
        add_field(hasher, "script_count", std::to_string(scripts.size()));
    }

    for (const auto* prototype : prototypes) {
        add_field(hasher, "prototype", prototype->id.value());
        add_field(hasher, "kind", prototype->kind);
        add_field(hasher, "display_name", prototype->display_name);

        std::map<std::string, std::string> sorted_fields(prototype->fields.begin(),
                                                         prototype->fields.end());
        for (const auto& [key, value] : sorted_fields) {
            add_field(hasher, key, value);
        }
    }

    for (const auto* patch : patches) {
        add_field(hasher, "patch_target", patch->target_id.value());
        add_field(hasher, "patch_stage", generic_prototype_patch_stage_name(patch->stage));

        std::map<std::string, std::string> sorted_fields(patch->set_fields.begin(),
                                                         patch->set_fields.end());
        for (const auto& [key, value] : sorted_fields) {
            add_field(hasher, "patch.set." + key, value);
        }
    }

    for (const auto* script : scripts) {
        add_field(hasher, "script_module", script->module_id);
        add_field(hasher, "script_stage", scripting::script_stage_name(script->stage));
        add_field(hasher, "script_api_version", std::to_string(script->api_version));
        add_field(hasher, "script_source", script->source);
        std::vector<std::string_view> permissions;
        permissions.reserve(script->permissions.size());
        for (const auto permission : script->permissions) {
            permissions.push_back(scripting::script_permission_name(permission));
        }
        std::ranges::sort(permissions);
        for (const auto permission : permissions) {
            add_field(hasher, "script_permission", permission);
        }
    }

    return hasher.hex();
}

} // namespace

std::vector<ModPrototypeFingerprint>
build_mod_prototype_fingerprints(const std::vector<ModManifest>& mods,
                                 const std::vector<GenericPrototype>& prototypes) {
    return build_mod_prototype_fingerprints(mods, prototypes, {}, {});
}

std::vector<ModPrototypeFingerprint>
build_mod_prototype_fingerprints(const std::vector<ModManifest>& mods,
                                 const std::vector<GenericPrototype>& prototypes,
                                 const std::vector<GenericPrototypePatch>& prototype_patches) {
    return build_mod_prototype_fingerprints(mods, prototypes, prototype_patches, {});
}

std::vector<ModPrototypeFingerprint>
build_mod_prototype_fingerprints(const std::vector<ModManifest>& mods,
                                 const std::vector<GenericPrototype>& prototypes,
                                 const std::vector<GenericPrototypePatch>& prototype_patches,
                                 const std::vector<scripting::ScriptModuleDesc>& script_modules) {
    std::vector<ModPrototypeFingerprint> fingerprints;
    fingerprints.reserve(mods.size());

    for (const auto& mod : mods) {
        auto owned_prototypes = prototypes_for_mod(mod, prototypes);
        auto owned_patches = patches_for_mod(mod, prototype_patches);
        auto owned_scripts = scripts_for_mod(mod, script_modules);
        fingerprints.push_back(ModPrototypeFingerprint{
            mod.id,
            mod.version,
            fingerprint_mod(mod, owned_prototypes, owned_patches, owned_scripts),
            owned_prototypes.size(),
            owned_patches.size(),
            owned_scripts.size(),
        });
    }

    std::ranges::sort(fingerprints, {}, &ModPrototypeFingerprint::id);
    return fingerprints;
}

} // namespace heartstead::modding
