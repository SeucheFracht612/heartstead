#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace heartstead::renderer::rhi {

namespace {

[[nodiscard]] bool is_valid_render_name(std::string_view name) noexcept {
    if (name.empty() || name.front() == '.' || name.back() == '.') {
        return false;
    }
    for (const auto character : name) {
        const auto valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_' ||
                           character == '-' || character == '.';
        if (!valid) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool contains_name(const std::vector<std::string>& names, std::string_view name) {
    return std::ranges::any_of(names, [name](const std::string& value) { return value == name; });
}

[[nodiscard]] bool contains_duplicate_name(const std::vector<std::string>& names) {
    std::unordered_set<std::string_view> seen;
    for (const auto& name : names) {
        if (!seen.insert(name).second) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] core::Status validate_resource_ref(const RenderFramePlan& plan,
                                                 std::string_view resource_name,
                                                 std::string_view pass_name) {
    if (!is_valid_render_name(resource_name)) {
        return core::Status::failure("renderer_plan.invalid_resource_ref",
                                     "render pass references an invalid resource name: " +
                                         std::string(pass_name));
    }
    if (plan.find_resource(resource_name) == nullptr) {
        return core::Status::failure("renderer_plan.unknown_resource",
                                     "render pass references an unknown resource: " +
                                         std::string(resource_name));
    }
    return core::Status::ok();
}

struct ResourceAccessState {
    std::optional<std::size_t> last_write_use_index;
    std::vector<std::size_t> active_read_use_indices;
    std::optional<std::size_t> last_use_index;
    RenderResourceState current_state = RenderResourceState::undefined;
};

[[nodiscard]] bool access_reads(RenderResourceAccess access) noexcept {
    return access == RenderResourceAccess::read || access == RenderResourceAccess::read_write ||
           access == RenderResourceAccess::present;
}

[[nodiscard]] bool access_writes(RenderResourceAccess access) noexcept {
    return access == RenderResourceAccess::write || access == RenderResourceAccess::read_write;
}

[[nodiscard]] RenderResourceState initial_resource_state(RenderResourceLifetime lifetime) noexcept {
    switch (lifetime) {
    case RenderResourceLifetime::transient:
        return RenderResourceState::undefined;
    case RenderResourceLifetime::external:
        return RenderResourceState::external;
    }
    return RenderResourceState::undefined;
}

[[nodiscard]] RenderResourceState
resource_state_for_access(RenderResourceAccess access,
                          const RenderResourceDesc& resource) noexcept {
    if (resource.storage) {
        switch (access) {
        case RenderResourceAccess::read:
            return RenderResourceState::storage_read;
        case RenderResourceAccess::write:
            return RenderResourceState::storage_write;
        case RenderResourceAccess::read_write:
            return RenderResourceState::storage_read_write;
        case RenderResourceAccess::present:
            return RenderResourceState::present;
        }
    }
    switch (access) {
    case RenderResourceAccess::read:
        return RenderResourceState::shader_read;
    case RenderResourceAccess::write:
        return is_depth_format(resource.format) ? RenderResourceState::depth_attachment_write
                                                : RenderResourceState::color_attachment_write;
    case RenderResourceAccess::read_write:
        return is_depth_format(resource.format) ? RenderResourceState::depth_attachment_write
                                                : RenderResourceState::color_attachment_read_write;
    case RenderResourceAccess::present:
        return RenderResourceState::present;
    }
    return RenderResourceState::undefined;
}

[[nodiscard]] RenderResourceAccess resource_access_for_pass(const RenderPassDesc& pass,
                                                            std::string_view resource_name,
                                                            RenderResourceLifetime lifetime) {
    const auto reads = contains_name(pass.reads, resource_name);
    const auto writes = contains_name(pass.writes, resource_name);
    if (pass.presents && reads && lifetime == RenderResourceLifetime::external) {
        return RenderResourceAccess::present;
    }
    if (reads && writes) {
        return RenderResourceAccess::read_write;
    }
    return writes ? RenderResourceAccess::write : RenderResourceAccess::read;
}

[[nodiscard]] std::vector<std::string> ordered_unique_resource_refs(const RenderPassDesc& pass) {
    std::vector<std::string> names;
    names.reserve(pass.reads.size() + pass.writes.size());
    for (const auto& read : pass.reads) {
        if (!contains_name(names, read)) {
            names.push_back(read);
        }
    }
    for (const auto& write : pass.writes) {
        if (!contains_name(names, write)) {
            names.push_back(write);
        }
    }
    return names;
}

void add_dependency(RenderFrameExecutionPlan& execution_plan,
                    const RenderFrameResourceUse& source_use,
                    const RenderFrameResourceUse& destination_use, std::size_t source_use_index,
                    std::size_t destination_use_index) {
    execution_plan.dependencies.push_back(RenderFrameDependency{
        destination_use.resource_name,
        source_use_index,
        destination_use_index,
        source_use.pass_index,
        destination_use.pass_index,
        source_use.access,
        destination_use.access,
    });
}

void add_transition(RenderFrameExecutionPlan& execution_plan,
                    const ResourceAccessState& resource_state,
                    const RenderFrameResourceUse& destination_use,
                    std::size_t destination_use_index) {
    if (resource_state.current_state == destination_use.required_state) {
        return;
    }

    RenderFrameResourceTransition transition;
    transition.resource_name = destination_use.resource_name;
    transition.has_source_use = resource_state.last_use_index.has_value();
    if (transition.has_source_use) {
        transition.source_use_index = *resource_state.last_use_index;
        transition.source_pass_index =
            execution_plan.resource_uses[transition.source_use_index].pass_index;
    }
    transition.destination_use_index = destination_use_index;
    transition.destination_pass_index = destination_use.pass_index;
    transition.before_state = resource_state.current_state;
    transition.after_state = destination_use.required_state;
    execution_plan.transitions.push_back(std::move(transition));
}

} // namespace

core::Status RenderFramePlan::validate() const {
    auto status = validate_render_extent(extent);
    if (!status) {
        return status;
    }
    if (resources.empty()) {
        return core::Status::failure("renderer_plan.no_resources",
                                     "render frame plan must declare at least one resource");
    }
    if (passes.empty()) {
        return core::Status::failure("renderer_plan.no_passes",
                                     "render frame plan must declare at least one pass");
    }

    std::unordered_set<std::string> resource_names;
    std::unordered_set<std::string> pass_names;
    std::unordered_set<std::string> written_resources;
    std::size_t present_passes = 0;

    for (const auto& resource : resources) {
        if (!is_valid_render_name(resource.name)) {
            return core::Status::failure("renderer_plan.invalid_resource_name",
                                         "render resource name is invalid");
        }
        status = validate_render_extent(resource.extent);
        if (!status) {
            return status;
        }
        if (resource.array_layers == 0 || resource.mip_levels == 0 ||
            (resource.cubemap && (resource.array_layers < 6 || resource.array_layers % 6 != 0)) ||
            (resource.storage && (resource.lifetime == RenderResourceLifetime::external ||
                                  is_depth_format(resource.format)))) {
            return core::Status::failure(
                "renderer_plan.invalid_resource_shape",
                "render image layers/mips/cubemap/storage properties are invalid: " +
                    resource.name);
        }
        std::uint32_t maximum_mips = 1;
        for (auto dimension = std::max(resource.extent.width, resource.extent.height);
             dimension > 1; dimension /= 2) {
            ++maximum_mips;
        }
        if (resource.mip_levels > maximum_mips) {
            return core::Status::failure(
                "renderer_plan.invalid_resource_mips",
                "render image declares more mip levels than its extent permits: " + resource.name);
        }
        if (!resource_names.insert(resource.name).second) {
            return core::Status::failure("renderer_plan.duplicate_resource",
                                         "render resource name is duplicated: " + resource.name);
        }
        if (resource.lifetime == RenderResourceLifetime::external) {
            written_resources.insert(resource.name);
        }
    }

    for (const auto& pass : passes) {
        if (!is_valid_render_name(pass.name)) {
            return core::Status::failure("renderer_plan.invalid_pass_name",
                                         "render pass name is invalid");
        }
        if (!pass_names.insert(pass.name).second) {
            return core::Status::failure("renderer_plan.duplicate_pass",
                                         "render pass name is duplicated: " + pass.name);
        }
        if (contains_duplicate_name(pass.reads) || contains_duplicate_name(pass.writes)) {
            return core::Status::failure("renderer_plan.duplicate_pass_resource_ref",
                                         "render pass contains duplicate resource references: " +
                                             pass.name);
        }
        if (pass.reads.empty() && pass.writes.empty() && !pass.presents) {
            return core::Status::failure(
                "renderer_plan.empty_pass",
                "render pass must read, write, or present at least one resource");
        }

        std::size_t presented_external_resource_count = 0;
        for (const auto& read : pass.reads) {
            status = validate_resource_ref(*this, read, pass.name);
            if (!status) {
                return status;
            }
            if (!written_resources.contains(read)) {
                return core::Status::failure(
                    "renderer_plan.read_before_write",
                    "render pass reads a transient resource before it is written: " + read);
            }
            const auto* resource = find_resource(read);
            if (resource != nullptr && resource->lifetime == RenderResourceLifetime::external) {
                ++presented_external_resource_count;
            }
        }
        // A sampled resource is a read expressed through a descriptor binding, so it has to be
        // declared as a read too. Otherwise the graph would derive no dependency for it and the
        // backend would sample an image nothing transitioned to a readable layout.
        for (const auto& sampled : pass.sampled_resources) {
            if (sampled.binding_name.empty()) {
                return core::Status::failure("renderer_plan.invalid_sampled_binding",
                                             "render pass '" + pass.name +
                                                 "' samples a resource without a binding name");
            }
            status = validate_resource_ref(*this, sampled.resource_name, pass.name);
            if (!status) {
                return status;
            }
            if (!contains_name(pass.reads, sampled.resource_name)) {
                return core::Status::failure("renderer_plan.sampled_resource_not_read",
                                             "render pass '" + pass.name + "' samples '" +
                                                 sampled.resource_name +
                                                 "' without declaring it as a read");
            }
            const auto* resource = find_resource(sampled.resource_name);
            if (resource != nullptr && resource->storage) {
                return core::Status::failure(
                    "renderer_plan.storage_resource_sampled",
                    "storage graph resource must use a storage descriptor binding: " +
                        sampled.resource_name);
            }
        }
        for (const auto& storage : pass.storage_resources) {
            if (storage.binding_name.empty()) {
                return core::Status::failure(
                    "renderer_plan.invalid_storage_binding",
                    "render pass '" + pass.name +
                        "' uses a storage resource without a binding name");
            }
            status = validate_resource_ref(*this, storage.resource_name, pass.name);
            if (!status) {
                return status;
            }
            const auto* resource = find_resource(storage.resource_name);
            if (resource == nullptr || !resource->storage) {
                return core::Status::failure(
                    "renderer_plan.non_storage_resource_bound",
                    "render pass storage binding does not name a storage resource: " +
                        storage.resource_name);
            }
            const auto reads = storage.access == RenderResourceAccess::read ||
                               storage.access == RenderResourceAccess::read_write;
            const auto writes = storage.access == RenderResourceAccess::write ||
                                storage.access == RenderResourceAccess::read_write;
            if ((reads && !contains_name(pass.reads, storage.resource_name)) ||
                (writes && !contains_name(pass.writes, storage.resource_name)) ||
                storage.access == RenderResourceAccess::present) {
                return core::Status::failure(
                    "renderer_plan.storage_access_mismatch",
                    "storage descriptor access must match the pass read/write declarations: " +
                        storage.resource_name);
            }
        }
        for (const auto& write : pass.writes) {
            status = validate_resource_ref(*this, write, pass.name);
            if (!status) {
                return status;
            }
            written_resources.insert(write);
        }
        if (pass.presents) {
            ++present_passes;
            if (pass.kind != RenderPassKind::present) {
                return core::Status::failure("renderer_plan.invalid_present_pass",
                                             "presenting render pass must use present pass kind");
            }
            if (!pass.writes.empty()) {
                return core::Status::failure("renderer_plan.present_pass_writes",
                                             "presenting render pass must not write resources");
            }
            if (presented_external_resource_count == 0) {
                return core::Status::failure(
                    "renderer_plan.present_without_external_resource",
                    "presenting render pass must read an external resource");
            }
            if (presented_external_resource_count > 1) {
                return core::Status::failure(
                    "renderer_plan.ambiguous_present_resource",
                    "presenting render pass must read exactly one external resource");
            }
        }
    }

    if (present_passes > 1) {
        return core::Status::failure("renderer_plan.too_many_present_passes",
                                     "render frame plan can contain at most one present pass");
    }
    return core::Status::ok();
}

core::Result<RenderFrameExecutionPlan> RenderFramePlan::build_execution_plan() const {
    auto status = validate();
    if (!status) {
        return core::Result<RenderFrameExecutionPlan>::failure(status.error().code,
                                                               status.error().message);
    }

    RenderFrameExecutionPlan execution_plan;
    execution_plan.extent = extent;
    execution_plan.ordered_passes.reserve(passes.size());
    execution_plan.resource_uses.reserve(passes.size() * 2);
    execution_plan.transitions.reserve(passes.size() * 2);

    std::unordered_map<std::string, ResourceAccessState> resource_states;
    for (const auto& resource : resources) {
        ResourceAccessState state;
        state.current_state = initial_resource_state(resource.lifetime);
        resource_states.emplace(resource.name, std::move(state));
    }

    for (std::size_t pass_index = 0; pass_index < passes.size(); ++pass_index) {
        const auto& pass = passes[pass_index];
        execution_plan.ordered_passes.push_back(pass.name);
        if (pass.presents) {
            ++execution_plan.present_pass_count;
        }

        for (const auto& resource_name : ordered_unique_resource_refs(pass)) {
            const auto* resource = find_resource(resource_name);
            if (resource == nullptr) {
                return core::Result<RenderFrameExecutionPlan>::failure(
                    "renderer_plan.unknown_resource",
                    "render pass references an unknown resource: " + resource_name);
            }

            const auto use_index = execution_plan.resource_uses.size();
            execution_plan.resource_uses.push_back(RenderFrameResourceUse{
                pass_index,
                pass.name,
                pass.kind,
                resource_name,
                resource->lifetime,
                resource_access_for_pass(pass, resource_name, resource->lifetime),
                {},
            });
            execution_plan.resource_uses.back().required_state =
                resource_state_for_access(execution_plan.resource_uses.back().access, *resource);

            const auto& current_use = execution_plan.resource_uses.back();
            auto& resource_state = resource_states.at(resource_name);
            add_transition(execution_plan, resource_state, current_use, use_index);

            bool added_last_write_dependency = false;
            if (access_reads(current_use.access) &&
                resource_state.last_write_use_index.has_value()) {
                const auto source_use_index = *resource_state.last_write_use_index;
                add_dependency(execution_plan, execution_plan.resource_uses[source_use_index],
                               current_use, source_use_index, use_index);
                added_last_write_dependency = true;
            }
            if (access_writes(current_use.access)) {
                if (resource_state.last_write_use_index.has_value() &&
                    !added_last_write_dependency) {
                    const auto source_use_index = *resource_state.last_write_use_index;
                    add_dependency(execution_plan, execution_plan.resource_uses[source_use_index],
                                   current_use, source_use_index, use_index);
                }
                for (const auto source_use_index : resource_state.active_read_use_indices) {
                    add_dependency(execution_plan, execution_plan.resource_uses[source_use_index],
                                   current_use, source_use_index, use_index);
                }
                resource_state.last_write_use_index = use_index;
                resource_state.active_read_use_indices.clear();
            } else {
                resource_state.active_read_use_indices.push_back(use_index);
            }
            resource_state.current_state = current_use.required_state;
            resource_state.last_use_index = use_index;
        }
    }

    return core::Result<RenderFrameExecutionPlan>::success(std::move(execution_plan));
}

const RenderResourceDesc* RenderFramePlan::find_resource(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(
        resources, [name](const RenderResourceDesc& resource) { return resource.name == name; });
    return found == resources.end() ? nullptr : &*found;
}

std::size_t RenderFramePlan::pass_count(RenderPassKind kind) const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        passes, [kind](const RenderPassDesc& pass) { return pass.kind == kind; }));
}

bool RenderFramePlan::has_present_pass() const noexcept {
    return std::ranges::any_of(passes, [](const RenderPassDesc& pass) { return pass.presents; });
}

ClearColor RenderFramePlan::first_clear_color() const noexcept {
    const auto found = std::ranges::find_if(
        passes, [](const RenderPassDesc& pass) { return pass.kind == RenderPassKind::clear; });
    if (found != passes.end()) {
        return found->clear_color;
    }
    const auto writer = std::ranges::find_if(
        passes, [](const RenderPassDesc& pass) { return !pass.writes.empty(); });
    return writer == passes.end() ? ClearColor{} : writer->clear_color;
}

RenderFramePlanBuilder::RenderFramePlanBuilder(RenderExtent extent) {
    plan_.extent = extent;
}

core::Status RenderFramePlanBuilder::add_resource(RenderResourceDesc resource) {
    if (!is_valid_render_name(resource.name)) {
        return core::Status::failure("renderer_plan.invalid_resource_name",
                                     "render resource name is invalid");
    }
    if (plan_.find_resource(resource.name) != nullptr) {
        return core::Status::failure("renderer_plan.duplicate_resource",
                                     "render resource name is duplicated: " + resource.name);
    }
    plan_.resources.push_back(std::move(resource));
    return core::Status::ok();
}

core::Status RenderFramePlanBuilder::add_pass(RenderPassDesc pass) {
    if (!is_valid_render_name(pass.name)) {
        return core::Status::failure("renderer_plan.invalid_pass_name",
                                     "render pass name is invalid");
    }
    if (contains_name(pass.reads, "") || contains_name(pass.writes, "")) {
        return core::Status::failure("renderer_plan.invalid_resource_ref",
                                     "render pass resource references must not be empty");
    }
    plan_.passes.push_back(std::move(pass));
    return core::Status::ok();
}

core::Result<RenderFramePlan> RenderFramePlanBuilder::build() const {
    auto status = plan_.validate();
    if (!status) {
        return core::Result<RenderFramePlan>::failure(status.error().code, status.error().message);
    }
    return core::Result<RenderFramePlan>::success(plan_);
}

RenderFramePlan make_clear_present_frame_plan(RenderExtent extent, ClearColor clear_color,
                                              bool present) {
    RenderFramePlan plan;
    plan.extent = extent;
    plan.resources.push_back(RenderResourceDesc{
        "swapchain",
        extent,
        RenderResourceLifetime::external,
    });
    plan.passes.push_back(RenderPassDesc{
        "clear",
        RenderPassKind::clear,
        {},
        {"swapchain"},
        clear_color,
        false,
    });
    if (present) {
        plan.passes.push_back(RenderPassDesc{
            "present",
            RenderPassKind::present,
            {"swapchain"},
            {},
            {},
            true,
        });
    }
    return plan;
}

core::Status validate_render_frame_submission_shape(const RenderFrameSubmission& frame) {
    auto status = frame.plan.validate();
    if (!status) {
        return status;
    }
    if (!frame.camera.view_projection.is_finite()) {
        return core::Status::failure(
            "renderer.invalid_frame_camera",
            "render frame camera view-projection matrix must contain finite values");
    }
    status = validate_render_environment(frame.environment);
    if (!status) {
        return status;
    }
    status = validate_render_exposure(frame.exposure);
    if (!status) {
        return status;
    }

    std::unordered_set<std::size_t> commanded_passes;
    for (const auto& pass_commands : frame.pass_commands) {
        if (pass_commands.pass_index >= frame.plan.passes.size()) {
            return core::Status::failure("renderer.invalid_draw_pass_index",
                                         "render pass command index is outside the frame plan");
        }
        if (!commanded_passes.insert(pass_commands.pass_index).second) {
            return core::Status::failure("renderer.duplicate_draw_pass_commands",
                                         "render frame contains duplicate commands for a pass");
        }
        // A compute pass carries dispatches instead of draws, so the entry is empty of one or the
        // other by design. Only an entry carrying neither is meaningless.
        const auto& commanded_pass = frame.plan.passes[pass_commands.pass_index];
        if (pass_commands.draws.empty() && pass_commands.dispatches.empty()) {
            return core::Status::failure("renderer.empty_pass_command_list",
                                         "render pass commands must carry draws or dispatches");
        }
        if (commanded_pass.kind == RenderPassKind::present) {
            return core::Status::failure("renderer.draws_in_present_pass",
                                         "present passes cannot contain graphics draws");
        }
        if (commanded_pass.kind == RenderPassKind::compute && !pass_commands.draws.empty()) {
            return core::Status::failure("renderer.draws_in_compute_pass",
                                         "compute passes record dispatches, not graphics draws");
        }
        if (commanded_pass.kind != RenderPassKind::compute && !pass_commands.dispatches.empty()) {
            return core::Status::failure("renderer.dispatches_outside_compute_pass",
                                         "only compute passes may record dispatches");
        }
        for (const auto& dispatch : pass_commands.dispatches) {
            if (!dispatch.pipeline.is_valid()) {
                return core::Status::failure("renderer.invalid_dispatch_pipeline",
                                             "compute dispatch must reference a pipeline");
            }
            if (dispatch.group_count_x == 0 || dispatch.group_count_y == 0 ||
                dispatch.group_count_z == 0) {
                return core::Status::failure("renderer.invalid_dispatch_group_count",
                                             "compute dispatch group counts must be non-zero");
            }
        }

        for (const auto& draw : pass_commands.draws) {
            switch (draw.index_type) {
            case RenderIndexType::uint16:
            case RenderIndexType::uint32:
                break;
            default:
                return core::Status::failure("renderer.invalid_index_type",
                                             "render draw must use a supported index element type");
            }
            if (!draw.pipeline.is_valid()) {
                return core::Status::failure("renderer.invalid_draw_pipeline",
                                             "render draw must reference a graphics pipeline");
            }
            // An index buffer selects an indexed draw. A non-indexed draw needs neither an index
            // nor a vertex buffer, so a fullscreen pass can generate its geometry in the shader.
            if (draw.index_buffer.is_valid()) {
                if (!draw.vertex_buffer.is_valid()) {
                    return core::Status::failure("renderer.invalid_vertex_buffer",
                                                 "indexed render draw must reference a vertex "
                                                 "buffer");
                }
                if (draw.index_count == 0) {
                    return core::Status::failure("renderer.invalid_index_count",
                                                 "indexed render draw count must be non-zero");
                }
                if (draw.first_index >
                    std::numeric_limits<std::uint32_t>::max() - draw.index_count) {
                    return core::Status::failure("renderer.draw_index_range_overflow",
                                                 "render draw index range overflows uint32");
                }
            } else {
                if (draw.vertex_count == 0) {
                    return core::Status::failure("renderer.invalid_vertex_count",
                                                 "non-indexed render draw vertex count must be "
                                                 "non-zero");
                }
                if (draw.index_count != 0) {
                    return core::Status::failure("renderer.missing_index_buffer",
                                                 "render draw sets an index count without an index "
                                                 "buffer");
                }
            }
            if (!std::isfinite(draw.sort_depth)) {
                return core::Status::failure("renderer.invalid_draw_sort_depth",
                                             "render draw sort depth must be finite");
            }
            if (draw.instance_count == 0) {
                return core::Status::failure("renderer.invalid_instance_count",
                                             "render draw instance count must be non-zero");
            }
            if (!draw.camera_relative_origin.is_finite()) {
                return core::Status::failure(
                    "renderer.invalid_camera_relative_translation",
                    "render draw camera-relative origin must contain finite values");
            }
            if (draw.view_projection_override_enabled &&
                !draw.view_projection_override.is_finite()) {
                return core::Status::failure("renderer.invalid_view_projection_override",
                                             "render draw view-projection override must be finite");
            }
            if (draw.scissor_enabled) {
                const auto right = static_cast<std::uint64_t>(draw.scissor.x) +
                                   static_cast<std::uint64_t>(draw.scissor.width);
                const auto bottom = static_cast<std::uint64_t>(draw.scissor.y) +
                                    static_cast<std::uint64_t>(draw.scissor.height);
                if (draw.scissor.width == 0 || draw.scissor.height == 0 ||
                    right > frame.plan.extent.width || bottom > frame.plan.extent.height) {
                    return core::Status::failure(
                        "renderer.invalid_draw_scissor",
                        "enabled draw scissor must be nonempty and contained by the frame extent");
                }
            }
        }
    }
    return core::Status::ok();
}

core::Status validate_render_environment(const RenderEnvironmentData& environment) {
    const auto nonnegative = [](math::Vec3f value) {
        return value.x >= 0.0F && value.y >= 0.0F && value.z >= 0.0F;
    };
    if (!environment.sun_direction.is_finite() || !environment.sun_color.is_finite() ||
        !environment.ambient_color.is_finite() || !environment.fog_color.is_finite() ||
        !environment.sky_zenith_color.is_finite() ||
        !environment.sky_horizon_color.is_finite() || !environment.wind_velocity.is_finite() ||
        !environment.water_shallow_color.is_finite() ||
        !environment.water_deep_color.is_finite() ||
        !environment.water_scattering_color.is_finite() ||
        !environment.water_foam_color.is_finite() ||
        !std::isfinite(environment.sun_intensity) ||
        !std::isfinite(environment.elapsed_seconds) ||
        !std::isfinite(environment.fog_start) || !std::isfinite(environment.fog_end) ||
        !std::isfinite(environment.sky_diffuse_intensity) ||
        !std::isfinite(environment.environment_specular_intensity) ||
        !std::isfinite(environment.environment_rotation_radians) ||
        !std::isfinite(environment.aerial_perspective) ||
        !std::isfinite(environment.cloud_coverage) ||
        !std::isfinite(environment.cloud_density) ||
        !std::isfinite(environment.wind_gust_strength) ||
        !std::isfinite(environment.wind_gust_frequency) ||
        !std::isfinite(environment.wind_turbulence) ||
        !std::isfinite(environment.precipitation_intensity) ||
        !std::isfinite(environment.wetness) || !std::isfinite(environment.snow) ||
        !std::isfinite(environment.storm_intensity) ||
        !std::isfinite(environment.visibility) ||
        !std::isfinite(environment.height_fog_density) ||
        !std::isfinite(environment.height_fog_falloff) ||
        !std::isfinite(environment.local_fog_density) ||
        !std::isfinite(environment.water_absorption_distance) ||
        !std::isfinite(environment.water_scattering_strength) ||
        !std::isfinite(environment.water_refraction_strength) ||
        !std::isfinite(environment.water_foam_strength) ||
        !std::isfinite(environment.water_normal_strength) ||
        !std::isfinite(environment.water_normal_speed) ||
        !std::isfinite(environment.water_fresnel_f0) ||
        !std::isfinite(environment.water_ripple_strength) ||
        !std::isfinite(environment.underwater_fog_distance)) {
        return core::Status::failure("renderer.invalid_environment_finite",
                                     "render environment values must be finite");
    }
    if (math::length_squared(environment.sun_direction) <= 0.0F ||
        environment.sun_intensity < 0.0F || environment.elapsed_seconds < 0.0F ||
        !nonnegative(environment.sun_color) || !nonnegative(environment.ambient_color) ||
        !nonnegative(environment.fog_color) || !nonnegative(environment.sky_zenith_color) ||
        !nonnegative(environment.sky_horizon_color) ||
        !nonnegative(environment.water_shallow_color) ||
        !nonnegative(environment.water_deep_color) ||
        !nonnegative(environment.water_scattering_color) ||
        !nonnegative(environment.water_foam_color) || environment.fog_start < 0.0F ||
        environment.fog_end <= environment.fog_start || environment.sky_diffuse_intensity < 0.0F ||
        environment.environment_specular_intensity < 0.0F ||
        environment.aerial_perspective < 0.0F || environment.cloud_coverage < 0.0F ||
        environment.cloud_coverage > 1.0F || environment.cloud_density < 0.0F ||
        environment.cloud_density > 1.0F || environment.wind_gust_strength < 0.0F ||
        environment.wind_gust_frequency < 0.0F || environment.wind_turbulence < 0.0F ||
        environment.precipitation_intensity < 0.0F ||
        environment.precipitation_intensity > 1.0F || environment.wetness < 0.0F ||
        environment.wetness > 1.0F || environment.snow < 0.0F ||
        environment.snow > 1.0F || environment.storm_intensity < 0.0F ||
        environment.storm_intensity > 1.0F || environment.visibility < 0.0F ||
        environment.visibility > 1.0F || environment.height_fog_density < 0.0F ||
        environment.height_fog_falloff < 0.0F || environment.local_fog_density < 0.0F ||
        environment.water_absorption_distance <= 0.0F ||
        environment.water_scattering_strength < 0.0F ||
        environment.water_refraction_strength < 0.0F ||
        environment.water_foam_strength < 0.0F ||
        environment.water_normal_strength < 0.0F || environment.water_normal_speed < 0.0F ||
        environment.water_fresnel_f0 < 0.0F || environment.water_fresnel_f0 > 1.0F ||
        environment.water_ripple_strength < 0.0F ||
        environment.underwater_fog_distance <= 0.0F) {
        return core::Status::failure(
            "renderer.invalid_environment_range",
            "render environment lighting, atmosphere, weather, wind, and water values are "
            "outside their valid ranges");
    }
    return core::Status::ok();
}

std::string_view render_resource_lifetime_name(RenderResourceLifetime lifetime) noexcept {
    switch (lifetime) {
    case RenderResourceLifetime::transient:
        return "transient";
    case RenderResourceLifetime::external:
        return "external";
    }
    return "unknown";
}

std::string_view render_pass_kind_name(RenderPassKind kind) noexcept {
    switch (kind) {
    case RenderPassKind::clear:
        return "clear";
    case RenderPassKind::world:
        return "world";
    case RenderPassKind::post_process:
        return "post_process";
    case RenderPassKind::ui:
        return "ui";
    case RenderPassKind::debug:
        return "debug";
    case RenderPassKind::compute:
        return "compute";
    case RenderPassKind::present:
        return "present";
    }
    return "unknown";
}

std::string_view render_resource_access_name(RenderResourceAccess access) noexcept {
    switch (access) {
    case RenderResourceAccess::read:
        return "read";
    case RenderResourceAccess::write:
        return "write";
    case RenderResourceAccess::read_write:
        return "read_write";
    case RenderResourceAccess::present:
        return "present";
    }
    return "unknown";
}

std::string_view render_resource_state_name(RenderResourceState state) noexcept {
    switch (state) {
    case RenderResourceState::undefined:
        return "undefined";
    case RenderResourceState::external:
        return "external";
    case RenderResourceState::shader_read:
        return "shader_read";
    case RenderResourceState::color_attachment_write:
        return "color_attachment_write";
    case RenderResourceState::color_attachment_read_write:
        return "color_attachment_read_write";
    case RenderResourceState::depth_attachment_write:
        return "depth_attachment_write";
    case RenderResourceState::storage_read:
        return "storage_read";
    case RenderResourceState::storage_write:
        return "storage_write";
    case RenderResourceState::storage_read_write:
        return "storage_read_write";
    case RenderResourceState::transfer_source:
        return "transfer_source";
    case RenderResourceState::transfer_destination:
        return "transfer_destination";
    case RenderResourceState::present:
        return "present";
    }
    return "unknown";
}

} // namespace heartstead::renderer::rhi
