#include "engine/world/voxels/voxel_palette.hpp"

#include "engine/world/fluids/fluid_state.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>

namespace heartstead::world {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
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

[[nodiscard]] core::Status validate_token(std::string_view token, std::string_view field_name) {
    if (core::is_valid_local_id(token)) {
        return core::Status::ok();
    }
    return core::Status::failure("voxel_palette.invalid_token",
                                 std::string(field_name) + " must be a valid token");
}

[[nodiscard]] core::Status validate_token_list(const std::vector<std::string>& tokens,
                                               std::string_view field_name) {
    for (const auto& token : tokens) {
        auto status = validate_token(token, field_name);
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Result<std::string> required_field(const modding::GenericPrototype& prototype,
                                                       std::string_view key) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return core::Result<std::string>::failure("voxel_palette.missing_field",
                                                  prototype.id.value() + ": missing voxel field " +
                                                      std::string(key));
    }
    return core::Result<std::string>::success(*value);
}

[[nodiscard]] std::vector<std::string> parse_tags(const modding::GenericPrototype& prototype) {
    std::vector<std::string> result;
    const auto* tags = field(prototype, "tags");
    if (tags == nullptr || tags->empty()) {
        return result;
    }
    for (const auto tag : split(*tags, ',')) {
        result.emplace_back(tag);
    }
    return result;
}

[[nodiscard]] core::Result<float> parse_float(std::string_view value, std::string_view key) {
    float parsed = 0.0F;
    const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || ptr != value.data() + value.size() || !std::isfinite(parsed)) {
        return core::Result<float>::failure("voxel_palette.invalid_number",
                                            std::string(key) + " contains an invalid number");
    }
    return core::Result<float>::success(parsed);
}

[[nodiscard]] core::Result<std::uint8_t> parse_u8_field(const modding::GenericPrototype& prototype,
                                                        std::string_view key,
                                                        std::uint8_t fallback) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return core::Result<std::uint8_t>::success(fallback);
    }
    std::uint64_t parsed = 0;
    const auto [ptr, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (error != std::errc{} || ptr != value->data() + value->size() ||
        parsed > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<std::uint8_t>::failure("voxel_palette.invalid_number",
                                                   std::string(key) + " must be in 0..255");
    }
    return core::Result<std::uint8_t>::success(static_cast<std::uint8_t>(parsed));
}

[[nodiscard]] core::Result<bool> parse_bool_field(const modding::GenericPrototype& prototype,
                                                  std::string_view key, bool fallback) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return core::Result<bool>::success(fallback);
    }
    if (*value == "true") {
        return core::Result<bool>::success(true);
    }
    if (*value == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("voxel_palette.invalid_bool",
                                       std::string(key) + " must be true or false");
}

[[nodiscard]] core::Result<math::Bounds3f> parse_bounds(std::string_view value,
                                                        std::string_view key) {
    const auto parts = split(value, ',');
    if (parts.size() != 6) {
        return core::Result<math::Bounds3f>::failure(
            "voxel_palette.invalid_bounds", std::string(key) + " must contain six numbers");
    }
    std::array<float, 6> numbers{};
    for (std::size_t index = 0; index < numbers.size(); ++index) {
        auto number = parse_float(parts[index], key);
        if (!number) {
            return core::Result<math::Bounds3f>::failure(number.error().code,
                                                         number.error().message);
        }
        numbers[index] = number.value();
    }
    math::Bounds3f bounds{{numbers[0], numbers[1], numbers[2]},
                          {numbers[3], numbers[4], numbers[5]}};
    if (!bounds.is_valid()) {
        return core::Result<math::Bounds3f>::failure("voxel_palette.invalid_bounds",
                                                     std::string(key) + " has inverted bounds");
    }
    return core::Result<math::Bounds3f>::success(bounds);
}

[[nodiscard]] core::Result<std::vector<math::Bounds3f>>
parse_bounds_list(const modding::GenericPrototype& prototype, std::string_view key,
                  std::vector<math::Bounds3f> fallback) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return core::Result<std::vector<math::Bounds3f>>::success(std::move(fallback));
    }
    if (*value == "none") {
        return core::Result<std::vector<math::Bounds3f>>::success({});
    }
    std::vector<math::Bounds3f> result;
    for (const auto encoded : split(*value, ';')) {
        auto bounds = parse_bounds(encoded, key);
        if (!bounds) {
            return core::Result<std::vector<math::Bounds3f>>::failure(bounds.error().code,
                                                                      bounds.error().message);
        }
        result.push_back(bounds.value());
    }
    return core::Result<std::vector<math::Bounds3f>>::success(std::move(result));
}

[[nodiscard]] core::Result<BlockLogicalOccupancy>
parse_occupancy(const modding::GenericPrototype& prototype) {
    const auto* value = field(prototype, "logical_occupancy");
    if (value == nullptr || *value == "full") {
        return core::Result<BlockLogicalOccupancy>::success(BlockLogicalOccupancy::full);
    }
    if (*value == "partial") {
        return core::Result<BlockLogicalOccupancy>::success(BlockLogicalOccupancy::partial);
    }
    if (*value == "decorative") {
        return core::Result<BlockLogicalOccupancy>::success(BlockLogicalOccupancy::decorative);
    }
    if (*value == "fluid") {
        return core::Result<BlockLogicalOccupancy>::success(BlockLogicalOccupancy::fluid);
    }
    return core::Result<BlockLogicalOccupancy>::failure("voxel_palette.invalid_occupancy",
                                                        "logical_occupancy has an unknown value");
}

[[nodiscard]] core::Result<BlockOcclusionBehavior>
parse_occlusion(const modding::GenericPrototype& prototype, BlockLogicalOccupancy occupancy) {
    const auto* value = field(prototype, "occlusion");
    if (value == nullptr || value->empty()) {
        if (occupancy == BlockLogicalOccupancy::full) {
            return core::Result<BlockOcclusionBehavior>::success(BlockOcclusionBehavior::full_cube);
        }
        if (occupancy == BlockLogicalOccupancy::partial) {
            return core::Result<BlockOcclusionBehavior>::success(BlockOcclusionBehavior::model);
        }
        return core::Result<BlockOcclusionBehavior>::success(BlockOcclusionBehavior::none);
    }
    if (*value == "none") {
        return core::Result<BlockOcclusionBehavior>::success(BlockOcclusionBehavior::none);
    }
    if (*value == "model") {
        return core::Result<BlockOcclusionBehavior>::success(BlockOcclusionBehavior::model);
    }
    if (*value == "full_cube") {
        return core::Result<BlockOcclusionBehavior>::success(BlockOcclusionBehavior::full_cube);
    }
    return core::Result<BlockOcclusionBehavior>::failure("voxel_palette.invalid_occlusion",
                                                         "occlusion has an unknown value");
}

[[nodiscard]] core::Status validate_bounds_list(const std::vector<math::Bounds3f>& bounds,
                                                std::string_view label) {
    for (const auto& entry : bounds) {
        if (!entry.is_valid()) {
            return core::Status::failure("voxel_palette.invalid_bounds",
                                         std::string(label) + " contains invalid bounds");
        }
    }
    return core::Status::ok();
}

} // namespace

core::Status VoxelDefinition::validate() const {
    if (type == air_type) {
        return core::Status::failure("voxel_palette.reserved_type",
                                     "voxel type 0 is reserved for air");
    }
    if (!prototype_id.is_valid()) {
        return core::Status::failure("voxel_palette.invalid_prototype",
                                     "voxel prototype id must be valid");
    }
    if (display_name.empty()) {
        return core::Status::failure("voxel_palette.missing_display_name",
                                     "voxel display name must not be empty");
    }
    auto status = validate_token(terrain_material, "voxel terrain material");
    if (!status) {
        return status;
    }
    status = validate_token(mining_tool, "voxel mining tool");
    if (!status) {
        return status;
    }
    status = validate_token_list(tags, "voxel tag");
    if (!status) {
        return status;
    }
    if (block_model_id.has_value() && !block_model_id->is_valid()) {
        return core::Status::failure("voxel_palette.invalid_block_model",
                                     "voxel block model id is invalid");
    }
    status = validate_bounds_list(collision_bounds, "collision bounds");
    if (!status) {
        return status;
    }
    status = validate_bounds_list(selection_bounds, "selection bounds");
    if (!status) {
        return status;
    }
    status = validate_bounds_list(occlusion_bounds, "occlusion bounds");
    if (!status) {
        return status;
    }
    if (occlusion == BlockOcclusionBehavior::full_cube &&
        logical_occupancy != BlockLogicalOccupancy::full) {
        return core::Status::failure("voxel_palette.invalid_full_occlusion",
                                     "only fully occupying blocks may declare full-cube occlusion");
    }
    return core::Status::ok();
}

core::Status VoxelPalette::add(VoxelDefinition definition) {
    auto status = definition.validate();
    if (!status) {
        return status;
    }
    if (by_type_.contains(definition.type)) {
        return core::Status::failure("voxel_palette.duplicate_type",
                                     "duplicate voxel type: " + std::to_string(definition.type));
    }
    if (by_prototype_.contains(definition.prototype_id.value())) {
        return core::Status::failure("voxel_palette.duplicate_prototype",
                                     "duplicate voxel prototype: " +
                                         definition.prototype_id.value());
    }
    if (definition.block_model_id.has_value() &&
        block_models_.find(*definition.block_model_id) == nullptr) {
        return core::Status::failure("voxel_palette.missing_block_model",
                                     "voxel references a missing block model: " +
                                         definition.block_model_id->value());
    }

    const auto index = definitions_.size();
    by_type_.emplace(definition.type, index);
    by_prototype_.emplace(definition.prototype_id.value(), index);
    definitions_.push_back(std::move(definition));
    if (render_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++render_revision_;
    return core::Status::ok();
}

core::Status VoxelPalette::add_block_model(BlockModelDefinition definition) {
    auto status = block_models_.add(std::move(definition));
    if (!status) {
        return status;
    }
    if (render_revision_ == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++render_revision_;
    return core::Status::ok();
}

const VoxelDefinition* VoxelPalette::find_by_type(std::uint16_t type) const noexcept {
    const auto found = by_type_.find(type);
    return found == by_type_.end() ? nullptr : &definitions_[found->second];
}

const VoxelDefinition*
VoxelPalette::find_by_prototype(const core::PrototypeId& prototype_id) const noexcept {
    const auto found = by_prototype_.find(prototype_id.value());
    return found == by_prototype_.end() ? nullptr : &definitions_[found->second];
}

std::optional<std::uint16_t>
VoxelPalette::type_for(const core::PrototypeId& prototype_id) const noexcept {
    const auto* definition = find_by_prototype(prototype_id);
    if (definition == nullptr) {
        return std::nullopt;
    }
    return definition->type;
}

core::Result<VoxelCell> VoxelPalette::cell_for(const core::PrototypeId& prototype_id,
                                               std::uint8_t light) const {
    const auto type = type_for(prototype_id);
    if (!type) {
        return core::Result<VoxelCell>::failure("voxel_palette.missing_prototype",
                                                "voxel prototype is not in the palette: " +
                                                    prototype_id.value());
    }
    const auto* definition = find_by_type(*type);
    const std::uint16_t state_bits =
        definition != nullptr && definition->logical_occupancy == BlockLogicalOccupancy::fluid
            ? full_fluid_state_bits()
            : std::uint16_t{0};
    return core::Result<VoxelCell>::success(VoxelCell{type.value(), light, state_bits});
}

std::vector<const VoxelDefinition*> VoxelPalette::definitions() const {
    std::vector<const VoxelDefinition*> result;
    result.reserve(definitions_.size());
    for (const auto& definition : definitions_) {
        result.push_back(&definition);
    }
    return result;
}

const BlockModelDatabase& VoxelPalette::block_models() const noexcept {
    return block_models_;
}

const BlockModelDefinition& VoxelPalette::model_for(const VoxelDefinition& definition) const {
    if (definition.block_model_id.has_value()) {
        if (const auto* model = block_models_.find(*definition.block_model_id)) {
            return *model;
        }
    }
    return legacy_cube_block_model();
}

const BlockModelDefinition* VoxelPalette::model_for_type(std::uint16_t type) const noexcept {
    const auto* definition = find_by_type(type);
    if (definition == nullptr) {
        return nullptr;
    }
    return &model_for(*definition);
}

std::uint16_t VoxelPalette::mesh_invalidation_radius(VoxelCell cell) const noexcept {
    const auto* definition = find_by_type(cell.type);
    if (definition != nullptr &&
        definition->logical_occupancy == BlockLogicalOccupancy::fluid) {
        return 1;
    }
    const auto* model = model_for_type(cell.type);
    return model == nullptr ? 1 : model->mesh_invalidation_radius;
}

std::uint16_t VoxelPalette::neighbor_dependency_radius(VoxelCell cell) const noexcept {
    const auto* definition = find_by_type(cell.type);
    if (definition != nullptr &&
        definition->logical_occupancy == BlockLogicalOccupancy::fluid) {
        return 1;
    }
    const auto* model = model_for_type(cell.type);
    return model == nullptr ? 1 : model->neighbor_dependency_radius;
}

std::size_t VoxelPalette::size() const noexcept {
    return definitions_.size();
}

bool VoxelPalette::empty() const noexcept {
    return definitions_.empty();
}

std::uint64_t VoxelPalette::render_revision() const noexcept {
    return render_revision_;
}

VoxelPaletteManifest VoxelPalette::manifest() const {
    VoxelPaletteManifest result;
    result.entries.reserve(definitions_.size());
    for (const auto& definition : definitions_) {
        result.entries.push_back({definition.type, definition.prototype_id});
    }
    std::ranges::sort(result.entries, {}, &VoxelPaletteManifestEntry::type);
    return result;
}

core::Status VoxelPaletteManifest::validate() const {
    std::vector<bool> used_types(static_cast<std::size_t>(
                                     std::numeric_limits<std::uint16_t>::max()) +
                                 1U);
    std::vector<std::string> used_prototypes;
    used_prototypes.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.type == VoxelDefinition::air_type) {
            return core::Status::failure("voxel_palette_manifest.reserved_type",
                                         "persisted voxel type 0 is reserved for air");
        }
        if (!entry.prototype_id.is_valid()) {
            return core::Status::failure("voxel_palette_manifest.invalid_prototype",
                                         "persisted voxel prototype id is invalid");
        }
        if (used_types[entry.type]) {
            return core::Status::failure("voxel_palette_manifest.duplicate_type",
                                         "persisted voxel palette contains a duplicate type");
        }
        used_types[entry.type] = true;
        if (std::ranges::find(used_prototypes, entry.prototype_id.value()) !=
            used_prototypes.end()) {
            return core::Status::failure(
                "voxel_palette_manifest.duplicate_prototype",
                "persisted voxel palette contains a duplicate prototype id");
        }
        used_prototypes.push_back(entry.prototype_id.value());
    }
    return core::Status::ok();
}

const VoxelPaletteManifestEntry*
VoxelPaletteManifest::find_by_type(std::uint16_t type) const noexcept {
    const auto found = std::ranges::find(entries, type, &VoxelPaletteManifestEntry::type);
    return found == entries.end() ? nullptr : &*found;
}

const VoxelPaletteManifestEntry*
VoxelPaletteManifest::find_by_prototype(const core::PrototypeId& prototype_id) const noexcept {
    const auto found = std::ranges::find(entries, prototype_id,
                                         &VoxelPaletteManifestEntry::prototype_id);
    return found == entries.end() ? nullptr : &*found;
}

core::Result<VoxelDefinition>
voxel_definition_from_prototype(const modding::GenericPrototype& prototype, std::uint16_t type) {
    if (prototype.kind != modding::PrototypeKinds::voxel) {
        return core::Result<VoxelDefinition>::failure(
            "voxel_palette.invalid_kind", prototype.id.value() + ": prototype kind must be voxel");
    }

    auto terrain_material = required_field(prototype, "terrain_material");
    if (!terrain_material) {
        return core::Result<VoxelDefinition>::failure(terrain_material.error().code,
                                                      terrain_material.error().message);
    }
    auto mining_tool = required_field(prototype, "mining_tool");
    if (!mining_tool) {
        return core::Result<VoxelDefinition>::failure(mining_tool.error().code,
                                                      mining_tool.error().message);
    }

    VoxelDefinition definition;
    definition.type = type;
    definition.prototype_id = prototype.id;
    definition.display_name = prototype.display_name;
    definition.terrain_material = std::move(terrain_material).value();
    definition.mining_tool = std::move(mining_tool).value();
    definition.tags = parse_tags(prototype);

    if (const auto* model = field(prototype, "block_model")) {
        auto model_id = core::PrototypeId::parse(*model);
        if (!model_id) {
            return core::Result<VoxelDefinition>::failure(
                "voxel_palette.invalid_block_model",
                prototype.id.value() + ": block_model must be a prototype id");
        }
        definition.block_model_id = model_id.value();
    }
    auto occupancy = parse_occupancy(prototype);
    if (!occupancy) {
        return core::Result<VoxelDefinition>::failure(
            occupancy.error().code, prototype.id.value() + ": " + occupancy.error().message);
    }
    definition.logical_occupancy = occupancy.value();
    auto occlusion = parse_occlusion(prototype, occupancy.value());
    auto emission = parse_u8_field(prototype, "light_emission", 0);
    auto absorption = parse_u8_field(prototype, "light_absorption", 255);
    auto metadata = parse_bool_field(prototype, "metadata_required", false);
    if (!occlusion || !emission || !absorption || !metadata) {
        const auto* error = !occlusion    ? &occlusion.error()
                            : !emission   ? &emission.error()
                            : !absorption ? &absorption.error()
                                          : &metadata.error();
        return core::Result<VoxelDefinition>::failure(error->code,
                                                      prototype.id.value() + ": " + error->message);
    }
    definition.occlusion = occlusion.value();
    definition.light_emission = emission.value();
    definition.light_absorption = absorption.value();
    definition.metadata_required = metadata.value();

    const std::vector<math::Bounds3f> full_bounds{{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}};
    const auto no_collision = occupancy.value() == BlockLogicalOccupancy::decorative ||
                              occupancy.value() == BlockLogicalOccupancy::fluid;
    auto collision_bounds = parse_bounds_list(
        prototype, "collision_bounds", no_collision ? std::vector<math::Bounds3f>{} : full_bounds);
    auto selection_bounds = parse_bounds_list(prototype, "selection_bounds", full_bounds);
    auto occlusion_bounds = parse_bounds_list(prototype, "occlusion_bounds",
                                              occlusion.value() == BlockOcclusionBehavior::none
                                                  ? std::vector<math::Bounds3f>{}
                                                  : full_bounds);
    if (!collision_bounds || !selection_bounds || !occlusion_bounds) {
        const auto* error = !collision_bounds   ? &collision_bounds.error()
                            : !selection_bounds ? &selection_bounds.error()
                                                : &occlusion_bounds.error();
        return core::Result<VoxelDefinition>::failure(error->code,
                                                      prototype.id.value() + ": " + error->message);
    }
    definition.collision_bounds = std::move(collision_bounds).value();
    definition.selection_bounds = std::move(selection_bounds).value();
    definition.occlusion_bounds = std::move(occlusion_bounds).value();

    auto status = definition.validate();
    if (!status) {
        return core::Result<VoxelDefinition>::failure(
            status.error().code, prototype.id.value() + ": " + status.error().message);
    }
    return core::Result<VoxelDefinition>::success(std::move(definition));
}

core::Result<VoxelPalette>
voxel_palette_from_prototypes(const modding::PrototypeRegistry& prototypes) {
    auto voxel_prototypes = prototypes.prototypes_of_kind(modding::PrototypeKinds::voxel);
    std::ranges::sort(voxel_prototypes, {}, [](const modding::GenericPrototype* prototype) {
        return prototype->id.value();
    });

    if (voxel_prototypes.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max())) {
        return core::Result<VoxelPalette>::failure(
            "voxel_palette.too_many_voxels",
            "voxel palette cannot contain more than 65535 content voxel definitions");
    }

    auto block_models = block_model_database_from_prototypes(prototypes);
    if (!block_models) {
        return core::Result<VoxelPalette>::failure(block_models.error().code,
                                                   block_models.error().message);
    }

    VoxelPalette palette;
    for (const auto* model : block_models.value().definitions()) {
        auto status = palette.add_block_model(*model);
        if (!status) {
            return core::Result<VoxelPalette>::failure(status.error().code, status.error().message);
        }
    }
    auto next_type = VoxelPalette::first_content_type;
    for (const auto* prototype : voxel_prototypes) {
        auto definition = voxel_definition_from_prototype(*prototype, next_type);
        if (!definition) {
            return core::Result<VoxelPalette>::failure(definition.error().code,
                                                       definition.error().message);
        }
        auto status = palette.add(std::move(definition).value());
        if (!status) {
            return core::Result<VoxelPalette>::failure(status.error().code, status.error().message);
        }
        ++next_type;
    }

    return core::Result<VoxelPalette>::success(std::move(palette));
}

core::Result<VoxelPalette>
voxel_palette_from_prototypes(const modding::PrototypeRegistry& prototypes,
                              const VoxelPaletteManifest& persisted_manifest,
                              bool append_new_prototypes) {
    auto manifest_status = persisted_manifest.validate();
    if (!manifest_status) {
        return core::Result<VoxelPalette>::failure(manifest_status.error().code,
                                                   manifest_status.error().message);
    }

    auto block_models = block_model_database_from_prototypes(prototypes);
    if (!block_models) {
        return core::Result<VoxelPalette>::failure(block_models.error().code,
                                                   block_models.error().message);
    }
    VoxelPalette palette;
    for (const auto* model : block_models.value().definitions()) {
        auto status = palette.add_block_model(*model);
        if (!status) {
            return core::Result<VoxelPalette>::failure(status.error().code,
                                                       status.error().message);
        }
    }

    auto entries = persisted_manifest.entries;
    std::ranges::sort(entries, {}, &VoxelPaletteManifestEntry::type);
    std::uint16_t highest_type = 0;
    for (const auto& entry : entries) {
        VoxelDefinition definition;
        const auto* prototype = prototypes.find(entry.prototype_id);
        if (prototype != nullptr && prototype->kind == modding::PrototypeKinds::voxel) {
            auto materialized = voxel_definition_from_prototype(*prototype, entry.type);
            if (!materialized) {
                return core::Result<VoxelPalette>::failure(materialized.error().code,
                                                           materialized.error().message);
            }
            definition = std::move(materialized).value();
        } else {
            definition.type = entry.type;
            definition.prototype_id = entry.prototype_id;
            definition.display_name = "Missing Block (" + entry.prototype_id.value() + ")";
            definition.terrain_material = "missing";
            definition.mining_tool = "none";
            definition.tags = {"missing_prototype"};
            definition.missing_prototype = true;
        }
        auto status = palette.add(std::move(definition));
        if (!status) {
            return core::Result<VoxelPalette>::failure(status.error().code,
                                                       status.error().message);
        }
        highest_type = std::max(highest_type, entry.type);
    }

    if (append_new_prototypes) {
        auto voxel_prototypes = prototypes.prototypes_of_kind(modding::PrototypeKinds::voxel);
        std::ranges::sort(voxel_prototypes, {}, [](const auto* prototype) {
            return prototype->id.value();
        });
        std::uint32_t next_type = static_cast<std::uint32_t>(highest_type) + 1U;
        for (const auto* prototype : voxel_prototypes) {
            if (persisted_manifest.find_by_prototype(prototype->id) != nullptr) {
                continue;
            }
            if (next_type > std::numeric_limits<std::uint16_t>::max()) {
                return core::Result<VoxelPalette>::failure(
                    "voxel_palette.too_many_voxels",
                    "new voxel prototypes do not fit after the persisted palette");
            }
            auto definition = voxel_definition_from_prototype(
                *prototype, static_cast<std::uint16_t>(next_type));
            if (!definition) {
                return core::Result<VoxelPalette>::failure(definition.error().code,
                                                           definition.error().message);
            }
            auto status = palette.add(std::move(definition).value());
            if (!status) {
                return core::Result<VoxelPalette>::failure(status.error().code,
                                                           status.error().message);
            }
            ++next_type;
        }
    }
    return core::Result<VoxelPalette>::success(std::move(palette));
}

} // namespace heartstead::world
