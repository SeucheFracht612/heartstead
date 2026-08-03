#include "engine/renderer/lighting/clustered_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>

namespace heartstead::renderer {

namespace {

[[nodiscard]] std::uint32_t ceil_div(std::uint32_t value, std::uint32_t divisor) noexcept {
    return (value + divisor - 1U) / divisor;
}

[[nodiscard]] float light_score(const RenderLightInstance& light,
                                const RenderCamera& camera) noexcept {
    const auto to_light = light.camera_relative_position - camera.local_position;
    const auto distance_squared = std::max(math::length_squared(to_light), 1.0F);
    return light.intensity * light.radius * light.radius / distance_squared *
           std::max({light.color.x, light.color.y, light.color.z});
}

} // namespace

core::Status ClusteredLightingConfig::validate() const {
    if (tile_size < 8U || tile_size > 64U || (tile_size & (tile_size - 1U)) != 0U ||
        maximum_lights == 0U || maximum_lights > 65'535U || maximum_lights_per_tile == 0U ||
        maximum_lights_per_tile > 128U || local_shadow_budget > maximum_lights ||
        local_shadow_budget > local_shadow_map_count) {
        return core::Status::failure(
            "clustered_lighting.invalid_config",
            "tile size must be a power of two in 8..64 and light budgets must fit the "
            "configured GPU shadow-map count");
    }
    return core::Status::ok();
}

ClusteredLightingSystem::ClusteredLightingSystem(rhi::IRenderDevice& device) : device_(&device) {}

ClusteredLightingSystem::~ClusteredLightingSystem() {
    (void)shutdown();
}

core::Status ClusteredLightingSystem::initialize(rhi::RenderExtent extent,
                                                 ClusteredLightingConfig config) {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    status = rhi::validate_render_extent(extent);
    if (!status) {
        return status;
    }
    if (light_buffer_.is_valid()) {
        return core::Status::failure("clustered_lighting.already_initialized",
                                     "clustered lighting cannot be initialized twice");
    }
    config_ = config;
    auto lights = device_->create_buffer(
        {rhi::RenderBufferUsage::storage,
         static_cast<std::size_t>(config_.maximum_lights) * sizeof(GpuLocalLight),
         "clustered_local_lights", rhi::RenderBufferMemory::host_visible});
    if (!lights) {
        return core::Status::failure(lights.error().code, lights.error().message);
    }
    light_buffer_ = lights.value().handle;
    status = create_grid_buffer(extent);
    if (!status) {
        (void)shutdown();
        return status;
    }
    gpu_lights_.reserve(config_.maximum_lights);
    shadow_candidates_.reserve(config_.local_shadow_budget);
    return core::Status::ok();
}

core::Status ClusteredLightingSystem::create_grid_buffer(rhi::RenderExtent extent) {
    const auto tiles_x = ceil_div(extent.width, config_.tile_size);
    const auto tiles_y = ceil_div(extent.height, config_.tile_size);
    const auto tile_count = static_cast<std::uint64_t>(tiles_x) * tiles_y;
    const auto words = 4ULL + tile_count * (1ULL + config_.maximum_lights_per_tile);
    if (words > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)) {
        return core::Status::failure("clustered_lighting.grid_too_large",
                                     "clustered light grid exceeds addressable memory");
    }
    auto grid = device_->create_buffer(
        {rhi::RenderBufferUsage::storage, static_cast<std::size_t>(words) * sizeof(std::uint32_t),
         "clustered_light_grid", rhi::RenderBufferMemory::host_visible});
    if (!grid) {
        return core::Status::failure(grid.error().code, grid.error().message);
    }
    if (grid_buffer_.is_valid()) {
        auto released = device_->release_resource(grid_buffer_);
        if (!released) {
            (void)device_->release_resource(grid.value().handle);
            return released;
        }
    }
    grid_buffer_ = grid.value().handle;
    extent_ = extent;
    grid_.assign(static_cast<std::size_t>(words), 0U);
    empty_grid_resident_ = false;
    return core::Status::ok();
}

core::Status ClusteredLightingSystem::resize(rhi::RenderExtent extent) {
    auto status = rhi::validate_render_extent(extent);
    if (!status) {
        return status;
    }
    if (extent.width == extent_.width && extent.height == extent_.height) {
        return core::Status::ok();
    }
    return create_grid_buffer(extent);
}

core::Status ClusteredLightingSystem::update(std::span<const RenderLightInstance> lights,
                                             const RenderCamera& camera) {
    if (!light_buffer_.is_valid() || !grid_buffer_.is_valid()) {
        return core::Status::failure("clustered_lighting.not_initialized",
                                     "clustered lighting must be initialized before update");
    }
    stats_ = {};
    gpu_lights_.clear();
    shadow_candidates_.clear();
    const auto tiles_x = ceil_div(extent_.width, config_.tile_size);
    const auto tiles_y = ceil_div(extent_.height, config_.tile_size);
    const auto stride = 1U + config_.maximum_lights_per_tile;
    stats_.tile_count = tiles_x * tiles_y;
    const auto has_local_lights = std::ranges::any_of(
        lights, [](const auto& light) { return light.kind != RenderLightKind::directional; });
    if (!has_local_lights) {
        observed_shadow_revisions_.clear();
        if (empty_grid_resident_) {
            return core::Status::ok();
        }
        std::ranges::fill(grid_, 0U);
        grid_[0] = tiles_x;
        grid_[1] = tiles_y;
        grid_[2] = config_.tile_size;
        grid_[3] = config_.maximum_lights_per_tile;
        const rhi::RenderBufferWrite write{grid_buffer_, 0, std::as_bytes(std::span{grid_})};
        auto upload = device_->upload_buffer_batch(std::span{&write, 1});
        if (!upload) {
            return core::Status::failure(upload.error().code, upload.error().message);
        }
        stats_.uploaded_bytes = upload.value().byte_size;
        empty_grid_resident_ = true;
        return core::Status::ok();
    }
    std::ranges::fill(grid_, 0U);
    grid_[0] = tiles_x;
    grid_[1] = tiles_y;
    grid_[2] = config_.tile_size;
    grid_[3] = config_.maximum_lights_per_tile;
    std::map<RenderLightId, std::pair<std::uint64_t, std::uint64_t>> current_revisions;

    for (const auto& light : lights) {
        if (light.kind == RenderLightKind::directional) {
            continue;
        }
        if (gpu_lights_.size() >= config_.maximum_lights) {
            ++stats_.dropped_lights;
            continue;
        }
        GpuLocalLight gpu;
        gpu.position_radius[0] = light.camera_relative_position.x;
        gpu.position_radius[1] = light.camera_relative_position.y;
        gpu.position_radius[2] = light.camera_relative_position.z;
        gpu.position_radius[3] = light.radius;
        gpu.direction_kind[0] = light.direction.x;
        gpu.direction_kind[1] = light.direction.y;
        gpu.direction_kind[2] = light.direction.z;
        gpu.direction_kind[3] = static_cast<float>(light.kind);
        gpu.color_intensity[0] = light.color.x;
        gpu.color_intensity[1] = light.color.y;
        gpu.color_intensity[2] = light.color.z;
        gpu.color_intensity[3] = light.intensity;
        gpu.spot_shadow[0] = light.inner_cone_cosine;
        gpu.spot_shadow[1] = light.outer_cone_cosine;
        // Zero means unshadowed. Only spotlights selected after budget ranking receive a
        // one-based shadow slot; point lights and rejected spotlights must never alias slot 0.
        gpu.spot_shadow[2] = 0.0F;
        gpu.spot_shadow[3] = light.gameplay_importance;
        const auto light_index = static_cast<std::uint32_t>(gpu_lights_.size());
        gpu_lights_.push_back(gpu);

        const auto view_position =
            camera.view * math::Vec4f{light.camera_relative_position.x,
                                      light.camera_relative_position.y,
                                      light.camera_relative_position.z, 1.0F};
        const auto view_depth = -view_position.z;
        if (view_depth + light.radius <= camera.near_plane) {
            continue;
        }
        // Project against at least the near plane. This conservatively retains a light whose
        // center is behind the eye while its influence sphere still intersects visible space.
        const auto projection_depth = std::max(view_depth, camera.near_plane);
        const auto ndc_x = camera.projection.at(0, 0) * view_position.x / projection_depth;
        const auto ndc_y = camera.projection.at(1, 1) * view_position.y / projection_depth;
        const auto projected_radius =
            light.radius * static_cast<float>(extent_.height) /
            std::max(2.0F * std::tan(camera.vertical_fov_radians * 0.5F) * projection_depth,
                     0.001F);
        const auto center_x = (ndc_x * 0.5F + 0.5F) * static_cast<float>(extent_.width);
        const auto center_y = (ndc_y * 0.5F + 0.5F) * static_cast<float>(extent_.height);
        if (center_x + projected_radius < 0.0F || center_y + projected_radius < 0.0F ||
            center_x - projected_radius >= static_cast<float>(extent_.width) ||
            center_y - projected_radius >= static_cast<float>(extent_.height)) {
            continue;
        }
        const auto clamp_tile_x = [tiles_x, this](float pixel) {
            return std::clamp(
                static_cast<int>(std::floor(pixel / static_cast<float>(config_.tile_size))), 0,
                static_cast<int>(tiles_x) - 1);
        };
        const auto clamp_tile_y = [tiles_y, this](float pixel) {
            return std::clamp(
                static_cast<int>(std::floor(pixel / static_cast<float>(config_.tile_size))), 0,
                static_cast<int>(tiles_y) - 1);
        };
        const auto min_x = clamp_tile_x(center_x - projected_radius);
        const auto max_x = clamp_tile_x(center_x + projected_radius);
        const auto min_y = clamp_tile_y(center_y - projected_radius);
        const auto max_y = clamp_tile_y(center_y + projected_radius);
        for (auto y = min_y; y <= max_y; ++y) {
            for (auto x = min_x; x <= max_x; ++x) {
                const auto base =
                    4U + (static_cast<std::uint32_t>(y) * tiles_x + static_cast<std::uint32_t>(x)) *
                             stride;
                auto& count = grid_[base];
                if (count < config_.maximum_lights_per_tile) {
                    grid_[base + 1U + count++] = light_index;
                }
            }
        }
        if (light.casts_shadow && light.kind == RenderLightKind::spot) {
            const auto revisions = std::pair{light.light_revision, light.nearby_geometry_revision};
            const auto observed = observed_shadow_revisions_.find(light.id);
            const auto changed =
                observed == observed_shadow_revisions_.end() || observed->second != revisions;
            current_revisions.insert_or_assign(light.id, revisions);
            shadow_candidates_.push_back(
                {light.id,
                 light_score(light, camera) * std::max(light.gameplay_importance, 0.01F) *
                     (changed ? 1.25F : 1.0F),
                 light_index});
        }
    }
    std::ranges::stable_sort(shadow_candidates_, [](const auto& left, const auto& right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.id < right.id;
    });
    if (shadow_candidates_.size() > config_.local_shadow_budget) {
        shadow_candidates_.resize(config_.local_shadow_budget);
    }
    for (std::size_t slot = 0; slot < shadow_candidates_.size(); ++slot) {
        gpu_lights_[shadow_candidates_[slot].light_index].spot_shadow[2] =
            static_cast<float>(slot + 1U);
    }
    observed_shadow_revisions_ = std::move(current_revisions);
    for (std::uint32_t tile = 0; tile < stats_.tile_count; ++tile) {
        const auto count = grid_[4U + tile * stride];
        stats_.populated_tiles += count > 0U ? 1U : 0U;
        stats_.maximum_tile_light_count = std::max(stats_.maximum_tile_light_count, count);
    }
    std::array<rhi::RenderBufferWrite, 2> writes;
    std::size_t write_count = 0;
    if (!gpu_lights_.empty()) {
        writes[write_count++] = {light_buffer_, 0, std::as_bytes(std::span{gpu_lights_})};
    }
    writes[write_count++] = {grid_buffer_, 0, std::as_bytes(std::span{grid_})};
    auto upload = device_->upload_buffer_batch(std::span{writes.data(), write_count});
    if (!upload) {
        return core::Status::failure(upload.error().code, upload.error().message);
    }
    stats_.submitted_lights = static_cast<std::uint32_t>(gpu_lights_.size());
    stats_.selected_shadow_lights = static_cast<std::uint32_t>(shadow_candidates_.size());
    stats_.uploaded_bytes = upload.value().byte_size;
    empty_grid_resident_ = false;
    return core::Status::ok();
}

core::Status ClusteredLightingSystem::bind(core::PrototypeId material,
                                           std::string_view light_binding,
                                           std::string_view grid_binding) {
    const std::array writes{
        rhi::RenderDescriptorWrite{material, std::string(light_binding), light_buffer_, 0,
                                   static_cast<std::size_t>(config_.maximum_lights) *
                                       sizeof(GpuLocalLight)},
        rhi::RenderDescriptorWrite{material, std::string(grid_binding), grid_buffer_, 0,
                                   grid_.size() * sizeof(std::uint32_t)},
    };
    auto result = device_->write_descriptors(writes);
    return result ? core::Status::ok()
                  : core::Status::failure(result.error().code, result.error().message);
}

core::Status ClusteredLightingSystem::shutdown() {
    auto status = core::Status::ok();
    if (grid_buffer_.is_valid()) {
        status = device_->release_resource(grid_buffer_);
        grid_buffer_ = {};
    }
    if (light_buffer_.is_valid()) {
        const auto released = device_->release_resource(light_buffer_);
        if (!released && status) {
            status = released;
        }
        light_buffer_ = {};
    }
    gpu_lights_.clear();
    grid_.clear();
    shadow_candidates_.clear();
    observed_shadow_revisions_.clear();
    stats_ = {};
    extent_ = {};
    empty_grid_resident_ = false;
    return status;
}

rhi::RenderResourceHandle ClusteredLightingSystem::light_buffer() const noexcept {
    return light_buffer_;
}

rhi::RenderResourceHandle ClusteredLightingSystem::grid_buffer() const noexcept {
    return grid_buffer_;
}

const ClusteredLightingStats& ClusteredLightingSystem::stats() const noexcept {
    return stats_;
}

std::span<const LocalShadowCandidate>
ClusteredLightingSystem::selected_shadow_lights() const noexcept {
    return shadow_candidates_;
}

std::span<const GpuLocalLight> ClusteredLightingSystem::gpu_lights() const noexcept {
    return gpu_lights_;
}

} // namespace heartstead::renderer
