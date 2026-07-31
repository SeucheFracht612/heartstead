#pragma once

#include "engine/core/result.hpp"
#include "engine/modding/generic_prototype.hpp"
#include "engine/modding/mod_diagnostic.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::modding {

struct PrototypeKinds {
    static constexpr std::string_view item = "item";
    static constexpr std::string_view cargo = "cargo";
    static constexpr std::string_view entity = "entity";
    static constexpr std::string_view entity_visual = "entity_visual";
    static constexpr std::string_view visual_prefab = "visual_prefab";
    static constexpr std::string_view voxel = "voxel";
    static constexpr std::string_view block_model = "block_model";
    static constexpr std::string_view build_piece = "build_piece";
    static constexpr std::string_view assembly = "assembly";
    static constexpr std::string_view workpiece = "workpiece";
    static constexpr std::string_view pattern = "pattern";
    static constexpr std::string_view process = "process";
    static constexpr std::string_view fire = "fire";
    static constexpr std::string_view room_descriptor = "room_descriptor";
    static constexpr std::string_view material = "material";
    static constexpr std::string_view scenario = "scenario";
    // Gameplay systems materialize these through their own public loaders.  The engine registry
    // must still preserve, patch, fingerprint and inspect them before those systems exist.
    static constexpr std::string_view recipe = "recipe";
    static constexpr std::string_view biome = "biome";
    static constexpr std::string_view world_feature = "world_feature";
    static constexpr std::string_view crop = "crop";
    static constexpr std::string_view animal = "animal";
    static constexpr std::string_view map_layer = "map_layer";
    static constexpr std::string_view ui_panel = "ui_panel";
    static constexpr std::string_view network = "network";
    static constexpr std::string_view ward = "ward";
    static constexpr std::string_view admin_command = "admin_command";
    static constexpr std::string_view sound_event = "sound_event";
    static constexpr std::string_view particle = "particle";
    static constexpr std::string_view environment_profile = "environment_profile";
    static constexpr std::string_view vegetation_species = "vegetation_species";
    static constexpr std::string_view decal = "decal";
};

struct PrototypeRegistryBuildResult {
    std::vector<ModDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
};

class PrototypeRegistry {
  public:
    [[nodiscard]] PrototypeRegistryBuildResult build(std::vector<GenericPrototype> prototypes);

    [[nodiscard]] const GenericPrototype* find(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] bool contains(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t count_kind(std::string_view kind) const noexcept;
    [[nodiscard]] std::vector<const GenericPrototype*>
    prototypes_of_kind(std::string_view kind) const;

    [[nodiscard]] core::Status require(const core::PrototypeId& id) const;
    [[nodiscard]] core::Status require_kind(const core::PrototypeId& id,
                                            std::string_view expected_kind) const;

  private:
    std::vector<GenericPrototype> prototypes_;
    std::unordered_map<std::string, std::size_t> by_id_;
    std::unordered_map<std::string, std::vector<std::size_t>> by_kind_;
};

} // namespace heartstead::modding
