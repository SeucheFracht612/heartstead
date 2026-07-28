#include "engine/physics/physics_world.hpp"

#include "engine/physics/jolt/jolt_backend.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace heartstead::physics {

namespace {

[[nodiscard]] bool is_positive_finite(float value) noexcept {
    return std::isfinite(value) && value > 0.0F;
}

constexpr float sleep_linear_velocity_squared = 0.0004F;
constexpr std::uint32_t sleep_step_threshold = 3;

[[nodiscard]] Vec3 add(Vec3 lhs, Vec3 rhs) noexcept {
    return Vec3{lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

[[nodiscard]] Vec3 scale(Vec3 value, float scalar) noexcept {
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] PhysicsAabb make_aabb(Vec3 center, Vec3 half_extents) noexcept {
    return PhysicsAabb{center - half_extents, center + half_extents};
}

[[nodiscard]] PhysicsAabb offset_aabb(PhysicsAabb bounds, Vec3 offset) noexcept {
    return PhysicsAabb{bounds.min + offset, bounds.max + offset};
}

[[nodiscard]] PhysicsAabb shape_local_aabb(const PhysicsShapeDesc& shape);

[[nodiscard]] PhysicsAabb child_local_aabb(const CompoundShapeChild& child) {
    PhysicsShapeDesc child_shape;
    child_shape.kind = child.kind;
    child_shape.half_extents = child.half_extents;
    child_shape.radius = child.radius;
    child_shape.half_height = child.half_height;
    return offset_aabb(shape_local_aabb(child_shape), child.local_position);
}

[[nodiscard]] PhysicsAabb shape_local_aabb(const PhysicsShapeDesc& shape) {
    switch (shape.kind) {
    case ShapeKind::box:
        return make_aabb(Vec3{}, shape.half_extents);
    case ShapeKind::sphere:
        return make_aabb(Vec3{}, Vec3{shape.radius, shape.radius, shape.radius});
    case ShapeKind::capsule:
        return make_aabb(Vec3{},
                         Vec3{shape.radius, shape.half_height + shape.radius, shape.radius});
    case ShapeKind::compound: {
        auto bounds = child_local_aabb(shape.children.front());
        for (std::size_t index = 1; index < shape.children.size(); ++index) {
            bounds = bounds.merged_with(child_local_aabb(shape.children[index]));
        }
        return bounds;
    }
    }

    return PhysicsAabb{};
}

[[nodiscard]] bool aabb_overlaps(PhysicsAabb lhs, PhysicsAabb rhs) noexcept {
    return lhs.min.x <= rhs.max.x && lhs.max.x >= rhs.min.x && lhs.min.y <= rhs.max.y &&
           lhs.max.y >= rhs.min.y && lhs.min.z <= rhs.max.z && lhs.max.z >= rhs.min.z;
}

[[nodiscard]] Vec3 aabb_center(PhysicsAabb bounds) noexcept {
    return (bounds.min + bounds.max) * 0.5F;
}

[[nodiscard]] float aabb_axis_overlap(float first_min, float first_max, float second_min,
                                      float second_max) noexcept {
    return std::min(first_max, second_max) - std::max(first_min, second_min);
}

[[nodiscard]] PhysicsContact make_contact(PhysicsBodyId first, const PhysicsBodyState& first_state,
                                          PhysicsAabb first_bounds, PhysicsBodyId second,
                                          const PhysicsBodyState& second_state,
                                          PhysicsAabb second_bounds) noexcept {
    const auto overlap_x = aabb_axis_overlap(first_bounds.min.x, first_bounds.max.x,
                                             second_bounds.min.x, second_bounds.max.x);
    const auto overlap_y = aabb_axis_overlap(first_bounds.min.y, first_bounds.max.y,
                                             second_bounds.min.y, second_bounds.max.y);
    const auto overlap_z = aabb_axis_overlap(first_bounds.min.z, first_bounds.max.z,
                                             second_bounds.min.z, second_bounds.max.z);
    const auto penetration = std::min({overlap_x, overlap_y, overlap_z});
    const auto delta = aabb_center(second_bounds) - aabb_center(first_bounds);

    Vec3 normal{1.0F, 0.0F, 0.0F};
    if (overlap_y <= overlap_x && overlap_y <= overlap_z) {
        normal = Vec3{0.0F, delta.y < 0.0F ? -1.0F : 1.0F, 0.0F};
    } else if (overlap_z <= overlap_x && overlap_z <= overlap_y) {
        normal = Vec3{0.0F, 0.0F, delta.z < 0.0F ? -1.0F : 1.0F};
    } else {
        normal = Vec3{delta.x < 0.0F ? -1.0F : 1.0F, 0.0F, 0.0F};
    }

    return PhysicsContact{
        first,         second, first_state.user_data, second_state.user_data, first_bounds,
        second_bounds, normal, penetration,
    };
}

[[nodiscard]] core::Status validate_compound_child(const CompoundShapeChild& child) {
    if (!child.local_position.is_finite()) {
        return core::Status::failure("physics.invalid_shape",
                                     "compound child local position must be finite");
    }

    PhysicsShapeDesc shape;
    shape.kind = child.kind;
    shape.half_extents = child.half_extents;
    shape.radius = child.radius;
    shape.half_height = child.half_height;
    return validate_physics_shape_desc(shape);
}

class HeadlessPhysicsWorld final : public IPhysicsWorld {
  public:
    explicit HeadlessPhysicsWorld(PhysicsWorldDesc desc) : desc_(desc) {}

    [[nodiscard]] PhysicsBackend backend() const noexcept override {
        return PhysicsBackend::headless;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return physics_backend_name(PhysicsBackend::headless);
    }

    [[nodiscard]] PhysicsBackendCapabilities capabilities() const noexcept override {
        return physics_backend_capabilities(PhysicsBackend::headless);
    }

    [[nodiscard]] std::uint32_t body_count() const noexcept override {
        return static_cast<std::uint32_t>(bodies_.size());
    }

    [[nodiscard]] core::Result<PhysicsBodyId> create_body(PhysicsBodyDesc desc) override {
        auto status = validate_physics_body_desc(desc);
        if (!status) {
            return core::Result<PhysicsBodyId>::failure(status.error().code,
                                                        status.error().message);
        }

        const auto id = next_body_id();
        bodies_.emplace(id.value(),
                        BodyRecord{desc, PhysicsBodyState{id, desc.motion_type, desc.position,
                                                          desc.linear_velocity, desc.mass, false,
                                                          desc.user_data}});
        return core::Result<PhysicsBodyId>::success(id);
    }

    [[nodiscard]] core::Status destroy_body(PhysicsBodyId id) override {
        if (!id.is_valid() || bodies_.erase(id.value()) == 0) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }
        contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(),
                                       [id](const PhysicsContact& contact) {
                                           return contact.first == id || contact.second == id;
                                       }),
                        contacts_.end());
        return core::Status::ok();
    }

    [[nodiscard]] std::optional<PhysicsBodyState>
    body_state(PhysicsBodyId id) const noexcept override {
        const auto found = bodies_.find(id.value());
        if (found == bodies_.end()) {
            return std::nullopt;
        }
        return found->second.state;
    }

    [[nodiscard]] core::Status set_body_position(PhysicsBodyId id, Vec3 position) override {
        if (!position.is_finite()) {
            return core::Status::failure("physics.invalid_position",
                                         "body position must be finite");
        }
        auto* body = find_body(id);
        if (body == nullptr) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }
        body->state.position = position;
        body->state.sleeping = false;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status set_linear_velocity(PhysicsBodyId id, Vec3 velocity) override {
        if (!velocity.is_finite()) {
            return core::Status::failure("physics.invalid_velocity",
                                         "body linear velocity must be finite");
        }
        auto* body = find_body(id);
        if (body == nullptr) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }
        if (body->state.motion_type == BodyMotionType::static_body) {
            return core::Status::failure("physics.static_body_velocity",
                                         "static bodies cannot receive linear velocity");
        }
        body->state.linear_velocity = velocity;
        body->state.sleeping = false;
        return core::Status::ok();
    }

    [[nodiscard]] core::Status apply_impulse(PhysicsBodyId id, Vec3 impulse) override {
        if (!impulse.is_finite()) {
            return core::Status::failure("physics.invalid_impulse", "impulse must be finite");
        }
        auto* body = find_body(id);
        if (body == nullptr) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }
        if (body->state.motion_type != BodyMotionType::dynamic) {
            return core::Status::failure("physics.impulse_requires_dynamic_body",
                                         "only dynamic bodies can receive impulses");
        }

        body->state.linear_velocity =
            add(body->state.linear_velocity, scale(impulse, 1.0F / body->state.mass));
        body->state.sleeping = false;
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<std::vector<PhysicsOverlap>>
    query_aabb(PhysicsAabb bounds) const override {
        auto status = validate_physics_aabb(bounds);
        if (!status) {
            return core::Result<std::vector<PhysicsOverlap>>::failure(status.error().code,
                                                                      status.error().message);
        }

        std::vector<std::uint64_t> ids;
        ids.reserve(bodies_.size());
        for (const auto& [id, _] : bodies_) {
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());

        std::vector<PhysicsOverlap> overlaps;
        for (const auto id : ids) {
            const auto& body = bodies_.at(id);
            const auto body_bounds = body_aabb(body);
            if (aabb_overlaps(body_bounds, bounds)) {
                overlaps.push_back(
                    PhysicsOverlap{body.state.id, body.state.user_data, body_bounds});
            }
        }

        return core::Result<std::vector<PhysicsOverlap>>::success(std::move(overlaps));
    }

    [[nodiscard]] std::vector<PhysicsContact> drain_contacts() override {
        auto contacts = std::move(contacts_);
        contacts_.clear();
        return contacts;
    }

    [[nodiscard]] core::Result<PhysicsStepStats> step(PhysicsStepDesc desc) override {
        if (!is_positive_finite(desc.delta_seconds)) {
            return core::Result<PhysicsStepStats>::failure(
                "physics.invalid_timestep", "physics timestep must be positive and finite");
        }

        PhysicsStepStats stats;
        stats.body_count = body_count();
        stats.simulated_seconds = desc.delta_seconds;

        for (const auto id : sorted_body_ids()) {
            auto& body = bodies_.at(id);
            if (body.state.motion_type == BodyMotionType::dynamic) {
                ++stats.dynamic_body_count;
                if (body.state.sleeping) {
                    continue;
                }

                body.state.linear_velocity =
                    add(body.state.linear_velocity,
                        scale(desc_.gravity, desc.delta_seconds * body.desc.gravity_scale));
                body.state.position =
                    add(body.state.position, scale(body.state.linear_velocity, desc.delta_seconds));
                ++stats.integrated_body_count;
                continue;
            }

            if (body.state.motion_type == BodyMotionType::kinematic) {
                body.state.position =
                    add(body.state.position, scale(body.state.linear_velocity, desc.delta_seconds));
                ++stats.integrated_body_count;
            }
        }

        contacts_ = collect_contacts();
        resolve_contacts(contacts_);
        contacts_ = collect_contacts();
        update_sleeping_state(contacts_);
        stats.contact_count = static_cast<std::uint32_t>(contacts_.size());

        return core::Result<PhysicsStepStats>::success(stats);
    }

  private:
    struct BodyRecord {
        PhysicsBodyDesc desc;
        PhysicsBodyState state;
        std::uint32_t idle_step_count = 0;
    };

    [[nodiscard]] PhysicsBodyId next_body_id() {
        const auto id = PhysicsBodyId::from_value(next_body_id_);
        ++next_body_id_;
        return id;
    }

    [[nodiscard]] BodyRecord* find_body(PhysicsBodyId id) noexcept {
        const auto found = bodies_.find(id.value());
        return found == bodies_.end() ? nullptr : &found->second;
    }

    [[nodiscard]] std::vector<std::uint64_t> sorted_body_ids() const {
        std::vector<std::uint64_t> ids;
        ids.reserve(bodies_.size());
        for (const auto& [id, _] : bodies_) {
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    [[nodiscard]] PhysicsAabb body_aabb(const BodyRecord& body) const {
        return offset_aabb(shape_local_aabb(body.desc.shape), body.state.position);
    }

    [[nodiscard]] std::vector<PhysicsContact> collect_contacts() const {
        const auto ids = sorted_body_ids();

        std::vector<PhysicsContact> contacts;
        for (std::size_t first_index = 0; first_index < ids.size(); ++first_index) {
            const auto& first = bodies_.at(ids[first_index]);
            const auto first_bounds = body_aabb(first);
            for (std::size_t second_index = first_index + 1; second_index < ids.size();
                 ++second_index) {
                const auto& second = bodies_.at(ids[second_index]);
                if (first.state.motion_type == BodyMotionType::static_body &&
                    second.state.motion_type == BodyMotionType::static_body) {
                    continue;
                }

                const auto second_bounds = body_aabb(second);
                if (!aabb_overlaps(first_bounds, second_bounds)) {
                    continue;
                }

                contacts.push_back(make_contact(first.state.id, first.state, first_bounds,
                                                second.state.id, second.state, second_bounds));
            }
        }

        return contacts;
    }

    void resolve_contacts(const std::vector<PhysicsContact>& contacts) {
        for (const auto& contact : contacts) {
            auto* first = find_body(contact.first);
            auto* second = find_body(contact.second);
            if (first == nullptr || second == nullptr) {
                continue;
            }

            const auto first_dynamic = first->state.motion_type == BodyMotionType::dynamic;
            const auto second_dynamic = second->state.motion_type == BodyMotionType::dynamic;
            if (!first_dynamic && !second_dynamic) {
                continue;
            }

            if (first_dynamic && second_dynamic) {
                apply_dynamic_response(*first, scale(contact.normal, -1.0F),
                                       contact.penetration_depth * 0.5F);
                apply_dynamic_response(*second, contact.normal, contact.penetration_depth * 0.5F);
                continue;
            }

            if (first_dynamic) {
                apply_dynamic_response(*first, scale(contact.normal, -1.0F),
                                       contact.penetration_depth);
            } else {
                apply_dynamic_response(*second, contact.normal, contact.penetration_depth);
            }
        }
    }

    void apply_dynamic_response(BodyRecord& body, Vec3 correction_direction,
                                float penetration_depth) noexcept {
        if (penetration_depth > 0.0F) {
            body.state.position =
                add(body.state.position, scale(correction_direction, penetration_depth));
        }

        const auto velocity_into_contact =
            math::dot(body.state.linear_velocity, correction_direction);
        if (velocity_into_contact < 0.0F) {
            body.state.linear_velocity =
                body.state.linear_velocity - scale(correction_direction, velocity_into_contact);
        }

        body.state.sleeping = false;
    }

    [[nodiscard]] bool has_contact(PhysicsBodyId id,
                                   const std::vector<PhysicsContact>& contacts) const noexcept {
        return std::any_of(contacts.begin(), contacts.end(), [id](const PhysicsContact& contact) {
            return contact.first == id || contact.second == id;
        });
    }

    void update_sleeping_state(const std::vector<PhysicsContact>& contacts) {
        for (const auto id : sorted_body_ids()) {
            auto& body = bodies_.at(id);
            if (body.state.motion_type != BodyMotionType::dynamic) {
                body.state.sleeping = false;
                body.idle_step_count = 0;
                continue;
            }
            if (!body.desc.allow_sleep) {
                body.state.sleeping = false;
                body.idle_step_count = 0;
                continue;
            }

            const auto speed_squared = math::length_squared(body.state.linear_velocity);
            if (speed_squared <= sleep_linear_velocity_squared &&
                has_contact(body.state.id, contacts)) {
                ++body.idle_step_count;
                if (body.idle_step_count >= sleep_step_threshold) {
                    body.state.linear_velocity = {};
                    body.state.sleeping = true;
                }
                continue;
            }

            body.state.sleeping = false;
            body.idle_step_count = 0;
        }
    }

    PhysicsWorldDesc desc_;
    std::uint64_t next_body_id_ = 1;
    std::unordered_map<std::uint64_t, BodyRecord> bodies_;
    std::vector<PhysicsContact> contacts_;
};

} // namespace

core::Result<std::unique_ptr<IPhysicsWorld>> create_physics_world(PhysicsWorldDesc desc) {
    auto status = validate_physics_world_desc(desc);
    if (!status) {
        return core::Result<std::unique_ptr<IPhysicsWorld>>::failure(status.error().code,
                                                                     status.error().message);
    }

    switch (desc.backend) {
    case PhysicsBackend::headless:
        return core::Result<std::unique_ptr<IPhysicsWorld>>::success(
            std::make_unique<HeadlessPhysicsWorld>(desc));
    case PhysicsBackend::jolt:
        return jolt::create_world(desc);
    }

    return core::Result<std::unique_ptr<IPhysicsWorld>>::failure("physics.unknown_backend",
                                                                 "unknown physics backend");
}

core::Status validate_physics_world_desc(const PhysicsWorldDesc& desc) {
    if (!desc.gravity.is_finite()) {
        return core::Status::failure("physics.invalid_gravity", "gravity vector must be finite");
    }
    return core::Status::ok();
}

core::Status validate_physics_body_desc(const PhysicsBodyDesc& desc) {
    auto shape_status = validate_physics_shape_desc(desc.shape);
    if (!shape_status) {
        return shape_status;
    }
    if (!desc.position.is_finite()) {
        return core::Status::failure("physics.invalid_position", "body position must be finite");
    }
    if (!desc.linear_velocity.is_finite()) {
        return core::Status::failure("physics.invalid_velocity",
                                     "body linear velocity must be finite");
    }
    if (!std::isfinite(desc.gravity_scale)) {
        return core::Status::failure("physics.invalid_gravity_scale",
                                     "body gravity scale must be finite");
    }
    if (desc.motion_type == BodyMotionType::dynamic && !is_positive_finite(desc.mass)) {
        return core::Status::failure("physics.invalid_mass",
                                     "dynamic bodies require positive finite mass");
    }
    if (desc.motion_type != BodyMotionType::dynamic && desc.mass < 0.0F) {
        return core::Status::failure("physics.invalid_mass", "body mass must not be negative");
    }
    return core::Status::ok();
}

core::Status validate_physics_shape_desc(const PhysicsShapeDesc& desc) {
    switch (desc.kind) {
    case ShapeKind::box:
        if (!is_positive_finite(desc.half_extents.x) || !is_positive_finite(desc.half_extents.y) ||
            !is_positive_finite(desc.half_extents.z)) {
            return core::Status::failure("physics.invalid_shape",
                                         "box half extents must be positive and finite");
        }
        return core::Status::ok();
    case ShapeKind::sphere:
        if (!is_positive_finite(desc.radius)) {
            return core::Status::failure("physics.invalid_shape",
                                         "sphere radius must be positive and finite");
        }
        return core::Status::ok();
    case ShapeKind::capsule:
        if (!is_positive_finite(desc.radius) || !is_positive_finite(desc.half_height)) {
            return core::Status::failure(
                "physics.invalid_shape",
                "capsule radius and half height must be positive and finite");
        }
        return core::Status::ok();
    case ShapeKind::compound:
        if (desc.children.empty()) {
            return core::Status::failure("physics.invalid_shape",
                                         "compound shape requires at least one child");
        }
        for (const auto& child : desc.children) {
            auto child_status = validate_compound_child(child);
            if (!child_status) {
                return child_status;
            }
        }
        return core::Status::ok();
    }

    return core::Status::failure("physics.invalid_shape", "unknown physics shape kind");
}

core::Status validate_physics_aabb(const PhysicsAabb& bounds) {
    if (!bounds.is_valid()) {
        return core::Status::failure("physics.invalid_aabb",
                                     "physics AABB min/max values must be finite and ordered");
    }
    return core::Status::ok();
}

PhysicsBackendInfo physics_backend_info(PhysicsBackend backend) noexcept {
    switch (backend) {
    case PhysicsBackend::headless:
        return PhysicsBackendInfo{PhysicsBackend::headless,
                                  physics_backend_name(PhysicsBackend::headless), true,
                                  "available"};
    case PhysicsBackend::jolt:
        return jolt::backend_info();
    }
    return PhysicsBackendInfo{backend, "unknown", false, "unknown physics backend"};
}

PhysicsBackendCapabilities physics_backend_capabilities(PhysicsBackend backend) noexcept {
    const auto info = physics_backend_info(backend);
    switch (backend) {
    case PhysicsBackend::headless:
        return PhysicsBackendCapabilities{
            PhysicsBackend::headless,
            info.available,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            false,
            false,
            true,
            "headless",
        };
    case PhysicsBackend::jolt:
        return PhysicsBackendCapabilities{
            PhysicsBackend::jolt,
            info.available,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            true,
            "jolt",
        };
    }
    return PhysicsBackendCapabilities{
        backend, false, false, false, false, false, false,
        false,   false, false, false, false, false, "unknown",
    };
}

std::string_view physics_backend_name(PhysicsBackend backend) noexcept {
    switch (backend) {
    case PhysicsBackend::headless:
        return "headless";
    case PhysicsBackend::jolt:
        return "jolt";
    }
    return "unknown";
}

std::string_view body_motion_type_name(BodyMotionType type) noexcept {
    switch (type) {
    case BodyMotionType::static_body:
        return "static";
    case BodyMotionType::kinematic:
        return "kinematic";
    case BodyMotionType::dynamic:
        return "dynamic";
    }
    return "unknown";
}

std::string_view shape_kind_name(ShapeKind kind) noexcept {
    switch (kind) {
    case ShapeKind::box:
        return "box";
    case ShapeKind::sphere:
        return "sphere";
    case ShapeKind::capsule:
        return "capsule";
    case ShapeKind::compound:
        return "compound";
    }
    return "unknown";
}

} // namespace heartstead::physics
