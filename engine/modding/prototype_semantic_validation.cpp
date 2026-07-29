#include "engine/modding/prototype_semantic_validation.hpp"

#include "engine/core/ids.hpp"
#include "engine/renderer/materials/material_prototype_loader.hpp"
#include "engine/scenarios/scenario_prototype.hpp"
#include "engine/simulation/fire_prototype.hpp"
#include "engine/workpieces/pattern_library.hpp"
#include "engine/workpieces/workpiece_grid.hpp"
#include "engine/world/blocks/block_model.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <vector>

namespace heartstead::modding {

namespace {

void add_error(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype,
               std::string code, std::string message) {
    result.diagnostics.push_back(ModDiagnostic{DiagnosticSeverity::error, prototype.source,
                                               std::move(code),
                                               prototype.id.value() + ": " + std::move(message)});
}

[[nodiscard]] const std::string* field(const GenericPrototype& prototype, std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

[[nodiscard]] bool require_field(PrototypeSemanticValidationResult& result,
                                 const GenericPrototype& prototype, std::string_view key) {
    if (field(prototype, key) != nullptr) {
        return true;
    }
    add_error(result, prototype, "prototype_semantic.missing_field",
              "missing required representation field: " + std::string(key));
    return false;
}

[[nodiscard]] bool parse_u64(std::string_view value, std::uint64_t& output) {
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, output);
    return error == std::errc{} && ptr == end;
}

[[nodiscard]] bool parse_positive_u64(PrototypeSemanticValidationResult& result,
                                      const GenericPrototype& prototype, std::string_view key) {
    const auto* value = field(prototype, key);
    if (value == nullptr) {
        return require_field(result, prototype, key);
    }

    std::uint64_t parsed = 0;
    if (!parse_u64(*value, parsed) || parsed == 0) {
        add_error(result, prototype, "prototype_semantic.invalid_number",
                  std::string(key) + " must be a positive integer");
        return false;
    }
    return true;
}

[[nodiscard]] bool validate_optional_u64_range(PrototypeSemanticValidationResult& result,
                                               const GenericPrototype& prototype,
                                               std::string_view key, std::uint64_t min,
                                               std::uint64_t max) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return true;
    }

    std::uint64_t parsed = 0;
    if (!parse_u64(*value, parsed) || parsed < min || parsed > max) {
        add_error(result, prototype, "prototype_semantic.invalid_number",
                  std::string(key) + " must be an integer in the valid range");
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char delimiter) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const auto end = value.find(delimiter, start);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(start));
            break;
        }
        result.push_back(value.substr(start, end - start));
        start = end + 1;
    }
    return result;
}

[[nodiscard]] bool validate_token_list(PrototypeSemanticValidationResult& result,
                                       const GenericPrototype& prototype, std::string_view key,
                                       bool required) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return required ? require_field(result, prototype, key) : true;
    }

    std::set<std::string_view> seen;
    for (const auto token : split(*value, ',')) {
        if (!core::is_valid_local_id(token)) {
            add_error(result, prototype, "prototype_semantic.invalid_token",
                      std::string(key) + " contains invalid token: " + std::string(token));
            return false;
        }
        if (!seen.insert(token).second) {
            add_error(result, prototype, "prototype_semantic.duplicate_token",
                      std::string(key) + " contains duplicate token: " + std::string(token));
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validate_local_id_field(PrototypeSemanticValidationResult& result,
                                           const GenericPrototype& prototype, std::string_view key,
                                           bool required) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return required ? require_field(result, prototype, key) : true;
    }
    if (core::is_valid_local_id(*value)) {
        return true;
    }
    add_error(result, prototype, "prototype_semantic.invalid_token",
              std::string(key) + " must be a valid local id");
    return false;
}

[[nodiscard]] bool validate_bool_field(PrototypeSemanticValidationResult& result,
                                       const GenericPrototype& prototype, std::string_view key,
                                       bool required) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return required ? require_field(result, prototype, key) : true;
    }
    if (*value == "true" || *value == "false") {
        return true;
    }
    add_error(result, prototype, "prototype_semantic.invalid_bool",
              std::string(key) + " must be true or false");
    return false;
}

[[nodiscard]] bool validate_enum_field(PrototypeSemanticValidationResult& result,
                                       const GenericPrototype& prototype, std::string_view key,
                                       std::initializer_list<std::string_view> allowed) {
    const auto* value = field(prototype, key);
    if (value == nullptr) {
        return require_field(result, prototype, key);
    }
    if (std::ranges::find(allowed, std::string_view(*value)) != allowed.end()) {
        return true;
    }
    add_error(result, prototype, "prototype_semantic.invalid_enum",
              std::string(key) + " has unsupported value: " + *value);
    return false;
}

[[nodiscard]] bool validate_prototype_id_list(PrototypeSemanticValidationResult& result,
                                              const GenericPrototype& prototype,
                                              const PrototypeRegistry& registry,
                                              std::string_view key, std::string_view expected_kind,
                                              bool required) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return required ? require_field(result, prototype, key) : true;
    }

    bool valid = true;
    for (const auto entry : split(*value, ',')) {
        const auto prototype_id = core::PrototypeId::parse(entry);
        if (!prototype_id) {
            add_error(result, prototype, "prototype_semantic.invalid_reference",
                      std::string(key) + " contains invalid prototype id: " + std::string(entry));
            valid = false;
            continue;
        }

        auto status = registry.require_kind(prototype_id.value(), expected_kind);
        if (!status) {
            add_error(result, prototype, status.error().code, status.error().message);
            valid = false;
        }
    }
    return valid;
}

[[nodiscard]] bool validate_transport_modes(PrototypeSemanticValidationResult& result,
                                            const GenericPrototype& prototype) {
    const auto* value = field(prototype, "transport_modes");
    if (value == nullptr || value->empty()) {
        return require_field(result, prototype, "transport_modes");
    }

    const std::set<std::string_view> allowed{"hand", "cart", "wagon", "boat", "animal", "crane"};
    for (const auto mode : split(*value, ',')) {
        if (!allowed.contains(mode)) {
            add_error(result, prototype, "prototype_semantic.invalid_transport_mode",
                      "unsupported cargo transport mode: " + std::string(mode));
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validate_grid(PrototypeSemanticValidationResult& result,
                                 const GenericPrototype& prototype) {
    const auto* value = field(prototype, "grid");
    if (value == nullptr) {
        return require_field(result, prototype, "grid");
    }

    const auto first_x = value->find('x');
    const auto second_x =
        first_x == std::string::npos ? std::string::npos : value->find('x', first_x + 1);
    if (first_x == std::string::npos || second_x == std::string::npos) {
        add_error(result, prototype, "prototype_semantic.invalid_grid",
                  "workpiece grid must be formatted as WxHxD");
        return false;
    }

    std::uint64_t width = 0;
    std::uint64_t height = 0;
    std::uint64_t depth = 0;
    if (!parse_u64(std::string_view(*value).substr(0, first_x), width) ||
        !parse_u64(std::string_view(*value).substr(first_x + 1, second_x - first_x - 1), height) ||
        !parse_u64(std::string_view(*value).substr(second_x + 1), depth) || width == 0 ||
        height == 0 || depth == 0 || width > 64 || height > 64 || depth > 64) {
        add_error(result, prototype, "prototype_semantic.invalid_grid",
                  "workpiece grid dimensions must be between 1 and 64");
        return false;
    }

    const auto grid = workpieces::WorkpieceGrid::create({static_cast<std::uint16_t>(width),
                                                         static_cast<std::uint16_t>(height),
                                                         static_cast<std::uint16_t>(depth)});
    if (!grid) {
        add_error(result, prototype, grid.error().code, grid.error().message);
        return false;
    }
    return true;
}

void validate_item(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype) {
    (void)parse_positive_u64(result, prototype, "stack_limit");
    (void)validate_optional_u64_range(result, prototype, "mass_grams", 0,
                                      std::numeric_limits<std::uint64_t>::max());
    (void)validate_token_list(result, prototype, "tags", false);
}

void validate_cargo(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype) {
    (void)parse_positive_u64(result, prototype, "mass_grams");
    (void)parse_positive_u64(result, prototype, "volume_milliliters");
    (void)validate_transport_modes(result, prototype);
    (void)validate_token_list(result, prototype, "hazard_tags", false);
}

void validate_entity(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype) {
    (void)validate_enum_field(result, prototype, "entity_kind",
                              {"player", "creature", "animal", "cart", "boat", "dropped_item",
                               "projectile", "temporary_physics"});
    (void)validate_bool_field(result, prototype, "persistent", false);
    (void)validate_optional_u64_range(result, prototype, "carry_capacity_grams", 1,
                                      std::numeric_limits<std::uint64_t>::max());
}

void validate_voxel(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype,
                    const PrototypeRegistry& registry) {
    (void)validate_token_list(result, prototype, "mining_tool", true);
    (void)validate_token_list(result, prototype, "terrain_material", true);
    (void)validate_token_list(result, prototype, "tags", false);
    auto definition = world::voxel_definition_from_prototype(prototype, 1);
    if (!definition) {
        add_error(result, prototype, definition.error().code, definition.error().message);
        return;
    }
    if (definition.value().block_model_id.has_value()) {
        auto status =
            registry.require_kind(*definition.value().block_model_id, PrototypeKinds::block_model);
        if (!status) {
            add_error(result, prototype, status.error().code, status.error().message);
        }
    }
}

void validate_build_piece(PrototypeSemanticValidationResult& result,
                          const GenericPrototype& prototype) {
    (void)validate_token_list(result, prototype, "material_tags", false);
    (void)validate_token_list(result, prototype, "network_ports", false);
    (void)validate_token_list(result, prototype, "room_contribution_tags", false);
}

void validate_assembly_parts(PrototypeSemanticValidationResult& result,
                             const GenericPrototype& prototype, const PrototypeRegistry& registry,
                             std::string_view key, bool required) {
    const auto* parts = field(prototype, key);
    if (parts == nullptr || parts->empty()) {
        if (!required) {
            return;
        }
        add_error(result, prototype, "prototype_semantic.missing_field",
                  "assembly must declare " + std::string(key));
        return;
    }

    for (const auto entry : split(*parts, ',')) {
        const auto separator = entry.find(':');
        if (separator == std::string_view::npos) {
            add_error(result, prototype, "prototype_semantic.invalid_assembly_part",
                      std::string(key) + " part must be name:prototype_id");
            continue;
        }
        const auto name = entry.substr(0, separator);
        const auto prototype_id_text = entry.substr(separator + 1);
        if (!core::is_valid_local_id(name)) {
            add_error(result, prototype, "prototype_semantic.invalid_assembly_part",
                      std::string(key) + " part has invalid name: " + std::string(name));
            continue;
        }
        const auto prototype_id = core::PrototypeId::parse(prototype_id_text);
        if (!prototype_id) {
            add_error(result, prototype, "prototype_semantic.invalid_assembly_part",
                      std::string(key) + " part prototype id is invalid");
            continue;
        }
        auto status = registry.require_kind(prototype_id.value(), PrototypeKinds::build_piece);
        if (!status) {
            add_error(result, prototype, status.error().code, status.error().message);
        }
    }
}

void validate_assembly(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype,
                       const PrototypeRegistry& registry) {
    validate_assembly_parts(result, prototype, registry, "required_parts", true);
    validate_assembly_parts(result, prototype, registry, "optional_parts", false);
    (void)validate_token_list(result, prototype, "required_ports", false);
}

void validate_workpiece(PrototypeSemanticValidationResult& result,
                        const GenericPrototype& prototype, const PrototypeRegistry& registry) {
    (void)validate_grid(result, prototype);
    const auto* material = field(prototype, "material");
    if (material == nullptr) {
        (void)require_field(result, prototype, "material");
        return;
    }
    const auto material_id = core::PrototypeId::parse(*material);
    if (!material_id) {
        add_error(result, prototype, "prototype_semantic.invalid_material",
                  "workpiece material must be a prototype id");
        return;
    }
    auto status = registry.require_kind(material_id.value(), PrototypeKinds::material);
    if (!status) {
        add_error(result, prototype, status.error().code, status.error().message);
    }
}

void validate_pattern(PrototypeSemanticValidationResult& result,
                      const GenericPrototype& prototype) {
    auto pattern = workpieces::pattern_definition_from_prototype(prototype);
    if (!pattern)
        add_error(result, prototype, pattern.error().code, pattern.error().message);
}

void validate_process(PrototypeSemanticValidationResult& result,
                      const GenericPrototype& prototype) {
    if (field(prototype, "default_required_work_ticks") != nullptr) {
        (void)parse_positive_u64(result, prototype, "default_required_work_ticks");
    } else {
        (void)parse_positive_u64(result, prototype, "default_required_work_ms");
    }
    (void)validate_bool_field(result, prototype, "requires_room", false);
    (void)validate_bool_field(result, prototype, "requires_power", false);
    (void)validate_optional_u64_range(
        result, prototype, "required_power_capacity", 1,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
    (void)validate_optional_u64_range(result, prototype, "base_quality_rate_per_mille", 0, 10000);
    (void)validate_token_list(result, prototype, "tags", false);
}

void validate_fire(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype) {
    auto definition = simulation::fire_definition_from_prototype(prototype);
    if (!definition) {
        add_error(result, prototype, definition.error().code, definition.error().message);
    }
}

void validate_room_descriptor(PrototypeSemanticValidationResult& result,
                              const GenericPrototype& prototype) {
    (void)validate_local_id_field(result, prototype, "code", true);
    (void)validate_enum_field(result, prototype, "severity", {"positive", "neutral", "warning"});
    (void)validate_token_list(result, prototype, "tags", false);
}

void validate_material(PrototypeSemanticValidationResult& result,
                       const GenericPrototype& prototype) {
    auto material = renderer::materials::material_definition_from_prototype(prototype);
    if (!material) {
        add_error(result, prototype, material.error().code, material.error().message);
    }
}

void validate_block_model(PrototypeSemanticValidationResult& result,
                          const GenericPrototype& prototype) {
    auto definition = world::block_model_definition_from_prototype(prototype);
    if (!definition) {
        add_error(result, prototype, definition.error().code, definition.error().message);
    }
}

void validate_scenario(PrototypeSemanticValidationResult& result, const GenericPrototype& prototype,
                       const PrototypeRegistry& registry) {
    (void)validate_token_list(result, prototype, "start_region", true);
    (void)validate_enum_field(result, prototype, "spawn_mode", {"homestead", "outpost", "debug"});
    (void)validate_token_list(result, prototype, "tags", false);
    (void)validate_prototype_id_list(result, prototype, registry, "starting_items",
                                     PrototypeKinds::item, false);
    (void)validate_prototype_id_list(result, prototype, registry, "starting_cargo",
                                     PrototypeKinds::cargo, false);
    if (field(prototype, "scene_entities") == nullptr) {
        return;
    }
    auto scenario = scenarios::scenario_definition_from_prototype(prototype);
    if (!scenario) {
        add_error(result, prototype, scenario.error().code, scenario.error().message);
        return;
    }
    for (const auto& placement : scenario.value().scene_entities) {
        const auto* entity = registry.find(placement.prototype_id);
        if (entity == nullptr || entity->kind != PrototypeKinds::entity) {
            add_error(result, prototype, "prototype_semantic.invalid_reference",
                      "scene_entities references a missing or non-entity prototype: " +
                          placement.prototype_id.value());
        }
    }
}

} // namespace

bool PrototypeSemanticValidationResult::has_errors() const noexcept {
    return std::ranges::any_of(diagnostics, [](const ModDiagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::error;
    });
}

PrototypeSemanticValidationResult
PrototypeSemanticValidator::validate(const std::vector<GenericPrototype>& prototypes,
                                     const PrototypeRegistry& registry) {
    PrototypeSemanticValidationResult result;
    for (const auto& prototype : prototypes) {
        if (prototype.kind == PrototypeKinds::item) {
            validate_item(result, prototype);
        } else if (prototype.kind == PrototypeKinds::cargo) {
            validate_cargo(result, prototype);
        } else if (prototype.kind == PrototypeKinds::entity) {
            validate_entity(result, prototype);
        } else if (prototype.kind == PrototypeKinds::voxel) {
            validate_voxel(result, prototype, registry);
        } else if (prototype.kind == PrototypeKinds::block_model) {
            validate_block_model(result, prototype);
        } else if (prototype.kind == PrototypeKinds::build_piece) {
            validate_build_piece(result, prototype);
        } else if (prototype.kind == PrototypeKinds::assembly) {
            validate_assembly(result, prototype, registry);
        } else if (prototype.kind == PrototypeKinds::workpiece) {
            validate_workpiece(result, prototype, registry);
        } else if (prototype.kind == PrototypeKinds::pattern) {
            validate_pattern(result, prototype);
        } else if (prototype.kind == PrototypeKinds::process) {
            validate_process(result, prototype);
        } else if (prototype.kind == PrototypeKinds::fire) {
            validate_fire(result, prototype);
        } else if (prototype.kind == PrototypeKinds::room_descriptor) {
            validate_room_descriptor(result, prototype);
        } else if (prototype.kind == PrototypeKinds::material) {
            validate_material(result, prototype);
        } else if (prototype.kind == PrototypeKinds::scenario) {
            validate_scenario(result, prototype, registry);
        }
    }
    return result;
}

} // namespace heartstead::modding
