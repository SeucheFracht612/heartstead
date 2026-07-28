#include "engine/core/logging.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/physics/physics_world.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct PhysicsTraceSample {
    heartstead::physics::Vec3 position{};
    heartstead::physics::Vec3 velocity{};
    bool sleeping = false;
    std::uint32_t contact_count = 0;

    friend bool operator==(const PhysicsTraceSample&, const PhysicsTraceSample&) = default;
};

struct PhysicsTrace {
    std::vector<PhysicsTraceSample> samples;
    bool observed_contact = false;
};

[[nodiscard]] heartstead::core::Result<PhysicsTrace>
run_trace(heartstead::physics::PhysicsBackend backend) {
    using namespace heartstead;

    physics::PhysicsWorldDesc world_desc;
    world_desc.backend = backend;
    world_desc.gravity = physics::Vec3{0.0F, -9.81F, 0.0F};
    auto world = physics::create_physics_world(world_desc);
    if (!world) {
        return core::Result<PhysicsTrace>::failure(world.error().code, world.error().message);
    }

    physics::PhysicsBodyDesc ground;
    ground.motion_type = physics::BodyMotionType::static_body;
    ground.shape.kind = physics::ShapeKind::box;
    ground.shape.half_extents = physics::Vec3{20.0F, 0.5F, 20.0F};
    ground.position = physics::Vec3{0.0F, -0.5F, 0.0F};
    auto ground_body = world.value()->create_body(ground);
    if (!ground_body) {
        return core::Result<PhysicsTrace>::failure(ground_body.error().code,
                                                   ground_body.error().message);
    }

    physics::PhysicsBodyDesc falling;
    falling.motion_type = physics::BodyMotionType::dynamic;
    falling.shape.kind = physics::ShapeKind::box;
    falling.shape.half_extents = physics::Vec3{0.5F, 0.5F, 0.5F};
    falling.position = physics::Vec3{0.0F, 3.0F, 0.0F};
    falling.mass = 2.0F;
    auto falling_body = world.value()->create_body(falling);
    if (!falling_body) {
        return core::Result<PhysicsTrace>::failure(falling_body.error().code,
                                                   falling_body.error().message);
    }

    PhysicsTrace trace;
    trace.samples.reserve(180);
    for (std::uint32_t step_index = 0; step_index < 180; ++step_index) {
        auto stats = world.value()->step({1.0F / 60.0F});
        if (!stats) {
            return core::Result<PhysicsTrace>::failure(stats.error().code, stats.error().message);
        }
        const auto contacts = world.value()->drain_contacts();
        const auto state = world.value()->body_state(falling_body.value());
        if (!state) {
            return core::Result<PhysicsTrace>::failure("physics_sandbox.body_disappeared",
                                                       "trace body disappeared");
        }
        trace.observed_contact =
            trace.observed_contact || stats.value().contact_count > 0 || !contacts.empty();
        trace.samples.push_back({state->position, state->linear_velocity, state->sleeping,
                                 stats.value().contact_count});
    }
    return core::Result<PhysicsTrace>::success(std::move(trace));
}

[[nodiscard]] heartstead::core::Status verify_backend_traces() {
    using namespace heartstead;

    auto headless_first = run_trace(physics::PhysicsBackend::headless);
    auto headless_second = run_trace(physics::PhysicsBackend::headless);
    auto jolt_first = run_trace(physics::PhysicsBackend::jolt);
    auto jolt_second = run_trace(physics::PhysicsBackend::jolt);
    if (!headless_first || !headless_second || !jolt_first || !jolt_second) {
        const auto* error = !headless_first    ? &headless_first.error()
                            : !headless_second ? &headless_second.error()
                            : !jolt_first      ? &jolt_first.error()
                                               : &jolt_second.error();
        return core::Status::failure(error->code, error->message);
    }
    if (headless_first.value().samples != headless_second.value().samples ||
        jolt_first.value().samples != jolt_second.value().samples) {
        return core::Status::failure(
            "physics_sandbox.nondeterministic_trace",
            "a physics backend produced different results for two identical runs");
    }

    const auto& headless_final = headless_first.value().samples.back();
    const auto& jolt_final = jolt_first.value().samples.back();
    const auto position_error = jolt_final.position - headless_final.position;
    const auto velocity_error = jolt_final.velocity - headless_final.velocity;
    constexpr float position_tolerance = 0.02F;
    constexpr float velocity_tolerance = 0.05F;
    if (std::abs(position_error.x) > position_tolerance ||
        std::abs(position_error.y) > position_tolerance ||
        std::abs(position_error.z) > position_tolerance ||
        std::abs(velocity_error.x) > velocity_tolerance ||
        std::abs(velocity_error.y) > velocity_tolerance ||
        std::abs(velocity_error.z) > velocity_tolerance ||
        headless_final.sleeping != jolt_final.sleeping ||
        !headless_first.value().observed_contact || !jolt_first.value().observed_contact) {
        return core::Status::failure("physics_sandbox.backend_trace_mismatch",
                                     "headless and Jolt traces exceeded the final-state tolerance");
    }
    return core::Status::ok();
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_no_argument_process_entry(argc, argv, [] {
        using namespace heartstead;

        core::log(core::LogLevel::info, "Heartstead physics sandbox starting");

        const auto jolt_info = physics::physics_backend_info(physics::PhysicsBackend::jolt);
        core::log(core::LogLevel::info, "Jolt backend status: " + std::string(jolt_info.status));
        const auto jolt_capabilities =
            physics::physics_backend_capabilities(physics::PhysicsBackend::jolt);
        core::log(core::LogLevel::info,
                  "Jolt contract: collision_response=" +
                      std::string(jolt_capabilities.supports_collision_response ? "yes" : "no") +
                      ", character_controllers=" +
                      std::string(jolt_capabilities.supports_character_controllers ? "yes" : "no") +
                      ", constraints=" +
                      std::string(jolt_capabilities.supports_constraints ? "yes" : "no"));

        if (jolt_info.available) {
            auto trace_status = verify_backend_traces();
            if (!trace_status) {
                core::log(core::LogLevel::error, trace_status.error().message);
                return 1;
            }
            core::log(core::LogLevel::info,
                      "Headless and Jolt repeated traces are deterministic and within tolerance");
        }

        physics::PhysicsWorldDesc world_desc;
        world_desc.backend =
            jolt_info.available ? physics::PhysicsBackend::jolt : physics::PhysicsBackend::headless;
        world_desc.gravity = physics::Vec3{0.0F, -9.81F, 0.0F};

        auto world = physics::create_physics_world(world_desc);
        if (!world) {
            core::log(core::LogLevel::error, world.error().message);
            return 1;
        }
        const auto selected_capabilities = world.value()->capabilities();
        core::log(core::LogLevel::info,
                  "Selected physics backend: " + std::string(world.value()->backend_name()) +
                      " | deterministic=" +
                      std::string(selected_capabilities.deterministic ? "yes" : "no") +
                      " | contacts=" +
                      std::string(selected_capabilities.supports_contacts ? "yes" : "no"));

        physics::PhysicsBodyDesc ground;
        ground.motion_type = physics::BodyMotionType::static_body;
        ground.shape.kind = physics::ShapeKind::box;
        ground.shape.half_extents = physics::Vec3{20.0F, 0.5F, 20.0F};
        ground.position = physics::Vec3{0.0F, -0.5F, 0.0F};
        ground.user_data = 100;
        auto ground_body = world.value()->create_body(ground);
        if (!ground_body) {
            core::log(core::LogLevel::error, ground_body.error().message);
            return 1;
        }

        auto tree_prototype = core::PrototypeId::parse("base:entities/felled_oak");
        auto log_cargo_prototype = core::PrototypeId::parse("base:cargo/heavy_log");
        if (!tree_prototype || !log_cargo_prototype) {
            core::log(core::LogLevel::error, "failed to parse physical resource prototype ids");
            return 1;
        }

        entities::PhysicalResourceRecord felled_tree;
        felled_tree.resource_id = core::SaveId::from_value(200);
        felled_tree.prototype_id = tree_prototype.value();
        felled_tree.cargo_prototype_id = log_cargo_prototype.value();
        felled_tree.kind = entities::PhysicalResourceKind::felled_tree;
        felled_tree.mass_grams = 80000;
        felled_tree.volume_milliliters = 160000;
        felled_tree.allowed_transport_modes = cargo::CargoTransportModes::of(
            {cargo::CargoTransportMode::cart, cargo::CargoTransportMode::wagon});
        felled_tree.hazard_tags.push_back("crush");
        felled_tree.segments.push_back(entities::PhysicalResourceSegment{
            physics::ShapeKind::box, physics::Vec3{0.0F, 0.0F, 0.0F},
            physics::Vec3{0.35F, 3.0F, 0.35F}, 0.5F, 0.5F});
        felled_tree.segments.push_back(entities::PhysicalResourceSegment{
            physics::ShapeKind::box, physics::Vec3{0.0F, 2.8F, 0.0F},
            physics::Vec3{0.9F, 0.35F, 0.9F}, 0.5F, 0.5F});

        auto tree_body_desc = entities::make_physical_resource_body_desc(
            felled_tree, physics::Vec3{0.0F, 3.0F, 0.0F});
        if (!tree_body_desc) {
            core::log(core::LogLevel::error, tree_body_desc.error().message);
            return 1;
        }

        auto tree_body = world.value()->create_body(tree_body_desc.value());
        if (!tree_body) {
            core::log(core::LogLevel::error, tree_body.error().message);
            return 1;
        }
        auto attach_status =
            entities::attach_physical_resource_body(felled_tree, tree_body.value());
        if (!attach_status) {
            core::log(core::LogLevel::error, attach_status.error().message);
            return 1;
        }

        auto impulse_status =
            world.value()->apply_impulse(tree_body.value(), physics::Vec3{80.0F, 0.0F, 0.0F});
        if (!impulse_status) {
            core::log(core::LogLevel::error, impulse_status.error().message);
            return 1;
        }

        for (std::uint32_t step_index = 0; step_index < 5; ++step_index) {
            auto stats = world.value()->step(physics::PhysicsStepDesc{1.0F / 30.0F});
            if (!stats) {
                core::log(core::LogLevel::error, stats.error().message);
                return 1;
            }

            auto overlaps = world.value()->query_aabb(physics::PhysicsAabb{
                physics::Vec3{-2.0F, -1.0F, -2.0F}, physics::Vec3{2.0F, 5.0F, 2.0F}});
            if (!overlaps) {
                core::log(core::LogLevel::error, overlaps.error().message);
                return 1;
            }

            const auto state = world.value()->body_state(tree_body.value());
            if (!state) {
                core::log(core::LogLevel::error, "tree body disappeared");
                return 1;
            }

            const auto contacts = world.value()->drain_contacts();
            core::log(core::LogLevel::info,
                      "Step " + std::to_string(step_index) + ": tree position " +
                          std::to_string(state->position.x) + ", " +
                          std::to_string(state->position.y) + ", " +
                          std::to_string(state->position.z) + " | query overlaps " +
                          std::to_string(overlaps.value().size()) + " | contacts " +
                          std::to_string(contacts.size()) + " (stats " +
                          std::to_string(stats.value().contact_count) + ")");
        }

        auto settled_status = entities::mark_physical_resource_settled(felled_tree);
        auto frozen_status =
            settled_status ? entities::freeze_physical_resource(felled_tree) : settled_status;
        if (!settled_status || !frozen_status) {
            core::log(core::LogLevel::error, !settled_status ? settled_status.error().message
                                                             : frozen_status.error().message);
            return 1;
        }
        auto cargo_record = entities::convert_physical_resource_to_cargo(
            felled_tree, core::SaveId::from_value(201), *world.value());
        if (!cargo_record) {
            core::log(core::LogLevel::error, cargo_record.error().message);
            return 1;
        }
        core::log(core::LogLevel::info,
                  "Converted settled tree to cargo " + cargo_record.value().cargo_id.to_string() +
                      " with mass " + std::to_string(cargo_record.value().mass_grams) + "g");

        core::log(core::LogLevel::info, "Physics sandbox completed with " +
                                            std::to_string(world.value()->body_count()) +
                                            " bodies");
        return 0;
    });
}
