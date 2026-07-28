#pragma once

#include "engine/assemblies/assembly.hpp"
#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/resource_pack.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/cargo/cargo.hpp"
#include "engine/core/result.hpp"
#include "engine/entities/entity.hpp"
#include "engine/items/item_stack.hpp"
#include "engine/modding/generic_prototype.hpp"
#include "engine/modding/mod_diagnostic.hpp"
#include "engine/modding/mod_fingerprint.hpp"
#include "engine/modding/mod_lifecycle.hpp"
#include "engine/modding/mod_manifest.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/processes/process.hpp"
#include "engine/renderer/materials/material_asset_validation.hpp"
#include "engine/renderer/materials/material_definition.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/rooms/room_descriptor_prototype.hpp"
#include "engine/save/save_metadata.hpp"
#include "engine/scenarios/scenario.hpp"
#include "engine/scripting/script_runtime.hpp"
#include "engine/workpieces/workpiece_definition.hpp"
#include "engine/ui/widget_tree.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::content {

struct ContentValidationReport {
    std::vector<modding::ModManifest> mods;
    std::vector<assets::ResourcePackManifest> resource_packs;
    assets::ResourcePackLoadPlan resource_pack_load_plan;
    std::vector<modding::GenericPrototype> prototypes;
    std::vector<modding::GenericPrototypePatch> prototype_patches;
    std::size_t applied_patch_count = 0;
    std::vector<modding::ModPrototypeFingerprint> mod_fingerprints;
    modding::ModLifecyclePlan lifecycle_plan;
    std::vector<scripting::ScriptModuleDesc> script_modules;
    modding::PrototypeRegistry registry;
    assets::AssetCatalog asset_catalog;
    audio::SoundEventRegistry sound_events;
    std::vector<items::ItemDefinition> item_definitions;
    std::vector<cargo::CargoDefinition> cargo_definitions;
    std::vector<entities::EntityDefinition> entity_definitions;
    world::VoxelPalette voxel_palette;
    std::vector<assemblies::AssemblyDefinition> assembly_definitions;
    std::vector<processes::ProcessDefinition> process_definitions;
    std::vector<rooms::RoomDescriptorDefinition> room_descriptor_definitions;
    std::vector<workpieces::WorkpieceDefinition> workpiece_definitions;
    std::vector<scenarios::ScenarioDefinition> scenario_definitions;
    renderer::materials::MaterialRegistry material_registry;
    renderer::materials::MaterialAssetValidationResult material_assets;
    std::vector<renderer::ParticlePrototype> particle_prototypes;
    ui::UiSkin ui_skin;
    std::vector<modding::ModDiagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] std::size_t count_severity(modding::DiagnosticSeverity severity) const noexcept;
    [[nodiscard]] std::size_t count_kind(std::string_view kind) const noexcept;
};

class ContentValidation {
  public:
    [[nodiscard]] static ContentValidationReport validate(const std::filesystem::path& source_root);
    [[nodiscard]] static ContentValidationReport
    validate(const std::filesystem::path& mods_root,
             const std::filesystem::path& resource_packs_root);
};

[[nodiscard]] core::Result<save::SaveMetadata>
save_metadata_from_content_report(const ContentValidationReport& report, std::string game_version,
                                  std::uint64_t world_seed);

} // namespace heartstead::content
