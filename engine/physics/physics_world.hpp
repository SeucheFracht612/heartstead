#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace heartstead::physics {

struct PhysicsBodyIdTag;
using PhysicsBodyId = core::StrongU64Id<PhysicsBodyIdTag>;

enum class PhysicsBackend {
    headless,
    jolt,
};

enum class BodyMotionType {
    static_body,
    kinematic,
    dynamic,
};

enum class ShapeKind {
    box,
    sphere,
    capsule,
    compound,
};

enum class PhysicsCharacterGroundState {
    on_ground,
    on_steep_ground,
    not_supported,
    in_air,
};

using Vec3 = math::Vec3f;
using PhysicsAabb = math::Bounds3f;

struct PhysicsBackendInfo {
    PhysicsBackend backend = PhysicsBackend::headless;
    std::string_view name;
    bool available = false;
    std::string_view status;
};

struct PhysicsBackendCapabilities {
    PhysicsBackend backend = PhysicsBackend::headless;
    bool available = false;
    bool deterministic = false;
    bool supports_dynamic_bodies = false;
    bool supports_kinematic_bodies = false;
    bool supports_static_bodies = false;
    bool supports_compound_shapes = false;
    bool supports_aabb_queries = false;
    bool supports_contacts = false;
    bool supports_sleeping = false;
    bool supports_character_controllers = false;
    bool supports_constraints = false;
    bool supports_collision_response = false;
    std::string_view library;
};

struct CompoundShapeChild {
    ShapeKind kind = ShapeKind::box;
    Vec3 local_position{};
    Vec3 half_extents{0.5F, 0.5F, 0.5F};
    float radius = 0.5F;
    float half_height = 0.5F;
};

struct PhysicsShapeDesc {
    ShapeKind kind = ShapeKind::box;
    Vec3 half_extents{0.5F, 0.5F, 0.5F};
    float radius = 0.5F;
    float half_height = 0.5F;
    std::vector<CompoundShapeChild> children;
};

struct PhysicsBodyDesc {
    BodyMotionType motion_type = BodyMotionType::static_body;
    PhysicsShapeDesc shape{};
    Vec3 position{};
    Vec3 rotation_degrees{};
    Vec3 linear_velocity{};
    float mass = 0.0F;
    float gravity_scale = 1.0F;
    bool allow_sleep = true;
    std::uint64_t user_data = 0;
};

struct PhysicsBodyState {
    PhysicsBodyId id;
    BodyMotionType motion_type = BodyMotionType::static_body;
    Vec3 position{};
    Vec3 rotation_degrees{};
    Vec3 linear_velocity{};
    float mass = 0.0F;
    bool sleeping = false;
    std::uint64_t user_data = 0;
};

struct PhysicsOverlap {
    PhysicsBodyId body;
    std::uint64_t user_data = 0;
    PhysicsAabb bounds{};
};

struct PhysicsContact {
    PhysicsBodyId first;
    PhysicsBodyId second;
    std::uint64_t first_user_data = 0;
    std::uint64_t second_user_data = 0;
    PhysicsAabb first_bounds{};
    PhysicsAabb second_bounds{};
    Vec3 normal{};
    float penetration_depth = 0.0F;
};

struct PhysicsWorldDesc {
    PhysicsBackend backend = PhysicsBackend::headless;
    Vec3 gravity{0.0F, -9.81F, 0.0F};
};

struct PhysicsStepDesc {
    float delta_seconds = 1.0F / 60.0F;
};

struct PhysicsStepStats {
    std::uint32_t body_count = 0;
    std::uint32_t dynamic_body_count = 0;
    std::uint32_t integrated_body_count = 0;
    std::uint32_t contact_count = 0;
    float simulated_seconds = 0.0F;
};

struct PhysicsCharacterShapeDesc {
    float width = 0.6F;
    float height = 1.8F;
};

struct PhysicsCharacterDesc {
    PhysicsCharacterShapeDesc shape{};
    Vec3 position{};
    float max_slope_angle_degrees = 50.0F;
    float mass = 70.0F;
    float max_strength = 100.0F;
    float padding = 0.02F;
};

struct PhysicsCharacterMoveDesc {
    Vec3 desired_delta{};
    float delta_seconds = 1.0F / 60.0F;
    float step_height = 0.0F;
    float stick_to_floor_distance = 0.05F;
};

struct PhysicsCharacterMoveResult {
    Vec3 position{};
    Vec3 requested_delta{};
    Vec3 applied_delta{};
    Vec3 linear_velocity{};
    Vec3 ground_normal{};
    Vec3 ground_velocity{};
    PhysicsCharacterGroundState ground_state = PhysicsCharacterGroundState::in_air;
    bool stepped = false;
    bool maximum_hits_exceeded = false;
};

class IPhysicsCharacter {
  public:
    virtual ~IPhysicsCharacter() = default;

    [[nodiscard]] virtual PhysicsBackend backend() const noexcept = 0;
    [[nodiscard]] virtual Vec3 position() const noexcept = 0;
    [[nodiscard]] virtual PhysicsCharacterShapeDesc shape() const noexcept = 0;
    [[nodiscard]] virtual core::Status set_position(Vec3 position) = 0;
    [[nodiscard]] virtual core::Result<bool> set_shape(PhysicsCharacterShapeDesc shape,
                                                       float maximum_penetration_depth = 0.05F) = 0;
    [[nodiscard]] virtual core::Result<PhysicsCharacterMoveResult>
    move(PhysicsCharacterMoveDesc desc) = 0;
    [[nodiscard]] virtual core::Result<Vec3>
    recover_from_penetration(std::uint32_t maximum_iterations = 8) = 0;
    [[nodiscard]] virtual core::Result<bool> has_support(float probe_distance = 0.05F) = 0;
};

class IPhysicsWorld {
  public:
    virtual ~IPhysicsWorld() = default;

    [[nodiscard]] virtual PhysicsBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual PhysicsBackendCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t body_count() const noexcept = 0;

    [[nodiscard]] virtual core::Result<PhysicsBodyId> create_body(PhysicsBodyDesc desc) = 0;
    [[nodiscard]] virtual core::Status destroy_body(PhysicsBodyId id) = 0;
    [[nodiscard]] virtual std::optional<PhysicsBodyState>
    body_state(PhysicsBodyId id) const noexcept = 0;

    [[nodiscard]] virtual core::Status set_body_position(PhysicsBodyId id, Vec3 position) = 0;
    [[nodiscard]] virtual core::Status set_body_rotation(PhysicsBodyId id,
                                                         Vec3 rotation_degrees) = 0;
    [[nodiscard]] virtual core::Status set_linear_velocity(PhysicsBodyId id, Vec3 velocity) = 0;
    [[nodiscard]] virtual core::Status apply_impulse(PhysicsBodyId id, Vec3 impulse) = 0;
    [[nodiscard]] virtual core::Result<std::vector<PhysicsOverlap>>
    query_aabb(PhysicsAabb bounds) const = 0;
    [[nodiscard]] virtual std::vector<PhysicsContact> drain_contacts() = 0;
    [[nodiscard]] virtual core::Result<PhysicsStepStats> step(PhysicsStepDesc desc) = 0;
    [[nodiscard]] virtual core::Result<std::unique_ptr<IPhysicsCharacter>>
    create_character(PhysicsCharacterDesc desc);
};

[[nodiscard]] core::Result<std::unique_ptr<IPhysicsWorld>>
create_physics_world(PhysicsWorldDesc desc);

[[nodiscard]] core::Status validate_physics_world_desc(const PhysicsWorldDesc& desc);
[[nodiscard]] core::Status validate_physics_body_desc(const PhysicsBodyDesc& desc);
[[nodiscard]] core::Status validate_physics_shape_desc(const PhysicsShapeDesc& desc);
[[nodiscard]] core::Status validate_physics_aabb(const PhysicsAabb& bounds);
[[nodiscard]] core::Status
validate_physics_character_shape_desc(const PhysicsCharacterShapeDesc& desc);
[[nodiscard]] core::Status validate_physics_character_desc(const PhysicsCharacterDesc& desc);
[[nodiscard]] core::Status
validate_physics_character_move_desc(const PhysicsCharacterMoveDesc& desc);

[[nodiscard]] PhysicsBackendInfo physics_backend_info(PhysicsBackend backend) noexcept;
[[nodiscard]] PhysicsBackendCapabilities
physics_backend_capabilities(PhysicsBackend backend) noexcept;
[[nodiscard]] std::string_view physics_backend_name(PhysicsBackend backend) noexcept;
[[nodiscard]] std::string_view body_motion_type_name(BodyMotionType type) noexcept;
[[nodiscard]] std::string_view shape_kind_name(ShapeKind kind) noexcept;
[[nodiscard]] std::string_view
physics_character_ground_state_name(PhysicsCharacterGroundState state) noexcept;

} // namespace heartstead::physics
