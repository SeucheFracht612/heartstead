#include "engine/physics/jolt/jolt_backend.hpp"

#if HEARTSTEAD_HAS_JOLT

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNING_PUSH
JPH_SUPPRESS_WARNINGS
#include <Jolt/Core/Factory.h>
#if HEARTSTEAD_JOLT_SINGLE_THREADED
#include <Jolt/Core/JobSystemSingleThreaded.h>
#else
#include <Jolt/Core/JobSystemThreadPool.h>
#endif
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
JPH_SUPPRESS_WARNING_POP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#endif

namespace heartstead::physics::jolt {

#if HEARTSTEAD_HAS_JOLT

namespace {

#if HEARTSTEAD_JOLT_SINGLE_THREADED
using JoltJobSystem = JPH::JobSystemSingleThreaded;
#else
using JoltJobSystem = JPH::JobSystemThreadPool;
#endif

constexpr JPH::ObjectLayer static_layer = 0;
constexpr JPH::ObjectLayer moving_layer = 1;
constexpr std::uint32_t object_layer_count = 2;
constexpr std::uint32_t broad_phase_layer_count = 2;
constexpr std::uint32_t maximum_bodies = 65'536;
constexpr std::uint32_t maximum_body_pairs = 65'536;
constexpr std::uint32_t maximum_contact_constraints = 20'480;
constexpr std::uint32_t temporary_allocator_bytes = 16u * 1024u * 1024u;

constexpr JPH::BroadPhaseLayer static_broad_phase_layer{0};
constexpr JPH::BroadPhaseLayer moving_broad_phase_layer{1};

[[nodiscard]] JPH::Vec3 to_jolt(Vec3 value) noexcept {
    return JPH::Vec3{value.x, value.y, value.z};
}

[[nodiscard]] JPH::RVec3 to_jolt_position(Vec3 value) noexcept {
    return JPH::RVec3{value.x, value.y, value.z};
}

[[nodiscard]] Vec3 from_jolt(JPH::Vec3Arg value) noexcept {
    return Vec3{value.GetX(), value.GetY(), value.GetZ()};
}

[[nodiscard]] Vec3 from_jolt_position(JPH::RVec3Arg value) noexcept {
    return Vec3{static_cast<float>(value.GetX()), static_cast<float>(value.GetY()),
                static_cast<float>(value.GetZ())};
}

[[nodiscard]] PhysicsAabb from_jolt(const JPH::AABox& bounds) noexcept {
    return PhysicsAabb{from_jolt(bounds.mMin), from_jolt(bounds.mMax)};
}

[[nodiscard]] JPH::EMotionType to_jolt(BodyMotionType type) noexcept {
    switch (type) {
    case BodyMotionType::static_body:
        return JPH::EMotionType::Static;
    case BodyMotionType::kinematic:
        return JPH::EMotionType::Kinematic;
    case BodyMotionType::dynamic:
        return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

[[nodiscard]] JPH::ObjectLayer layer_for(BodyMotionType type) noexcept {
    return type == BodyMotionType::static_body ? static_layer : moving_layer;
}

[[nodiscard]] JPH::EActivation activation_for(BodyMotionType type) noexcept {
    return type == BodyMotionType::static_body ? JPH::EActivation::DontActivate
                                               : JPH::EActivation::Activate;
}

[[nodiscard]] core::Result<JPH::ShapeRefC> shape_result(JPH::Shape::ShapeResult result,
                                                        std::string_view kind) {
    if (!result.IsValid()) {
        const auto detail =
            result.HasError() ? std::string(result.GetError().c_str()) : "unknown Jolt shape error";
        return core::Result<JPH::ShapeRefC>::failure("physics.jolt_shape_creation_failed",
                                                     "Jolt rejected " + std::string(kind) +
                                                         " shape: " + detail);
    }
    JPH::ShapeRefC shape = result.Get();
    return core::Result<JPH::ShapeRefC>::success(std::move(shape));
}

[[nodiscard]] core::Result<JPH::ShapeRefC> make_leaf_shape(ShapeKind kind, Vec3 half_extents,
                                                           float radius, float half_height) {
    switch (kind) {
    case ShapeKind::box: {
        JPH::BoxShapeSettings settings(to_jolt(half_extents));
        settings.SetEmbedded();
        return shape_result(settings.Create(), "box");
    }
    case ShapeKind::sphere: {
        JPH::SphereShapeSettings settings(radius);
        settings.SetEmbedded();
        return shape_result(settings.Create(), "sphere");
    }
    case ShapeKind::capsule: {
        JPH::CapsuleShapeSettings settings(half_height, radius);
        settings.SetEmbedded();
        return shape_result(settings.Create(), "capsule");
    }
    case ShapeKind::compound:
        return core::Result<JPH::ShapeRefC>::failure(
            "physics.jolt_shape_creation_failed",
            "nested compound shapes are not supported by the physics boundary");
    }
    return core::Result<JPH::ShapeRefC>::failure("physics.jolt_shape_creation_failed",
                                                 "unknown physics shape kind");
}

[[nodiscard]] core::Result<JPH::ShapeRefC> make_shape(const PhysicsShapeDesc& desc) {
    if (desc.kind != ShapeKind::compound) {
        return make_leaf_shape(desc.kind, desc.half_extents, desc.radius, desc.half_height);
    }

    JPH::StaticCompoundShapeSettings settings;
    settings.SetEmbedded();
    for (const auto& child : desc.children) {
        auto shape =
            make_leaf_shape(child.kind, child.half_extents, child.radius, child.half_height);
        if (!shape) {
            return shape;
        }
        settings.AddShape(to_jolt(child.local_position), JPH::Quat::sIdentity(),
                          shape.value().GetPtr());
    }
    return shape_result(settings.Create(), "compound");
}

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
  public:
    BroadPhaseLayerInterface() noexcept {
        object_to_broad_phase_[static_layer] = static_broad_phase_layer;
        object_to_broad_phase_[moving_layer] = moving_broad_phase_layer;
    }

    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override {
        return broad_phase_layer_count;
    }

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        if (layer >= object_layer_count) {
            return static_broad_phase_layer;
        }
        return object_to_broad_phase_[layer];
    }

  private:
    std::array<JPH::BroadPhaseLayer, object_layer_count> object_to_broad_phase_{};
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer object_layer,
                                     JPH::BroadPhaseLayer broad_phase_layer) const override {
        if (object_layer == static_layer) {
            return broad_phase_layer == moving_broad_phase_layer;
        }
        return object_layer == moving_layer;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
  public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer first,
                                     JPH::ObjectLayer second) const override {
        if (first == static_layer) {
            return second == moving_layer;
        }
        return first == moving_layer && second < object_layer_count;
    }
};

std::once_flag allocator_registration;
std::mutex runtime_mutex;
std::weak_ptr<class RuntimeRegistration> current_runtime;

class RuntimeRegistration {
  public:
    RuntimeRegistration() {
        std::call_once(allocator_registration, [] { JPH::RegisterDefaultAllocator(); });
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    ~RuntimeRegistration() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    RuntimeRegistration(const RuntimeRegistration&) = delete;
    RuntimeRegistration& operator=(const RuntimeRegistration&) = delete;
};

[[nodiscard]] std::shared_ptr<RuntimeRegistration> acquire_runtime() {
    std::scoped_lock lock(runtime_mutex);
    if (auto runtime = current_runtime.lock()) {
        return runtime;
    }
    auto runtime = std::make_shared<RuntimeRegistration>();
    current_runtime = runtime;
    return runtime;
}

class JoltPhysicsWorld final : public IPhysicsWorld {
  public:
    explicit JoltPhysicsWorld(PhysicsWorldDesc desc)
        : desc_(desc), runtime_(acquire_runtime()), contact_listener_(*this),
          temporary_allocator_(temporary_allocator_bytes),
#if HEARTSTEAD_JOLT_SINGLE_THREADED
          job_system_(JPH::cMaxPhysicsJobs) {
#else
          job_system_(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, worker_thread_count()) {
#endif
        physics_system_.Init(maximum_bodies, 0, maximum_body_pairs, maximum_contact_constraints,
                             broad_phase_layers_, object_vs_broad_phase_filter_,
                             object_layer_pair_filter_);
        auto physics_settings = physics_system_.GetPhysicsSettings();
        physics_settings.mTimeBeforeSleep = 3.0F / 60.0F;
        physics_settings.mPointVelocitySleepThreshold = 0.02F;
        physics_settings.mPenetrationSlop = 0.005F;
        physics_system_.SetPhysicsSettings(physics_settings);
        physics_system_.SetGravity(to_jolt(desc.gravity));
        physics_system_.SetContactListener(&contact_listener_);
    }

    ~JoltPhysicsWorld() override {
        physics_system_.SetContactListener(nullptr);
        auto& bodies = physics_system_.GetBodyInterface();
        for (const auto& [_, record] : records_) {
            if (bodies.IsAdded(record.jolt_id)) {
                bodies.RemoveBody(record.jolt_id);
            }
            bodies.DestroyBody(record.jolt_id);
        }
    }

    JoltPhysicsWorld(const JoltPhysicsWorld&) = delete;
    JoltPhysicsWorld& operator=(const JoltPhysicsWorld&) = delete;

    [[nodiscard]] PhysicsBackend backend() const noexcept override {
        return PhysicsBackend::jolt;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return physics_backend_name(PhysicsBackend::jolt);
    }

    [[nodiscard]] PhysicsBackendCapabilities capabilities() const noexcept override {
        return physics_backend_capabilities(PhysicsBackend::jolt);
    }

    [[nodiscard]] std::uint32_t body_count() const noexcept override {
        return static_cast<std::uint32_t>(records_.size());
    }

    [[nodiscard]] core::Result<PhysicsBodyId> create_body(PhysicsBodyDesc desc) override {
        auto status = validate_physics_body_desc(desc);
        if (!status) {
            return core::Result<PhysicsBodyId>::failure(status.error().code,
                                                        status.error().message);
        }

        auto shape = make_shape(desc.shape);
        if (!shape) {
            return core::Result<PhysicsBodyId>::failure(shape.error().code, shape.error().message);
        }

        const auto id = PhysicsBodyId::from_value(next_body_id_);
        JPH::BodyCreationSettings settings(shape.value().GetPtr(), to_jolt_position(desc.position),
                                           JPH::Quat::sIdentity(), to_jolt(desc.motion_type),
                                           layer_for(desc.motion_type));
        settings.mLinearVelocity = to_jolt(desc.linear_velocity);
        settings.mUserData = id.value();
        settings.mGravityFactor = desc.gravity_scale;
        settings.mAllowSleeping = desc.allow_sleep;
        settings.mLinearDamping = 0.0F;
        settings.mAngularDamping = 0.0F;
        settings.mFriction = 0.0F;
        settings.mRestitution = 0.0F;
        settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY |
                                JPH::EAllowedDOFs::TranslationZ;
        if (desc.motion_type == BodyMotionType::dynamic) {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = desc.mass;
        }

        auto& bodies = physics_system_.GetBodyInterface();
        auto* body = bodies.CreateBody(settings);
        if (body == nullptr) {
            return core::Result<PhysicsBodyId>::failure(
                "physics.jolt_body_capacity_exhausted",
                "Jolt could not allocate another physics body");
        }
        const auto jolt_id = body->GetID();
        bodies.AddBody(jolt_id, activation_for(desc.motion_type));

        records_.emplace(id.value(),
                         BodyRecord{jolt_id, desc.motion_type, desc.mass, desc.user_data});
        ++next_body_id_;
        return core::Result<PhysicsBodyId>::success(id);
    }

    [[nodiscard]] core::Status destroy_body(PhysicsBodyId id) override {
        const auto found = records_.find(id.value());
        if (!id.is_valid() || found == records_.end()) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }

        auto& bodies = physics_system_.GetBodyInterface();
        if (bodies.IsAdded(found->second.jolt_id)) {
            bodies.RemoveBody(found->second.jolt_id);
        }
        bodies.DestroyBody(found->second.jolt_id);
        records_.erase(found);
        std::erase_if(contacts_, [id](const PhysicsContact& contact) {
            return contact.first == id || contact.second == id;
        });
        return core::Status::ok();
    }

    [[nodiscard]] std::optional<PhysicsBodyState>
    body_state(PhysicsBodyId id) const noexcept override {
        const auto found = records_.find(id.value());
        if (found == records_.end()) {
            return std::nullopt;
        }

        const auto& record = found->second;
        const auto& bodies = physics_system_.GetBodyInterface();
        PhysicsBodyState state;
        state.id = id;
        state.motion_type = record.motion_type;
        state.position = from_jolt_position(bodies.GetPosition(record.jolt_id));
        state.linear_velocity = record.motion_type == BodyMotionType::static_body
                                    ? Vec3{}
                                    : from_jolt(bodies.GetLinearVelocity(record.jolt_id));
        state.mass = record.mass;
        state.sleeping =
            record.motion_type == BodyMotionType::dynamic && !bodies.IsActive(record.jolt_id);
        state.user_data = record.user_data;
        return state;
    }

    [[nodiscard]] core::Status set_body_position(PhysicsBodyId id, Vec3 position) override {
        if (!position.is_finite()) {
            return core::Status::failure("physics.invalid_position",
                                         "body position must be finite");
        }
        auto* record = find_record(id);
        if (record == nullptr) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }
        physics_system_.GetBodyInterface().SetPosition(record->jolt_id, to_jolt_position(position),
                                                       activation_for(record->motion_type));
        return core::Status::ok();
    }

    [[nodiscard]] core::Status set_linear_velocity(PhysicsBodyId id, Vec3 velocity) override {
        if (!velocity.is_finite()) {
            return core::Status::failure("physics.invalid_velocity",
                                         "body linear velocity must be finite");
        }
        auto* record = find_record(id);
        if (record == nullptr) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }
        if (record->motion_type == BodyMotionType::static_body) {
            return core::Status::failure("physics.static_body_velocity",
                                         "static bodies cannot receive linear velocity");
        }
        physics_system_.GetBodyInterface().SetLinearVelocity(record->jolt_id, to_jolt(velocity));
        return core::Status::ok();
    }

    [[nodiscard]] core::Status apply_impulse(PhysicsBodyId id, Vec3 impulse) override {
        if (!impulse.is_finite()) {
            return core::Status::failure("physics.invalid_impulse", "impulse must be finite");
        }
        auto* record = find_record(id);
        if (record == nullptr) {
            return core::Status::failure("physics.body_not_found", "physics body was not found");
        }
        if (record->motion_type != BodyMotionType::dynamic) {
            return core::Status::failure("physics.impulse_requires_dynamic_body",
                                         "only dynamic bodies can receive impulses");
        }
        physics_system_.GetBodyInterface().AddImpulse(record->jolt_id, to_jolt(impulse));
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<std::vector<PhysicsOverlap>>
    query_aabb(PhysicsAabb bounds) const override {
        auto status = validate_physics_aabb(bounds);
        if (!status) {
            return core::Result<std::vector<PhysicsOverlap>>::failure(status.error().code,
                                                                      status.error().message);
        }

        JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
        physics_system_.GetBroadPhaseQuery().CollideAABox(
            JPH::AABox(to_jolt(bounds.min), to_jolt(bounds.max)), collector);

        std::vector<PhysicsOverlap> overlaps;
        overlaps.reserve(collector.mHits.size());
        const auto& bodies = physics_system_.GetBodyInterface();
        for (const auto& jolt_id : collector.mHits) {
            const auto id = PhysicsBodyId::from_value(bodies.GetUserData(jolt_id));
            const auto found = records_.find(id.value());
            if (found == records_.end()) {
                continue;
            }
            const auto shape = bodies.GetTransformedShape(jolt_id);
            overlaps.push_back(PhysicsOverlap{id, found->second.user_data,
                                              from_jolt(shape.GetWorldSpaceBounds())});
        }
        std::ranges::sort(overlaps, {},
                          [](const PhysicsOverlap& overlap) { return overlap.body.value(); });
        return core::Result<std::vector<PhysicsOverlap>>::success(std::move(overlaps));
    }

    [[nodiscard]] std::vector<PhysicsContact> drain_contacts() override {
        auto contacts = std::move(contacts_);
        contacts_.clear();
        return contacts;
    }

    [[nodiscard]] core::Result<PhysicsStepStats> step(PhysicsStepDesc desc) override {
        if (!std::isfinite(desc.delta_seconds) || desc.delta_seconds <= 0.0F) {
            return core::Result<PhysicsStepStats>::failure(
                "physics.invalid_timestep", "physics timestep must be positive and finite");
        }

        PhysicsStepStats stats;
        stats.body_count = body_count();
        stats.simulated_seconds = desc.delta_seconds;

        auto& bodies = physics_system_.GetBodyInterface();
        for (const auto& [_, record] : records_) {
            if (record.motion_type == BodyMotionType::dynamic) {
                ++stats.dynamic_body_count;
                if (bodies.IsActive(record.jolt_id)) {
                    ++stats.integrated_body_count;
                }
            } else if (record.motion_type == BodyMotionType::kinematic) {
                const auto position = bodies.GetPosition(record.jolt_id);
                const auto velocity = bodies.GetLinearVelocity(record.jolt_id);
                bodies.MoveKinematic(record.jolt_id,
                                     position + JPH::RVec3(velocity * desc.delta_seconds),
                                     JPH::Quat::sIdentity(), desc.delta_seconds);
                ++stats.integrated_body_count;
            }
        }

        {
            std::scoped_lock lock(contact_mutex_);
            callback_contacts_.clear();
        }

        const auto bounded_step = std::min(desc.delta_seconds, 8.0F / 60.0F);
        const auto collision_steps = std::max(1, static_cast<int>(std::ceil(bounded_step * 60.0F)));
        const auto update_error = physics_system_.Update(desc.delta_seconds, collision_steps,
                                                         &temporary_allocator_, &job_system_);
        if (update_error != JPH::EPhysicsUpdateError::None) {
            return core::Result<PhysicsStepStats>::failure(
                "physics.jolt_step_capacity_exhausted",
                "Jolt exhausted a body-pair, manifold, or contact-constraint buffer");
        }

        {
            std::scoped_lock lock(contact_mutex_);
            contacts_ = callback_contacts_;
        }
        normalize_contacts();
        stats.contact_count = static_cast<std::uint32_t>(contacts_.size());
        return core::Result<PhysicsStepStats>::success(stats);
    }

  private:
    struct BodyRecord {
        JPH::BodyID jolt_id;
        BodyMotionType motion_type = BodyMotionType::static_body;
        float mass = 0.0F;
        std::uint64_t user_data = 0;
    };

    class ContactListener final : public JPH::ContactListener {
      public:
        explicit ContactListener(JoltPhysicsWorld& owner) noexcept : owner_(&owner) {}

        void OnContactAdded(const JPH::Body& first, const JPH::Body& second,
                            const JPH::ContactManifold& manifold, JPH::ContactSettings&) override {
            owner_->record_contact(first, second, manifold);
        }

        void OnContactPersisted(const JPH::Body& first, const JPH::Body& second,
                                const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override {
            owner_->record_contact(first, second, manifold);
        }

      private:
        JoltPhysicsWorld* owner_ = nullptr;
    };

#if !HEARTSTEAD_JOLT_SINGLE_THREADED
    [[nodiscard]] static int worker_thread_count() noexcept {
        const auto threads = std::thread::hardware_concurrency();
        return threads > 1 ? static_cast<int>(std::min(threads - 1, 2u)) : 0;
    }
#endif

    [[nodiscard]] BodyRecord* find_record(PhysicsBodyId id) noexcept {
        const auto found = records_.find(id.value());
        return found == records_.end() ? nullptr : &found->second;
    }

    void record_contact(const JPH::Body& first_body, const JPH::Body& second_body,
                        const JPH::ContactManifold& manifold) {
        auto first = PhysicsBodyId::from_value(first_body.GetUserData());
        auto second = PhysicsBodyId::from_value(second_body.GetUserData());
        auto first_found = records_.find(first.value());
        auto second_found = records_.find(second.value());
        if (!first.is_valid() || !second.is_valid() || first_found == records_.end() ||
            second_found == records_.end()) {
            return;
        }

        auto first_bounds = from_jolt(first_body.GetWorldSpaceBounds());
        auto second_bounds = from_jolt(second_body.GetWorldSpaceBounds());
        auto normal = from_jolt(manifold.mWorldSpaceNormal);
        auto first_user_data = first_found->second.user_data;
        auto second_user_data = second_found->second.user_data;
        if (second < first) {
            std::swap(first, second);
            std::swap(first_bounds, second_bounds);
            std::swap(first_user_data, second_user_data);
            normal = normal * -1.0F;
        }

        PhysicsContact contact{
            first,           second,
            first_user_data, second_user_data,
            first_bounds,    second_bounds,
            normal,          std::max(0.0F, manifold.mPenetrationDepth),
        };
        std::scoped_lock lock(contact_mutex_);
        callback_contacts_.push_back(contact);
    }

    void normalize_contacts() {
        std::ranges::sort(contacts_, [](const PhysicsContact& lhs, const PhysicsContact& rhs) {
            if (lhs.first != rhs.first) {
                return lhs.first < rhs.first;
            }
            if (lhs.second != rhs.second) {
                return lhs.second < rhs.second;
            }
            return lhs.penetration_depth > rhs.penetration_depth;
        });
        const auto unique_end =
            std::unique(contacts_.begin(), contacts_.end(),
                        [](const PhysicsContact& lhs, const PhysicsContact& rhs) {
                            return lhs.first == rhs.first && lhs.second == rhs.second;
                        });
        contacts_.erase(unique_end, contacts_.end());
    }

    PhysicsWorldDesc desc_;
    std::shared_ptr<RuntimeRegistration> runtime_;
    BroadPhaseLayerInterface broad_phase_layers_;
    ObjectVsBroadPhaseLayerFilter object_vs_broad_phase_filter_;
    ObjectLayerPairFilter object_layer_pair_filter_;
    ContactListener contact_listener_;
    JPH::PhysicsSystem physics_system_;
    JPH::TempAllocatorImpl temporary_allocator_;
    JoltJobSystem job_system_;
    std::uint64_t next_body_id_ = 1;
    std::map<std::uint64_t, BodyRecord> records_;
    std::mutex contact_mutex_;
    std::vector<PhysicsContact> callback_contacts_;
    std::vector<PhysicsContact> contacts_;
};

} // namespace

#endif

PhysicsBackendInfo backend_info() noexcept {
#if HEARTSTEAD_HAS_JOLT
    return PhysicsBackendInfo{
        PhysicsBackend::jolt,
        physics_backend_name(PhysicsBackend::jolt),
        true,
        "available (Jolt Physics 5.6.0)",
    };
#else
    return PhysicsBackendInfo{
        PhysicsBackend::jolt,
        physics_backend_name(PhysicsBackend::jolt),
        false,
        "jolt backend was disabled at configure time",
    };
#endif
}

core::Result<std::unique_ptr<IPhysicsWorld>> create_world(PhysicsWorldDesc desc) {
#if HEARTSTEAD_HAS_JOLT
    return core::Result<std::unique_ptr<IPhysicsWorld>>::success(
        std::make_unique<JoltPhysicsWorld>(desc));
#else
    (void)desc;
    return core::Result<std::unique_ptr<IPhysicsWorld>>::failure(
        "physics.jolt_unavailable", "jolt backend was disabled at configure time");
#endif
}

} // namespace heartstead::physics::jolt
