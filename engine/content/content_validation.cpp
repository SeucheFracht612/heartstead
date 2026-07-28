#include "engine/content/content_validation.hpp"

#include "engine/assemblies/assembly_prototype.hpp"
#include "engine/cargo/cargo_prototype.hpp"
#include "engine/entities/entity_prototype.hpp"
#include "engine/items/item_prototype.hpp"
#include "engine/modding/mod_validation.hpp"
#include "engine/processes/process_prototype.hpp"
#include "engine/renderer/materials/material_prototype_loader.hpp"
#include "engine/rooms/room_descriptor_prototype.hpp"
#include "engine/scenarios/scenario_prototype.hpp"
#include "engine/workpieces/workpiece_prototype.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace heartstead::content {

namespace {

void append_diagnostics_copy(std::vector<modding::ModDiagnostic>& destination,
                             const std::vector<modding::ModDiagnostic>& diagnostics) {
    destination.insert(destination.end(), diagnostics.begin(), diagnostics.end());
}

void append_diagnostics_move(std::vector<modding::ModDiagnostic>& destination,
                             std::vector<modding::ModDiagnostic> diagnostics) {
    destination.insert(destination.end(), std::make_move_iterator(diagnostics.begin()),
                       std::make_move_iterator(diagnostics.end()));
}

void add_error(ContentValidationReport& report, std::filesystem::path source, std::string code,
               std::string message) {
    report.diagnostics.push_back(modding::ModDiagnostic{
        modding::DiagnosticSeverity::error,
        std::move(source),
        std::move(code),
        std::move(message),
    });
}

void index_assets(ContentValidationReport& report, const std::filesystem::path& root,
                  std::string namespace_id, assets::AssetSourceKind source_kind,
                  std::string source_id, std::uint32_t priority) {
    auto indexed = assets::AssetCatalogBuilder::index_directory(
        report.asset_catalog, root, std::move(namespace_id), source_kind, std::move(source_id),
        priority);
    append_diagnostics_move(report.diagnostics, std::move(indexed.diagnostics));
}

} // namespace

bool ContentValidationReport::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const modding::ModDiagnostic& diagnostic) {
        return diagnostic.severity == modding::DiagnosticSeverity::error;
    });
}

std::size_t
ContentValidationReport::count_severity(modding::DiagnosticSeverity severity) const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(diagnostics, [severity](const modding::ModDiagnostic& diagnostic) {
            return diagnostic.severity == severity;
        }));
}

std::size_t ContentValidationReport::count_kind(std::string_view kind) const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(prototypes, [kind](const modding::GenericPrototype& prototype) {
            return prototype.kind == kind;
        }));
}

ContentValidationReport ContentValidation::validate(const std::filesystem::path& source_root) {
    return validate(source_root / "mods", source_root / "resource_packs");
}

ContentValidationReport
ContentValidation::validate(const std::filesystem::path& mods_root,
                            const std::filesystem::path& resource_packs_root) {
    ContentValidationReport report;

    auto mod_report = modding::ModValidation::validate(mods_root);
    report.mods = std::move(mod_report.mods);
    report.prototypes = std::move(mod_report.prototypes);
    report.prototype_patches = std::move(mod_report.prototype_patches);
    report.applied_patch_count = mod_report.applied_patch_count;
    report.mod_fingerprints = std::move(mod_report.mod_fingerprints);
    report.lifecycle_plan = std::move(mod_report.lifecycle_plan);
    report.script_modules = std::move(mod_report.script_modules);
    report.registry = std::move(mod_report.registry);
    append_diagnostics_move(report.diagnostics, std::move(mod_report.diagnostics));

    for (std::size_t index = 0; index < report.mods.size(); ++index) {
        const auto& mod = report.mods[index];
        index_assets(report, mod.root / "assets", mod.id, assets::AssetSourceKind::mod, mod.id,
                     static_cast<std::uint32_t>(index + 1));
    }

    assets::ResourcePackDiscoverer resource_pack_discoverer;
    auto resource_pack_discovery = resource_pack_discoverer.discover(resource_packs_root);
    append_diagnostics_move(report.diagnostics, std::move(resource_pack_discovery.diagnostics));

    auto resource_pack_plan =
        assets::ResourcePackLoadPlanner::plan(std::move(resource_pack_discovery.packs));
    if (!resource_pack_plan) {
        add_error(report, resource_packs_root, resource_pack_plan.error().code,
                  resource_pack_plan.error().message);
    } else {
        report.resource_pack_load_plan = std::move(resource_pack_plan).value();
    }

    for (const auto& entry : report.resource_pack_load_plan.entries) {
        report.resource_packs.push_back(entry.manifest);
        auto indexed = assets::ResourcePackPolicy::index_assets(
            report.asset_catalog, entry.manifest, entry.asset_priority);
        append_diagnostics_move(report.diagnostics, std::move(indexed.diagnostics));
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::item)) {
        auto definition = items::item_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.item_definitions.push_back(std::move(definition).value());
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::cargo)) {
        auto definition = cargo::cargo_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.cargo_definitions.push_back(std::move(definition).value());
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::entity)) {
        auto definition = entities::entity_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.entity_definitions.push_back(std::move(definition).value());
    }

    auto voxel_palette = world::voxel_palette_from_prototypes(report.registry);
    if (!voxel_palette) {
        add_error(report, mods_root, voxel_palette.error().code, voxel_palette.error().message);
    } else {
        report.voxel_palette = std::move(voxel_palette).value();
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::assembly)) {
        auto definition = assemblies::assembly_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.assembly_definitions.push_back(std::move(definition).value());
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::process)) {
        auto definition = processes::process_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.process_definitions.push_back(std::move(definition).value());
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::room_descriptor)) {
        auto definition = rooms::room_descriptor_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.room_descriptor_definitions.push_back(std::move(definition).value());
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::workpiece)) {
        auto definition = workpieces::workpiece_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.workpiece_definitions.push_back(std::move(definition).value());
    }

    for (const auto* prototype :
         report.registry.prototypes_of_kind(modding::PrototypeKinds::scenario)) {
        auto definition = scenarios::scenario_definition_from_prototype(*prototype);
        if (!definition) {
            add_error(report, prototype->source, definition.error().code,
                      definition.error().message);
            continue;
        }
        report.scenario_definitions.push_back(std::move(definition).value());
    }

    auto materials = renderer::materials::material_registry_from_prototypes(report.registry);
    if (!materials) {
        add_error(report, mods_root, materials.error().code, materials.error().message);
        return report;
    }
    report.material_registry = std::move(materials).value();

    report.material_assets = renderer::materials::validate_material_asset_references(
        report.material_registry, report.asset_catalog);
    append_diagnostics_copy(report.diagnostics, report.material_assets.diagnostics);

    auto sound_events =
        audio::sound_event_registry_from_prototypes(report.registry, report.asset_catalog);
    if (!sound_events) {
        add_error(report, mods_root, sound_events.error().code, sound_events.error().message);
        return report;
    }
    report.sound_events = std::move(sound_events).value();

    return report;
}

core::Result<save::SaveMetadata>
save_metadata_from_content_report(const ContentValidationReport& report, std::string game_version,
                                  std::uint64_t world_seed) {
    if (report.has_errors()) {
        return core::Result<save::SaveMetadata>::failure(
            "content.invalid_for_save_metadata",
            "cannot build save metadata from content validation errors");
    }
    if (report.mod_fingerprints.empty()) {
        return core::Result<save::SaveMetadata>::failure(
            "content.missing_mod_fingerprints",
            "save metadata requires active mod prototype fingerprints");
    }

    save::SaveMetadata metadata;
    metadata.schema_version = save::current_save_schema_version;
    metadata.game_version = std::move(game_version);
    metadata.world_seed = world_seed;
    metadata.enabled_mods = save::saved_mod_records_from_fingerprints(report.mod_fingerprints);

    auto status = metadata.validate();
    if (!status) {
        return core::Result<save::SaveMetadata>::failure(status.error().code,
                                                         status.error().message);
    }
    return core::Result<save::SaveMetadata>::success(std::move(metadata));
}

} // namespace heartstead::content
