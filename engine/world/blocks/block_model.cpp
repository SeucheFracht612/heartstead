#include "engine/world/blocks/block_model.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
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

[[nodiscard]] core::Result<float> parse_float(std::string_view value, std::string_view label) {
    float parsed = 0.0F;
    const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || ptr != value.data() + value.size() || !std::isfinite(parsed)) {
        return core::Result<float>::failure("block_model.invalid_number",
                                            "invalid block model number: " + std::string(label));
    }
    return core::Result<float>::success(parsed);
}

[[nodiscard]] core::Result<std::uint16_t> parse_radius(const modding::GenericPrototype& prototype,
                                                       std::string_view key,
                                                       std::uint16_t fallback) {
    const auto* value = field(prototype, key);
    if (value == nullptr || value->empty()) {
        return core::Result<std::uint16_t>::success(fallback);
    }
    std::uint64_t parsed = 0;
    const auto [ptr, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (error != std::errc{} || ptr != value->data() + value->size() ||
        parsed > BlockModelDefinition::max_dependency_radius) {
        return core::Result<std::uint16_t>::failure(
            "block_model.invalid_radius",
            std::string(key) + " must be in 0.." +
                std::to_string(BlockModelDefinition::max_dependency_radius));
    }
    return core::Result<std::uint16_t>::success(static_cast<std::uint16_t>(parsed));
}

[[nodiscard]] core::Result<math::Bounds3f> parse_bounds(std::string_view value,
                                                        std::string_view label) {
    const auto components = split(value, ',');
    if (components.size() != 6) {
        return core::Result<math::Bounds3f>::failure(
            "block_model.invalid_bounds", std::string(label) + " must contain six numbers");
    }
    std::array<float, 6> parsed{};
    for (std::size_t index = 0; index < parsed.size(); ++index) {
        auto component = parse_float(components[index], label);
        if (!component) {
            return core::Result<math::Bounds3f>::failure(component.error().code,
                                                         component.error().message);
        }
        parsed[index] = component.value();
    }
    math::Bounds3f bounds{{parsed[0], parsed[1], parsed[2]}, {parsed[3], parsed[4], parsed[5]}};
    if (!bounds.is_valid()) {
        return core::Result<math::Bounds3f>::failure("block_model.invalid_bounds",
                                                     std::string(label) + " has inverted bounds");
    }
    constexpr float limit = static_cast<float>(BlockModelDefinition::max_dependency_radius) + 1.0F;
    if (bounds.min.x < -limit || bounds.min.y < -limit || bounds.min.z < -limit ||
        bounds.max.x > limit || bounds.max.y > limit || bounds.max.z > limit) {
        return core::Result<math::Bounds3f>::failure(
            "block_model.bounds_too_large", std::string(label) + " exceeds the model halo limit");
    }
    return core::Result<math::Bounds3f>::success(bounds);
}

[[nodiscard]] core::Result<std::vector<BlockModelBox>>
parse_boxes(const modding::GenericPrototype& prototype, BlockModelKind kind) {
    const auto* boxes = field(prototype, "boxes");
    if (boxes == nullptr || boxes->empty()) {
        const auto* triangles = field(prototype, "triangles");
        if (kind == BlockModelKind::mesh || kind == BlockModelKind::cross_plane ||
            (triangles != nullptr && !triangles->empty())) {
            return core::Result<std::vector<BlockModelBox>>::success({});
        }
        return core::Result<std::vector<BlockModelBox>>::success(
            {BlockModelBox{{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}}});
    }

    std::vector<BlockModelBox> result;
    for (const auto encoded_box : split(*boxes, ';')) {
        if (encoded_box.empty()) {
            return core::Result<std::vector<BlockModelBox>>::failure(
                "block_model.invalid_boxes", "block model boxes contain an empty entry");
        }
        auto bounds = parse_bounds(encoded_box, "boxes");
        if (!bounds) {
            return core::Result<std::vector<BlockModelBox>>::failure(bounds.error().code,
                                                                     bounds.error().message);
        }
        result.push_back(BlockModelBox{bounds.value()});
    }
    return core::Result<std::vector<BlockModelBox>>::success(std::move(result));
}

[[nodiscard]] core::Result<std::vector<BlockModelTriangle>>
parse_triangles(const modding::GenericPrototype& prototype) {
    const auto* triangles = field(prototype, "triangles");
    if (triangles == nullptr || triangles->empty()) {
        return core::Result<std::vector<BlockModelTriangle>>::success({});
    }
    std::vector<BlockModelTriangle> result;
    for (const auto encoded_triangle : split(*triangles, ';')) {
        const auto components = split(encoded_triangle, ',');
        if (components.size() != 9U) {
            return core::Result<std::vector<BlockModelTriangle>>::failure(
                "block_model.invalid_triangles",
                "each authored block triangle must contain nine numbers");
        }
        BlockModelTriangle triangle;
        for (std::size_t position = 0; position < triangle.positions.size(); ++position) {
            auto x = parse_float(components[position * 3U], "triangles");
            auto y = parse_float(components[position * 3U + 1U], "triangles");
            auto z = parse_float(components[position * 3U + 2U], "triangles");
            if (!x || !y || !z) {
                const auto* error = !x ? &x.error() : !y ? &y.error() : &z.error();
                return core::Result<std::vector<BlockModelTriangle>>::failure(error->code,
                                                                              error->message);
            }
            triangle.positions[position] = {x.value(), y.value(), z.value()};
        }
        auto status = triangle.validate();
        if (!status) {
            return core::Result<std::vector<BlockModelTriangle>>::failure(status.error().code,
                                                                          status.error().message);
        }
        result.push_back(triangle);
    }
    return core::Result<std::vector<BlockModelTriangle>>::success(std::move(result));
}

[[nodiscard]] math::Bounds3f
bounds_for_geometry(const std::vector<BlockModelBox>& boxes,
                    const std::vector<BlockModelTriangle>& triangles) noexcept {
    if (boxes.empty() && triangles.empty()) {
        return {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    }
    math::Bounds3f bounds{};
    bool initialized = false;
    for (const auto& box : boxes) {
        bounds = initialized ? bounds.merged_with(box.bounds) : box.bounds;
        initialized = true;
    }
    for (const auto& triangle : triangles) {
        for (const auto position : triangle.positions) {
            if (!initialized) {
                bounds = {position, position};
                initialized = true;
            } else {
                bounds.min = math::component_min(bounds.min, position);
                bounds.max = math::component_max(bounds.max, position);
            }
        }
    }
    return bounds;
}

[[nodiscard]] bool same_float(float left, float right) noexcept {
    return std::abs(left - right) <= 0.0001F;
}

[[nodiscard]] std::uint16_t geometric_invalidation_radius(const math::Bounds3f& bounds) noexcept {
    const auto outside = std::max({0.0F, -bounds.min.x, -bounds.min.y, -bounds.min.z,
                                   bounds.max.x - 1.0F, bounds.max.y - 1.0F, bounds.max.z - 1.0F});
    return static_cast<std::uint16_t>(std::ceil(outside));
}

[[nodiscard]] bool bounds_within_engine_limit(const math::Bounds3f& bounds) noexcept {
    constexpr float limit = static_cast<float>(BlockModelDefinition::max_dependency_radius) + 1.0F;
    return bounds.min.x >= -limit && bounds.min.y >= -limit && bounds.min.z >= -limit &&
           bounds.max.x <= limit && bounds.max.y <= limit && bounds.max.z <= limit;
}

} // namespace

core::Status BlockModelBox::validate() const {
    if (!bounds.is_valid()) {
        return core::Status::failure("block_model.invalid_box", "block model box is invalid");
    }
    if (same_float(bounds.min.x, bounds.max.x) || same_float(bounds.min.y, bounds.max.y) ||
        same_float(bounds.min.z, bounds.max.z)) {
        return core::Status::failure("block_model.empty_box",
                                     "block model box must have positive volume");
    }
    return core::Status::ok();
}

core::Status BlockModelTriangle::validate() const {
    if (!positions[0].is_finite() || !positions[1].is_finite() || !positions[2].is_finite()) {
        return core::Status::failure("block_model.invalid_triangle",
                                     "authored block triangle contains a non-finite position");
    }
    const auto normal = math::cross(positions[1] - positions[0], positions[2] - positions[0]);
    if (math::length_squared(normal) <= 0.00000001F) {
        return core::Status::failure("block_model.degenerate_triangle",
                                     "authored block triangle must have nonzero area");
    }
    return core::Status::ok();
}

core::Status BlockModelDefinition::validate() const {
    if (!prototype_id.is_valid()) {
        return core::Status::failure("block_model.invalid_prototype",
                                     "block model requires a stable prototype id");
    }
    if (!render_bounds.is_valid()) {
        return core::Status::failure("block_model.invalid_render_bounds",
                                     "block model render bounds are invalid");
    }
    if (!bounds_within_engine_limit(render_bounds)) {
        return core::Status::failure("block_model.bounds_too_large",
                                     "block model render bounds exceed the engine halo limit");
    }
    if (neighbor_dependency_radius > max_dependency_radius ||
        mesh_invalidation_radius > max_dependency_radius) {
        return core::Status::failure("block_model.invalid_radius",
                                     "block model dependency radius exceeds engine limit");
    }
    if (mesh_invalidation_radius < neighbor_dependency_radius) {
        return core::Status::failure(
            "block_model.invalidation_too_small",
            "mesh invalidation radius must cover the neighbor dependency radius");
    }
    if (mesh_invalidation_radius < geometric_invalidation_radius(render_bounds)) {
        return core::Status::failure(
            "block_model.invalidation_too_small",
            "mesh invalidation radius must cover render geometry outside the owning cell");
    }
    if (kind == BlockModelKind::mesh && mesh_asset.empty()) {
        return core::Status::failure("block_model.missing_mesh_asset",
                                     "mesh block models require a mesh_asset");
    }
    if (uses_chunk_geometry() && kind != BlockModelKind::cross_plane && boxes.empty() &&
        triangles.empty()) {
        return core::Status::failure("block_model.missing_geometry",
                                     "chunk block model has no geometry");
    }
    if (!triangles.empty() &&
        (kind == BlockModelKind::mesh || kind == BlockModelKind::cross_plane)) {
        return core::Status::failure(
            "block_model.unsupported_authored_triangles",
            "authored triangles are only valid for chunk-geometry block models");
    }
    for (const auto& box : boxes) {
        auto status = box.validate();
        if (!status) {
            return status;
        }
        if (!bounds_within_engine_limit(box.bounds) ||
            mesh_invalidation_radius < geometric_invalidation_radius(box.bounds)) {
            return core::Status::failure("block_model.invalidation_too_small",
                                         "mesh invalidation radius must cover every model box");
        }
    }
    for (const auto& triangle : triangles) {
        auto status = triangle.validate();
        if (!status) {
            return status;
        }
        for (const auto position : triangle.positions) {
            if (!bounds_within_engine_limit({position, position}) ||
                !render_bounds.contains(position)) {
                return core::Status::failure(
                    "block_model.invalid_triangle_bounds",
                    "authored block triangle lies outside the model render bounds or halo limit");
            }
        }
    }
    return core::Status::ok();
}

bool BlockModelDefinition::uses_chunk_geometry() const noexcept {
    return kind != BlockModelKind::mesh;
}

bool BlockModelDefinition::is_unit_cube() const noexcept {
    if (kind != BlockModelKind::cube || boxes.size() != 1 || !triangles.empty()) {
        return false;
    }
    const auto& bounds = boxes.front().bounds;
    return same_float(bounds.min.x, 0.0F) && same_float(bounds.min.y, 0.0F) &&
           same_float(bounds.min.z, 0.0F) && same_float(bounds.max.x, 1.0F) &&
           same_float(bounds.max.y, 1.0F) && same_float(bounds.max.z, 1.0F);
}

core::Status BlockModelDatabase::add(BlockModelDefinition definition) {
    auto status = definition.validate();
    if (!status) {
        return status;
    }
    if (by_prototype_.contains(definition.prototype_id.value())) {
        return core::Status::failure("block_model.duplicate_prototype",
                                     "duplicate block model: " + definition.prototype_id.value());
    }
    const auto index = definitions_.size();
    by_prototype_.emplace(definition.prototype_id.value(), index);
    definitions_.push_back(std::move(definition));
    return core::Status::ok();
}

const BlockModelDefinition*
BlockModelDatabase::find(const core::PrototypeId& prototype_id) const noexcept {
    const auto found = by_prototype_.find(prototype_id.value());
    return found == by_prototype_.end() ? nullptr : &definitions_[found->second];
}

std::vector<const BlockModelDefinition*> BlockModelDatabase::definitions() const {
    std::vector<const BlockModelDefinition*> result;
    result.reserve(definitions_.size());
    for (const auto& definition : definitions_) {
        result.push_back(&definition);
    }
    return result;
}

std::size_t BlockModelDatabase::size() const noexcept {
    return definitions_.size();
}

bool BlockModelDatabase::empty() const noexcept {
    return definitions_.empty();
}

const BlockModelDefinition& legacy_cube_block_model() noexcept {
    static const BlockModelDefinition model = [] {
        BlockModelDefinition definition;
        definition.prototype_id =
            core::PrototypeId::parse("engine:block_models/legacy_cube").value();
        definition.kind = BlockModelKind::cube;
        definition.boxes.push_back({{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}});
        definition.render_bounds = definition.boxes.front().bounds;
        return definition;
    }();
    return model;
}

std::string_view block_model_kind_name(BlockModelKind kind) noexcept {
    switch (kind) {
    case BlockModelKind::cube:
        return "cube";
    case BlockModelKind::parametric:
        return "parametric";
    case BlockModelKind::cross_plane:
        return "cross_plane";
    case BlockModelKind::custom_voxel:
        return "custom_voxel";
    case BlockModelKind::mesh:
        return "mesh";
    case BlockModelKind::connected_texture:
        return "connected_texture";
    case BlockModelKind::ore_protrusion:
        return "ore_protrusion";
    case BlockModelKind::random_variant:
        return "random_variant";
    case BlockModelKind::state_dependent:
        return "state_dependent";
    }
    return "unknown";
}

std::optional<BlockModelKind> parse_block_model_kind(std::string_view value) noexcept {
    constexpr std::array pairs{
        std::pair{std::string_view("cube"), BlockModelKind::cube},
        std::pair{std::string_view("parametric"), BlockModelKind::parametric},
        std::pair{std::string_view("cross_plane"), BlockModelKind::cross_plane},
        std::pair{std::string_view("custom_voxel"), BlockModelKind::custom_voxel},
        std::pair{std::string_view("mesh"), BlockModelKind::mesh},
        std::pair{std::string_view("connected_texture"), BlockModelKind::connected_texture},
        std::pair{std::string_view("ore_protrusion"), BlockModelKind::ore_protrusion},
        std::pair{std::string_view("random_variant"), BlockModelKind::random_variant},
        std::pair{std::string_view("state_dependent"), BlockModelKind::state_dependent},
    };
    for (const auto& [name, kind] : pairs) {
        if (name == value) {
            return kind;
        }
    }
    return std::nullopt;
}

core::Result<BlockModelDefinition>
block_model_definition_from_prototype(const modding::GenericPrototype& prototype) {
    if (prototype.kind != modding::PrototypeKinds::block_model) {
        return core::Result<BlockModelDefinition>::failure(
            "block_model.invalid_kind", prototype.id.value() + ": kind must be block_model");
    }
    const auto* model_type = field(prototype, "model_type");
    const auto kind = model_type == nullptr ? std::optional{BlockModelKind::cube}
                                            : parse_block_model_kind(*model_type);
    if (!kind) {
        return core::Result<BlockModelDefinition>::failure(
            "block_model.invalid_model_type", prototype.id.value() + ": unknown model_type");
    }
    auto boxes = parse_boxes(prototype, *kind);
    auto triangles = parse_triangles(prototype);
    auto dependency = parse_radius(prototype, "neighbor_dependency_radius", 1);
    auto invalidation =
        parse_radius(prototype, "mesh_invalidation_radius", dependency ? dependency.value() : 1);
    if (!boxes || !triangles || !dependency || !invalidation) {
        const auto* error = !boxes        ? &boxes.error()
                            : !triangles  ? &triangles.error()
                            : !dependency ? &dependency.error()
                                          : &invalidation.error();
        return core::Result<BlockModelDefinition>::failure(error->code, prototype.id.value() +
                                                                            ": " + error->message);
    }

    BlockModelDefinition definition;
    definition.prototype_id = prototype.id;
    definition.kind = *kind;
    definition.boxes = std::move(boxes).value();
    definition.triangles = std::move(triangles).value();
    definition.render_bounds = bounds_for_geometry(definition.boxes, definition.triangles);
    if (const auto* encoded_bounds = field(prototype, "render_bounds")) {
        auto parsed = parse_bounds(*encoded_bounds, "render_bounds");
        if (!parsed) {
            return core::Result<BlockModelDefinition>::failure(
                parsed.error().code, prototype.id.value() + ": " + parsed.error().message);
        }
        definition.render_bounds = parsed.value();
    }
    if (const auto* material = field(prototype, "material")) {
        definition.material_asset = *material;
    }
    if (const auto* mesh = field(prototype, "mesh_asset")) {
        definition.mesh_asset = *mesh;
    }
    definition.neighbor_dependency_radius = dependency.value();
    definition.mesh_invalidation_radius = invalidation.value();

    auto status = definition.validate();
    if (!status) {
        return core::Result<BlockModelDefinition>::failure(
            status.error().code, prototype.id.value() + ": " + status.error().message);
    }
    return core::Result<BlockModelDefinition>::success(std::move(definition));
}

core::Result<BlockModelDatabase>
block_model_database_from_prototypes(const modding::PrototypeRegistry& prototypes) {
    auto model_prototypes = prototypes.prototypes_of_kind(modding::PrototypeKinds::block_model);
    std::ranges::sort(model_prototypes, {}, [](const modding::GenericPrototype* prototype) {
        return prototype->id.value();
    });
    BlockModelDatabase database;
    for (const auto* prototype : model_prototypes) {
        auto definition = block_model_definition_from_prototype(*prototype);
        if (!definition) {
            return core::Result<BlockModelDatabase>::failure(definition.error().code,
                                                             definition.error().message);
        }
        auto status = database.add(std::move(definition).value());
        if (!status) {
            return core::Result<BlockModelDatabase>::failure(status.error().code,
                                                             status.error().message);
        }
    }
    return core::Result<BlockModelDatabase>::success(std::move(database));
}

} // namespace heartstead::world
