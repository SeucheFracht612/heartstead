#include "engine/entities/physical_resource.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace heartstead::entities {

namespace {

[[nodiscard]] bool state_requires_body(PhysicalResourceState state) noexcept {
    return state == PhysicalResourceState::dynamic ||
           state == PhysicalResourceState::settled_sleeping ||
           state == PhysicalResourceState::frozen_static;
}

[[nodiscard]] bool state_allows_body(PhysicalResourceState state) noexcept {
    return state == PhysicalResourceState::dynamic ||
           state == PhysicalResourceState::settled_sleeping ||
           state == PhysicalResourceState::frozen_static;
}

[[nodiscard]] physics::CompoundShapeChild
to_compound_child(const PhysicalResourceSegment& segment) noexcept {
    return physics::CompoundShapeChild{
        segment.shape,  segment.local_position, segment.half_extents,
        segment.radius, segment.half_height,
    };
}

[[nodiscard]] physics::PhysicsShapeDesc to_child_shape(const PhysicalResourceSegment& segment) {
    physics::PhysicsShapeDesc shape;
    shape.kind = segment.shape;
    shape.half_extents = segment.half_extents;
    shape.radius = segment.radius;
    shape.half_height = segment.half_height;
    return shape;
}

[[nodiscard]] bool can_convert_to_cargo(PhysicalResourceState state) noexcept {
    return state == PhysicalResourceState::settled_sleeping ||
           state == PhysicalResourceState::frozen_static;
}

} // namespace

core::Status PhysicalResourceRecord::validate() const {
    if (!resource_id.is_valid()) {
        return core::Status::failure("physical_resource.invalid_id",
                                     "physical resource needs a stable save id");
    }
    if (!prototype_id.is_valid()) {
        return core::Status::failure("physical_resource.invalid_prototype",
                                     "physical resource prototype id must be valid");
    }
    if (!cargo_prototype_id.is_valid()) {
        return core::Status::failure("physical_resource.invalid_cargo_prototype",
                                     "physical resource cargo prototype id must be valid");
    }
    if (!position.is_valid()) {
        return core::Status::failure("physical_resource.invalid_position",
                                     "physical resource requires an anchored world position");
    }
    if (!rotation_degrees.is_finite() || !linear_velocity.is_finite() ||
        !angular_velocity.is_finite()) {
        return core::Status::failure("physical_resource.invalid_transform",
                                     "physical resource rotation and velocities must be finite");
    }
    if (mass_grams == 0) {
        return core::Status::failure("physical_resource.invalid_mass",
                                     "physical resource mass must be non-zero");
    }
    if (volume_milliliters == 0) {
        return core::Status::failure("physical_resource.invalid_volume",
                                     "physical resource volume must be non-zero");
    }
    if (stability_per_mille < 0 || stability_per_mille > 1000) {
        return core::Status::failure("physical_resource.invalid_stability",
                                     "physical resource stability must be between 0 and 1000");
    }
    if (allowed_transport_modes.empty()) {
        return core::Status::failure("physical_resource.no_transport_modes",
                                     "physical resource must declare cargo transport modes");
    }
    if (segments.empty()) {
        return core::Status::failure("physical_resource.no_segments",
                                     "physical resource needs at least one compound segment");
    }
    if (state_requires_body(state) && !physics_body_id.is_valid() && !needs_physics_rebuild) {
        return core::Status::failure("physical_resource.missing_physics_body",
                                     "active physical resource state requires a physics body id");
    }
    if ((!state_allows_body(state) || needs_physics_rebuild) && physics_body_id.is_valid()) {
        return core::Status::failure(
            "physical_resource.unexpected_physics_body",
            "inactive physical resource state must not keep a physics body id");
    }
    for (const auto& tag : hazard_tags) {
        if (!core::is_valid_local_id(tag)) {
            return core::Status::failure("physical_resource.invalid_hazard_tag",
                                         "physical resource hazard tag is invalid: " + tag);
        }
    }
    for (const auto& segment : segments) {
        if (!segment.local_position.is_finite()) {
            return core::Status::failure("physical_resource.invalid_segment",
                                         "physical resource segment position must be finite");
        }
        auto shape_status = physics::validate_physics_shape_desc(to_child_shape(segment));
        if (!shape_status) {
            return core::Status::failure(shape_status.error().code, shape_status.error().message);
        }
    }
    return core::Status::ok();
}

core::Result<physics::PhysicsBodyDesc>
make_physical_resource_body_desc(const PhysicalResourceRecord& resource, physics::Vec3 position,
                                 physics::Vec3 linear_velocity, physics::Vec3 rotation_degrees,
                                 physics::Vec3 angular_velocity) {
    if (!position.is_finite() || !linear_velocity.is_finite() || !rotation_degrees.is_finite() ||
        !angular_velocity.is_finite()) {
        return core::Result<physics::PhysicsBodyDesc>::failure(
            "physical_resource.invalid_body_transform",
            "physical resource body transform and velocities must be finite");
    }
    if (resource.state == PhysicalResourceState::converted_to_cargo) {
        return core::Result<physics::PhysicsBodyDesc>::failure(
            "physical_resource.already_cargo",
            "converted physical resources cannot create physics bodies");
    }
    auto status = resource.validate();
    if (!status && status.error().code != "physical_resource.missing_physics_body") {
        return core::Result<physics::PhysicsBodyDesc>::failure(status.error().code,
                                                               status.error().message);
    }
    if (resource.segments.empty()) {
        return core::Result<physics::PhysicsBodyDesc>::failure(
            "physical_resource.no_segments",
            "physical resource needs segments before creating a physics body");
    }

    physics::PhysicsBodyDesc desc;
    const auto frozen = resource.state == PhysicalResourceState::frozen_static;
    desc.motion_type =
        frozen ? physics::BodyMotionType::static_body : physics::BodyMotionType::dynamic;
    desc.mass = frozen ? 0.0F : static_cast<float>(resource.mass_grams) / 1000.0F;
    desc.position = position;
    desc.rotation_degrees = rotation_degrees;
    desc.linear_velocity = frozen ? physics::Vec3{} : linear_velocity;
    desc.angular_velocity = frozen ? physics::Vec3{} : angular_velocity;
    desc.user_data = resource.resource_id.value();
    desc.shape.kind = physics::ShapeKind::compound;
    desc.shape.children.reserve(resource.segments.size());
    for (const auto& segment : resource.segments) {
        desc.shape.children.push_back(to_compound_child(segment));
    }

    status = physics::validate_physics_body_desc(desc);
    if (!status) {
        return core::Result<physics::PhysicsBodyDesc>::failure(status.error().code,
                                                               status.error().message);
    }
    return core::Result<physics::PhysicsBodyDesc>::success(std::move(desc));
}

core::Status attach_physical_resource_body(PhysicalResourceRecord& resource,
                                           physics::PhysicsBodyId body_id) {
    if (!body_id.is_valid()) {
        return core::Status::failure("physical_resource.invalid_physics_body",
                                     "attached physics body id must be valid");
    }
    if (resource.state == PhysicalResourceState::converted_to_cargo) {
        return core::Status::failure("physical_resource.already_cargo",
                                     "converted physical resources cannot attach physics bodies");
    }
    if (resource.physics_body_id.is_valid()) {
        return core::Status::failure("physical_resource.physics_body_already_attached",
                                     "physical resource already owns a physics body");
    }

    auto staged = resource;
    const auto restored_state = staged.state;
    const auto restoring = staged.needs_physics_rebuild;
    staged.physics_body_id = body_id;
    staged.state = restoring ? restored_state : PhysicalResourceState::dynamic;
    staged.needs_physics_rebuild = false;
    auto status = staged.validate();
    if (status) {
        resource = std::move(staged);
    }
    return status;
}

core::Status mark_physical_resource_settled(PhysicalResourceRecord& resource) {
    if (resource.state != PhysicalResourceState::dynamic) {
        return core::Status::failure("physical_resource.not_dynamic",
                                     "only dynamic physical resources can become settled");
    }
    resource.state = PhysicalResourceState::settled_sleeping;
    return resource.validate();
}

core::Status freeze_physical_resource(PhysicalResourceRecord& resource) {
    if (resource.state != PhysicalResourceState::settled_sleeping) {
        return core::Status::failure("physical_resource.not_settled",
                                     "only settled physical resources can freeze static");
    }
    resource.state = PhysicalResourceState::frozen_static;
    resource.linear_velocity = {};
    resource.angular_velocity = {};
    return resource.validate();
}

core::Result<cargo::CargoRecord>
convert_physical_resource_to_cargo(PhysicalResourceRecord& resource, core::SaveId cargo_id,
                                   physics::IPhysicsWorld& physics_world) {
    if (!can_convert_to_cargo(resource.state)) {
        return core::Result<cargo::CargoRecord>::failure(
            "physical_resource.not_convertible",
            "physical resource must be settled or frozen before cargo conversion");
    }
    auto status = resource.validate();
    if (!status) {
        return core::Result<cargo::CargoRecord>::failure(status.error().code,
                                                         status.error().message);
    }

    cargo::CargoRecord cargo_record;
    cargo_record.cargo_id = cargo_id;
    cargo_record.prototype_id = resource.cargo_prototype_id;
    cargo_record.position = resource.position;
    cargo_record.mass_grams = resource.mass_grams;
    cargo_record.volume_milliliters = resource.volume_milliliters;
    cargo_record.stability_per_mille = resource.stability_per_mille;
    cargo_record.allowed_transport_modes = resource.allowed_transport_modes;
    cargo_record.hazard_tags = resource.hazard_tags;
    status = cargo_record.validate();
    if (!status) {
        return core::Result<cargo::CargoRecord>::failure(status.error().code,
                                                         status.error().message);
    }

    if (resource.physics_body_id.is_valid()) {
        status = physics_world.destroy_body(resource.physics_body_id);
        if (!status) {
            return core::Result<cargo::CargoRecord>::failure(status.error().code,
                                                             status.error().message);
        }
    }

    resource.state = PhysicalResourceState::converted_to_cargo;
    resource.physics_body_id = {};
    resource.needs_physics_rebuild = false;
    return core::Result<cargo::CargoRecord>::success(std::move(cargo_record));
}

namespace {

[[nodiscard]] std::vector<std::string_view> split_resource(std::string_view value, char delimiter) {
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

template <typename T> [[nodiscard]] bool parse_resource_number(std::string_view text, T& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] core::Result<physics::ShapeKind> parse_shape_kind(std::string_view value) {
    if (value == "box")
        return core::Result<physics::ShapeKind>::success(physics::ShapeKind::box);
    if (value == "sphere")
        return core::Result<physics::ShapeKind>::success(physics::ShapeKind::sphere);
    if (value == "capsule")
        return core::Result<physics::ShapeKind>::success(physics::ShapeKind::capsule);
    if (value == "compound")
        return core::Result<physics::ShapeKind>::success(physics::ShapeKind::compound);
    return core::Result<physics::ShapeKind>::failure(
        "physical_resource.invalid_shape", "physical resource saved shape is unsupported");
}

} // namespace

std::string PhysicalResourceTextCodec::encode(const PhysicalResourceRecord& resource) {
    std::ostringstream output;
    output << physical_resource_state_magic;
    output << "cargo=" << resource.cargo_prototype_id.value() << '\n';
    output << "kind=" << static_cast<unsigned int>(resource.kind) << '\n';
    output << "state=" << static_cast<unsigned int>(resource.state) << '\n';
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "rotation=" << resource.rotation_degrees.x << ',' << resource.rotation_degrees.y
           << ',' << resource.rotation_degrees.z << '\n';
    output << "linear_velocity=" << resource.linear_velocity.x << ',' << resource.linear_velocity.y
           << ',' << resource.linear_velocity.z << '\n';
    output << "angular_velocity=" << resource.angular_velocity.x << ','
           << resource.angular_velocity.y << ',' << resource.angular_velocity.z << '\n';
    output << "mass=" << resource.mass_grams << '\n';
    output << "volume=" << resource.volume_milliliters << '\n';
    output << "stability=" << resource.stability_per_mille << '\n';
    output << "transport=" << resource.allowed_transport_modes.bits() << '\n';
    output << "hazards=";
    for (std::size_t index = 0; index < resource.hazard_tags.size(); ++index) {
        if (index > 0)
            output << ',';
        output << resource.hazard_tags[index];
    }
    output << '\n';
    for (const auto& segment : resource.segments) {
        output << "segment=" << physics::shape_kind_name(segment.shape) << ','
               << segment.local_position.x << ',' << segment.local_position.y << ','
               << segment.local_position.z << ',' << segment.half_extents.x << ','
               << segment.half_extents.y << ',' << segment.half_extents.z << ',' << segment.radius
               << ',' << segment.half_height << '\n';
    }
    output << "end\n";
    return output.str();
}

core::Result<PhysicalResourceRecord>
PhysicalResourceTextCodec::decode(core::SaveId resource_id, core::PrototypeId prototype_id,
                                  world::WorldPosition position, std::string_view text) {
    if (!text.starts_with(physical_resource_state_magic) || !text.ends_with("end\n")) {
        return core::Result<PhysicalResourceRecord>::failure(
            "physical_resource.invalid_saved_state",
            "physical resource saved state framing is invalid");
    }
    PhysicalResourceRecord resource;
    resource.resource_id = resource_id;
    resource.prototype_id = std::move(prototype_id);
    resource.position = position;
    bool saw_cargo = false;
    bool saw_kind = false;
    bool saw_state = false;
    bool saw_mass = false;
    bool saw_volume = false;
    bool saw_stability = false;
    bool saw_transport = false;
    std::size_t start = physical_resource_state_magic.size();
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end - start);
        if (line == "end")
            break;
        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            return core::Result<PhysicalResourceRecord>::failure(
                "physical_resource.invalid_saved_state",
                "physical resource saved field is malformed");
        }
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (key == "cargo") {
            auto parsed = core::PrototypeId::parse(value);
            if (!parsed)
                return core::Result<PhysicalResourceRecord>::failure(
                    "physical_resource.invalid_saved_state",
                    "physical resource cargo prototype is invalid");
            resource.cargo_prototype_id = parsed.value();
            saw_cargo = true;
        } else if (key == "kind") {
            unsigned int parsed = 0;
            if (!parse_resource_number(value, parsed) ||
                parsed > static_cast<unsigned int>(PhysicalResourceKind::stone_block))
                return core::Result<PhysicalResourceRecord>::failure(
                    "physical_resource.invalid_saved_state", "physical resource kind is invalid");
            resource.kind = static_cast<PhysicalResourceKind>(parsed);
            saw_kind = true;
        } else if (key == "state") {
            unsigned int parsed = 0;
            if (!parse_resource_number(value, parsed) ||
                parsed > static_cast<unsigned int>(PhysicalResourceState::converted_to_cargo))
                return core::Result<PhysicalResourceRecord>::failure(
                    "physical_resource.invalid_saved_state", "physical resource state is invalid");
            resource.state = static_cast<PhysicalResourceState>(parsed);
            saw_state = true;
        } else if (key == "rotation" || key == "linear_velocity" || key == "angular_velocity") {
            const auto fields = split_resource(value, ',');
            physics::Vec3 parsed;
            if (fields.size() != 3 || !parse_resource_number(fields[0], parsed.x) ||
                !parse_resource_number(fields[1], parsed.y) ||
                !parse_resource_number(fields[2], parsed.z)) {
                return core::Result<PhysicalResourceRecord>::failure(
                    "physical_resource.invalid_saved_state",
                    "physical resource saved transform vector is malformed");
            }
            if (key == "rotation") {
                resource.rotation_degrees = parsed;
            } else if (key == "linear_velocity") {
                resource.linear_velocity = parsed;
            } else {
                resource.angular_velocity = parsed;
            }
        } else if (key == "mass") {
            saw_mass = parse_resource_number(value, resource.mass_grams);
        } else if (key == "volume") {
            saw_volume = parse_resource_number(value, resource.volume_milliliters);
        } else if (key == "stability") {
            saw_stability = parse_resource_number(value, resource.stability_per_mille);
        } else if (key == "transport") {
            std::uint32_t bits = 0;
            saw_transport = parse_resource_number(value, bits);
            resource.allowed_transport_modes = cargo::CargoTransportModes::from_bits(bits);
        } else if (key == "hazards") {
            if (!value.empty())
                for (const auto tag : split_resource(value, ','))
                    resource.hazard_tags.emplace_back(tag);
        } else if (key == "segment") {
            const auto fields = split_resource(value, ',');
            if (fields.size() != 9)
                return core::Result<PhysicalResourceRecord>::failure(
                    "physical_resource.invalid_saved_state",
                    "physical resource segment is malformed");
            auto shape = parse_shape_kind(fields[0]);
            PhysicalResourceSegment segment;
            if (!shape || !parse_resource_number(fields[1], segment.local_position.x) ||
                !parse_resource_number(fields[2], segment.local_position.y) ||
                !parse_resource_number(fields[3], segment.local_position.z) ||
                !parse_resource_number(fields[4], segment.half_extents.x) ||
                !parse_resource_number(fields[5], segment.half_extents.y) ||
                !parse_resource_number(fields[6], segment.half_extents.z) ||
                !parse_resource_number(fields[7], segment.radius) ||
                !parse_resource_number(fields[8], segment.half_height)) {
                return core::Result<PhysicalResourceRecord>::failure(
                    "physical_resource.invalid_saved_state",
                    "physical resource segment values are invalid");
            }
            segment.shape = shape.value();
            resource.segments.push_back(segment);
        } else {
            return core::Result<PhysicalResourceRecord>::failure(
                "physical_resource.invalid_saved_state",
                "physical resource saved state contains an unknown field");
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    if (!saw_cargo || !saw_kind || !saw_state || !saw_mass || !saw_volume || !saw_stability ||
        !saw_transport) {
        return core::Result<PhysicalResourceRecord>::failure(
            "physical_resource.incomplete_saved_state",
            "physical resource saved state is incomplete");
    }
    resource.physics_body_id = {};
    resource.needs_physics_rebuild = state_requires_body(resource.state);
    auto status = resource.validate();
    if (!status)
        return core::Result<PhysicalResourceRecord>::failure(status.error().code,
                                                             status.error().message);
    return core::Result<PhysicalResourceRecord>::success(std::move(resource));
}

std::string_view physical_resource_kind_name(PhysicalResourceKind kind) noexcept {
    switch (kind) {
    case PhysicalResourceKind::felled_tree:
        return "felled_tree";
    case PhysicalResourceKind::haulable_log:
        return "haulable_log";
    case PhysicalResourceKind::stone_block:
        return "stone_block";
    }
    return "unknown";
}

std::string_view physical_resource_state_name(PhysicalResourceState state) noexcept {
    switch (state) {
    case PhysicalResourceState::cutting:
        return "cutting";
    case PhysicalResourceState::dynamic:
        return "dynamic";
    case PhysicalResourceState::settled_sleeping:
        return "settled_sleeping";
    case PhysicalResourceState::frozen_static:
        return "frozen_static";
    case PhysicalResourceState::converted_to_cargo:
        return "converted_to_cargo";
    }
    return "unknown";
}

} // namespace heartstead::entities
