#include "engine/cargo/cargo.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/physics/physical_resource_physics_system.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/world/world_state.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>

using namespace heartstead;

namespace {

entities::PhysicalResourceRecord make_resource(std::uint64_t id, world::WorldPosition position) {
    entities::PhysicalResourceRecord resource;
    resource.resource_id = core::SaveId::from_value(id);
    resource.prototype_id = *core::PrototypeId::parse("base:entities/dropped_log");
    resource.cargo_prototype_id = *core::PrototypeId::parse("base:cargo/heavy_log");
    resource.position = position;
    resource.kind = entities::PhysicalResourceKind::haulable_log;
    resource.mass_grams = 12'000;
    resource.volume_milliliters = 24'000;
    resource.allowed_transport_modes = cargo::CargoTransportModes::of(
        {cargo::CargoTransportMode::hand, cargo::CargoTransportMode::cart});
    resource.segments.push_back({physics::ShapeKind::box, {}, {0.6F, 0.2F, 0.2F}, 0.5F, 0.5F});
    assert(resource.validate());
    return resource;
}

physics::PhysicsBodyId add_ground(physics::IPhysicsWorld& physics_world) {
    physics::PhysicsBodyDesc ground;
    ground.motion_type = physics::BodyMotionType::static_body;
    ground.position = {0.0F, -0.5F, 0.0F};
    ground.shape.half_extents = {8.0F, 0.5F, 8.0F};
    auto body = physics_world.create_body(ground);
    assert(body);
    return body.value();
}

void step_resources(physics::PhysicalResourcePhysicsSystem& resources,
                    physics::IPhysicsWorld& physics_world, world::WorldState& world,
                    std::uint32_t count) {
    for (std::uint32_t index = 0; index < count; ++index) {
        assert(resources.prepare(world));
        assert(physics_world.step({1.0F / 60.0F}));
        assert(resources.synchronize(world));
    }
}

void test_invalid_config_is_rejected() {
    auto physics_world = physics::create_physics_world({});
    assert(physics_world);
    physics::PhysicalResourcePhysicsSystemConfig invalid;
    invalid.physics_island.max_local_extent = 0.0F;
    auto system = physics::PhysicalResourcePhysicsSystem::create(*physics_world.value(), invalid);
    assert(!system);
    assert(system.error().code == "physical_resource_physics.invalid_config");
}

void test_headless_drop_settles_and_freezes() {
    auto physics_world = physics::create_physics_world({});
    assert(physics_world);
    const auto ground = add_ground(*physics_world.value());
    physics::PhysicalResourcePhysicsSystemConfig config;
    config.freeze_after_sleeping_ticks = 2;
    auto resources = physics::PhysicalResourcePhysicsSystem::create(*physics_world.value(), config);
    assert(resources);
    world::WorldState world;

    auto resource = make_resource(20, {0.0, 3.0, 0.0});
    assert(resources.value()->activate(resource));
    const auto dynamic_body = resource.physics_body_id;
    assert(world.physical_resources().insert(std::move(resource)));

    step_resources(*resources.value(), *physics_world.value(), world, 180);

    const auto* settled = world.physical_resources().find(core::SaveId::from_value(20));
    assert(settled != nullptr);
    assert(settled->state == entities::PhysicalResourceState::frozen_static);
    assert(settled->physics_body_id.is_valid());
    assert(settled->physics_body_id != dynamic_body);
    assert(std::abs(settled->position.approximate_global().y - 0.2) < 0.02);
    assert((settled->linear_velocity == physics::Vec3{}));
    assert((settled->angular_velocity == physics::Vec3{}));
    const auto body = physics_world.value()->body_state(settled->physics_body_id);
    assert(body.has_value());
    assert(body->motion_type == physics::BodyMotionType::static_body);
    assert(body->user_data == 20);
    assert(resources.value()->stats().settled_bodies == 1);
    assert(resources.value()->stats().frozen_bodies == 1);
    assert(resources.value()->stats().active_body_count == 1);
    assert(physics_world.value()->body_state(ground).has_value());
}

void test_saved_resources_rebuild_in_stable_id_order() {
    auto physics_world = physics::create_physics_world({});
    assert(physics_world);
    auto resources = physics::PhysicalResourcePhysicsSystem::create(*physics_world.value(), {});
    assert(resources);
    world::WorldState world;

    auto later = make_resource(200, {2.0, 1.0, 0.0});
    later.state = entities::PhysicalResourceState::settled_sleeping;
    later.needs_physics_rebuild = true;
    later.rotation_degrees = {10.0F, 20.0F, 30.0F};
    later.linear_velocity = {1.0F, 2.0F, 3.0F};
    later.angular_velocity = {0.1F, 0.2F, 0.3F};
    assert(later.validate());

    const auto encoded = entities::PhysicalResourceTextCodec::encode(later);
    auto decoded = entities::PhysicalResourceTextCodec::decode(
        later.resource_id, later.prototype_id, later.position, encoded);
    assert(decoded);
    assert(decoded.value().rotation_degrees == later.rotation_degrees);
    assert(decoded.value().linear_velocity == later.linear_velocity);
    assert(decoded.value().angular_velocity == later.angular_velocity);
    assert(decoded.value().needs_physics_rebuild);

    auto earlier = make_resource(100, {-2.0, 1.0, 0.0});
    earlier.state = entities::PhysicalResourceState::frozen_static;
    earlier.needs_physics_rebuild = true;
    assert(earlier.validate());
    assert(world.physical_resources().insert(std::move(later)));
    assert(world.physical_resources().insert(std::move(earlier)));

    assert(resources.value()->prepare(world));
    const auto* first = world.physical_resources().find(core::SaveId::from_value(100));
    const auto* second = world.physical_resources().find(core::SaveId::from_value(200));
    assert(first != nullptr && second != nullptr);
    assert(first->physics_body_id.value() < second->physics_body_id.value());
    assert(first->state == entities::PhysicalResourceState::frozen_static);
    assert(second->state == entities::PhysicalResourceState::settled_sleeping);
    assert(resources.value()->stats().restored_this_tick == 2);
}

void test_jolt_compound_resource_settles() {
    if (!physics::physics_backend_info(physics::PhysicsBackend::jolt).available) {
        return;
    }
    physics::PhysicsWorldDesc world_desc;
    world_desc.backend = physics::PhysicsBackend::jolt;
    auto physics_world = physics::create_physics_world(world_desc);
    assert(physics_world);
    static_cast<void>(add_ground(*physics_world.value()));
    physics::PhysicalResourcePhysicsSystemConfig config;
    config.freeze_after_sleeping_ticks = 5;
    auto resources = physics::PhysicalResourcePhysicsSystem::create(*physics_world.value(), config);
    assert(resources);
    world::WorldState world;

    auto resource = make_resource(300, {0.0, 3.0, 0.0});
    resource.segments.push_back(
        {physics::ShapeKind::box, {0.45F, 0.25F, 0.0F}, {0.2F, 0.15F, 0.15F}, 0.5F, 0.5F});
    resource.rotation_degrees = {0.0F, 0.0F, 12.0F};
    assert(resources.value()->activate(resource, {0.4F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.6F}));
    assert(world.physical_resources().insert(std::move(resource)));

    step_resources(*resources.value(), *physics_world.value(), world, 600);

    const auto* settled = world.physical_resources().find(core::SaveId::from_value(300));
    assert(settled != nullptr);
    assert(settled->state == entities::PhysicalResourceState::frozen_static);
    assert(settled->position.approximate_global().y > 0.1);
    assert(settled->position.approximate_global().y < 0.9);
    assert(std::abs(settled->rotation_degrees.z - 12.0F) > 0.1F);
}

} // namespace

int main() {
    test_invalid_config_is_rejected();
    test_headless_drop_settles_and_freezes();
    test_saved_resources_rebuild_in_stable_id_order();
    test_jolt_compound_resource_settles();
    return 0;
}
