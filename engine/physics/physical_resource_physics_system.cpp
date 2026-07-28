#include "engine/physics/physical_resource_physics_system.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace heartstead::physics {

namespace {

[[nodiscard]] std::vector<core::SaveId>
sorted_resource_ids(const world::PhysicalResourceDatabase& resources) {
    std::vector<core::SaveId> ids;
    ids.reserve(resources.count());
    for (const auto* resource : resources.records()) {
        ids.push_back(resource->resource_id);
    }
    std::ranges::sort(ids, {}, [](core::SaveId id) { return id.value(); });
    return ids;
}

[[nodiscard]] bool body_state_requires_physics(entities::PhysicalResourceState state) noexcept {
    return state == entities::PhysicalResourceState::dynamic ||
           state == entities::PhysicalResourceState::settled_sleeping ||
           state == entities::PhysicalResourceState::frozen_static;
}

} // namespace

core::Status PhysicalResourcePhysicsSystemConfig::validate() const {
    if (!std::isfinite(physics_island.max_local_extent) ||
        physics_island.max_local_extent <= 0.0F) {
        return core::Status::failure(
            "physical_resource_physics.invalid_config",
            "physical resource physics requires a positive finite island extent");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<PhysicalResourcePhysicsSystem>>
PhysicalResourcePhysicsSystem::create(IPhysicsWorld& physics_world,
                                      PhysicalResourcePhysicsSystemConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<PhysicalResourcePhysicsSystem>>::failure(
            status.error().code, status.error().message);
    }
    return core::Result<std::unique_ptr<PhysicalResourcePhysicsSystem>>::success(
        std::unique_ptr<PhysicalResourcePhysicsSystem>(
            new PhysicalResourcePhysicsSystem(physics_world, config)));
}

PhysicalResourcePhysicsSystem::PhysicalResourcePhysicsSystem(
    IPhysicsWorld& physics_world, PhysicalResourcePhysicsSystemConfig config)
    : physics_world_(&physics_world), config_(config) {}

PhysicalResourcePhysicsSystem::~PhysicalResourcePhysicsSystem() {
    shutdown();
}

core::Status PhysicalResourcePhysicsSystem::activate(entities::PhysicalResourceRecord& resource,
                                                     Vec3 linear_velocity, Vec3 angular_velocity) {
    if (physics_world_ == nullptr) {
        return core::Status::failure("physical_resource_physics.shutdown",
                                     "physical resource physics system is shut down");
    }
    if (resource.state != entities::PhysicalResourceState::cutting ||
        resource.physics_body_id.is_valid() || resource.needs_physics_rebuild) {
        return core::Status::failure(
            "physical_resource_physics.not_activatable",
            "only an unattached cutting resource can enter dynamic simulation");
    }
    if (!linear_velocity.is_finite() || !angular_velocity.is_finite()) {
        return core::Status::failure("physical_resource_physics.invalid_velocity",
                                     "physical resource activation velocities must be finite");
    }
    resource.linear_velocity = linear_velocity;
    resource.angular_velocity = angular_velocity;
    return create_attached_body(resource, false);
}

core::Status PhysicalResourcePhysicsSystem::prepare(world::WorldState& world) {
    if (physics_world_ == nullptr) {
        return core::Status::failure("physical_resource_physics.shutdown",
                                     "physical resource physics system is shut down");
    }
    reset_tick_stats();
    auto& resources = world.physical_resources();
    const auto ids = sorted_resource_ids(resources);

    for (const auto id : ids) {
        auto* resource = resources.find(id);
        if (resource == nullptr) {
            continue;
        }
        if (!body_state_requires_physics(resource->state)) {
            sleeping_ticks_.erase(id.value());
            const auto owned = owned_bodies_.find(id.value());
            if (owned != owned_bodies_.end()) {
                if (physics_world_->body_state(owned->second).has_value()) {
                    auto status = physics_world_->destroy_body(owned->second);
                    if (!status) {
                        return status;
                    }
                }
                owned_bodies_.erase(owned);
            }
            continue;
        }

        if (resource->physics_body_id.is_valid() &&
            !physics_world_->body_state(resource->physics_body_id).has_value()) {
            resource->physics_body_id = {};
            resource->needs_physics_rebuild = true;
        }
        if (resource->needs_physics_rebuild) {
            auto status = create_attached_body(*resource, true);
            if (!status) {
                return status;
            }
        } else if (resource->physics_body_id.is_valid()) {
            owned_bodies_[id.value()] = resource->physics_body_id;
        }
    }

    for (auto owned = owned_bodies_.begin(); owned != owned_bodies_.end();) {
        const auto id = core::SaveId::from_value(owned->first);
        const auto* resource = resources.find(id);
        if (resource != nullptr && resource->physics_body_id == owned->second &&
            body_state_requires_physics(resource->state)) {
            ++owned;
            continue;
        }
        if (physics_world_->body_state(owned->second).has_value()) {
            auto status = physics_world_->destroy_body(owned->second);
            if (!status) {
                return status;
            }
        }
        sleeping_ticks_.erase(owned->first);
        owned = owned_bodies_.erase(owned);
    }
    refresh_counts(resources);
    return core::Status::ok();
}

core::Status PhysicalResourcePhysicsSystem::synchronize(world::WorldState& world) {
    if (physics_world_ == nullptr) {
        return core::Status::failure("physical_resource_physics.shutdown",
                                     "physical resource physics system is shut down");
    }
    auto& resources = world.physical_resources();
    const auto ids = sorted_resource_ids(resources);
    for (const auto id : ids) {
        auto* resource = resources.find(id);
        if (resource == nullptr || !resource->physics_body_id.is_valid()) {
            continue;
        }
        const auto body = physics_world_->body_state(resource->physics_body_id);
        if (!body.has_value()) {
            resource->physics_body_id = {};
            resource->needs_physics_rebuild = body_state_requires_physics(resource->state);
            return core::Status::failure(
                "physical_resource_physics.body_missing",
                "physical resource body disappeared during authoritative synchronization");
        }
        if (body->user_data != resource->resource_id.value()) {
            return core::Status::failure(
                "physical_resource_physics.identity_mismatch",
                "physical resource body user data does not match its stable save id");
        }
        auto position = world::from_physics_local(body->position, config_.physics_island);
        if (!position) {
            return core::Status::failure(position.error().code, position.error().message);
        }
        resource->position = position.value();
        resource->rotation_degrees = body->rotation_degrees;
        resource->linear_velocity = body->linear_velocity;
        resource->angular_velocity = body->angular_velocity;
        ++stats_.synchronized_this_tick;
        ++stats_.synchronized_bodies;

        if (body->motion_type != BodyMotionType::dynamic) {
            sleeping_ticks_.erase(id.value());
            continue;
        }
        if (!body->sleeping) {
            sleeping_ticks_.erase(id.value());
            if (resource->state == entities::PhysicalResourceState::settled_sleeping) {
                resource->state = entities::PhysicalResourceState::dynamic;
                ++stats_.woken_this_tick;
                ++stats_.woken_bodies;
            }
            continue;
        }

        auto& sleeping_ticks = sleeping_ticks_[id.value()];
        ++sleeping_ticks;
        if (resource->state == entities::PhysicalResourceState::dynamic) {
            auto status = entities::mark_physical_resource_settled(*resource);
            if (!status) {
                return status;
            }
            ++stats_.settled_this_tick;
            ++stats_.settled_bodies;
        }
        if (resource->state == entities::PhysicalResourceState::settled_sleeping &&
            config_.freeze_after_sleeping_ticks > 0 &&
            sleeping_ticks >= config_.freeze_after_sleeping_ticks) {
            auto status = replace_with_static_body(*resource);
            if (!status) {
                return status;
            }
            sleeping_ticks_.erase(id.value());
            ++stats_.frozen_this_tick;
            ++stats_.frozen_bodies;
        }
    }
    refresh_counts(resources);
    return core::Status::ok();
}

void PhysicalResourcePhysicsSystem::shutdown() noexcept {
    if (physics_world_ != nullptr) {
        for (const auto& [_, body_id] : owned_bodies_) {
            if (physics_world_->body_state(body_id).has_value()) {
                (void)physics_world_->destroy_body(body_id);
            }
        }
    }
    owned_bodies_.clear();
    sleeping_ticks_.clear();
    stats_ = {};
    physics_world_ = nullptr;
}

const PhysicalResourcePhysicsSystemStats& PhysicalResourcePhysicsSystem::stats() const noexcept {
    return stats_;
}

core::Status
PhysicalResourcePhysicsSystem::create_attached_body(entities::PhysicalResourceRecord& resource,
                                                    bool restored) {
    auto local_position = world::to_physics_local(resource.position, config_.physics_island);
    if (!local_position) {
        return core::Status::failure(local_position.error().code, local_position.error().message);
    }
    auto desc = entities::make_physical_resource_body_desc(
        resource, local_position.value(), resource.linear_velocity, resource.rotation_degrees,
        resource.angular_velocity);
    if (!desc) {
        return core::Status::failure(desc.error().code, desc.error().message);
    }
    auto body = physics_world_->create_body(std::move(desc).value());
    if (!body) {
        return core::Status::failure(body.error().code, body.error().message);
    }
    auto status = entities::attach_physical_resource_body(resource, body.value());
    if (!status) {
        (void)physics_world_->destroy_body(body.value());
        return status;
    }
    owned_bodies_[resource.resource_id.value()] = body.value();
    ++stats_.created_this_tick;
    ++stats_.created_bodies;
    if (restored) {
        ++stats_.restored_this_tick;
        ++stats_.restored_bodies;
    }
    return core::Status::ok();
}

core::Status PhysicalResourcePhysicsSystem::replace_with_static_body(
    entities::PhysicalResourceRecord& resource) {
    auto status = entities::freeze_physical_resource(resource);
    if (!status) {
        return status;
    }
    const auto dynamic_body = resource.physics_body_id;
    status = physics_world_->destroy_body(dynamic_body);
    if (!status) {
        return status;
    }
    owned_bodies_.erase(resource.resource_id.value());
    resource.physics_body_id = {};
    resource.needs_physics_rebuild = true;
    return create_attached_body(resource, false);
}

void PhysicalResourcePhysicsSystem::reset_tick_stats() noexcept {
    stats_.created_this_tick = 0;
    stats_.restored_this_tick = 0;
    stats_.synchronized_this_tick = 0;
    stats_.settled_this_tick = 0;
    stats_.woken_this_tick = 0;
    stats_.frozen_this_tick = 0;
}

void PhysicalResourcePhysicsSystem::refresh_counts(
    const world::PhysicalResourceDatabase& resources) noexcept {
    stats_.active_body_count = 0;
    stats_.dynamic_body_count = 0;
    stats_.sleeping_body_count = 0;
    stats_.frozen_body_count = 0;
    for (const auto* resource : resources.records()) {
        if (!resource->physics_body_id.is_valid()) {
            continue;
        }
        ++stats_.active_body_count;
        if (resource->state == entities::PhysicalResourceState::frozen_static) {
            ++stats_.frozen_body_count;
            continue;
        }
        ++stats_.dynamic_body_count;
        if (resource->state == entities::PhysicalResourceState::settled_sleeping) {
            ++stats_.sleeping_body_count;
        }
    }
}

} // namespace heartstead::physics
