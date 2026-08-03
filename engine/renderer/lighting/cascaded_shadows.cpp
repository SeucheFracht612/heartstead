#include "engine/renderer/lighting/cascaded_shadows.hpp"

#include <algorithm>
#include <cmath>

namespace heartstead::renderer {

namespace {

[[nodiscard]] math::Vec3f normalized_or(math::Vec3f value, math::Vec3f fallback) noexcept {
    const auto length_squared = math::length_squared(value);
    return length_squared > 1.0e-8F ? value / std::sqrt(length_squared) : fallback;
}

[[nodiscard]] math::Mat4f light_view(math::Vec3f center, math::Vec3f toward_light) noexcept {
    const auto forward = normalized_or(toward_light * -1.0F, {0.0F, -1.0F, 0.0F});
    const auto up_reference =
        std::abs(forward.y) > 0.95F ? math::Vec3f{0.0F, 0.0F, 1.0F} : math::Vec3f{0.0F, 1.0F, 0.0F};
    const auto right = normalized_or(math::cross(forward, up_reference), {1.0F, 0.0F, 0.0F});
    const auto up = math::cross(right, forward);
    math::Mat4f result = math::Mat4f::identity();
    result.at(0, 0) = right.x;
    result.at(0, 1) = right.y;
    result.at(0, 2) = right.z;
    result.at(0, 3) = -math::dot(right, center);
    result.at(1, 0) = up.x;
    result.at(1, 1) = up.y;
    result.at(1, 2) = up.z;
    result.at(1, 3) = -math::dot(up, center);
    result.at(2, 0) = -forward.x;
    result.at(2, 1) = -forward.y;
    result.at(2, 2) = -forward.z;
    result.at(2, 3) = math::dot(forward, center);
    return result;
}

[[nodiscard]] math::Mat4f orthographic(float radius, float near_plane, float far_plane) noexcept {
    math::Mat4f result{};
    result.at(0, 0) = 1.0F / radius;
    // Vulkan's framebuffer coordinates use a downward-positive Y axis.
    result.at(1, 1) = -1.0F / radius;
    result.at(2, 2) = 1.0F / (near_plane - far_plane);
    result.at(2, 3) = near_plane / (near_plane - far_plane);
    result.at(3, 3) = 1.0F;
    return result;
}

} // namespace

core::Status DirectionalShadowConfig::validate() const {
    if (resolution < 256U || resolution > 8192U || (resolution & (resolution - 1U)) != 0U ||
        cascade_count == 0U || cascade_count > directional_shadow_cascade_count ||
        !std::isfinite(distance) || distance <= 1.0F || !std::isfinite(split_lambda) ||
        split_lambda < 0.0F || split_lambda > 1.0F || !std::isfinite(constant_bias) ||
        constant_bias < 0.0F || !std::isfinite(normal_bias) || normal_bias < 0.0F ||
        !std::isfinite(fade_fraction) || fade_fraction < 0.0F || fade_fraction > 0.5F) {
        return core::Status::failure(
            "directional_shadows.invalid_config",
            "shadow resolution, distance, split, bias, and fade settings are outside bounds");
    }
    return core::Status::ok();
}

CascadedShadowSystem::CascadedShadowSystem(rhi::IRenderDevice& device) : device_(&device) {}

CascadedShadowSystem::~CascadedShadowSystem() {
    (void)shutdown();
}

core::Status CascadedShadowSystem::initialize(DirectionalShadowConfig config) {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (data_buffer_.is_valid()) {
        return core::Status::failure("directional_shadows.already_initialized",
                                     "directional shadow system cannot be initialized twice");
    }
    config_ = config;
    auto buffer =
        device_->create_buffer({rhi::RenderBufferUsage::storage, sizeof(gpu_data_),
                                "directional_shadow_data", rhi::RenderBufferMemory::host_visible});
    if (!buffer) {
        return core::Status::failure(buffer.error().code, buffer.error().message);
    }
    data_buffer_ = buffer.value().handle;
    return core::Status::ok();
}

core::Status
CascadedShadowSystem::update(const RenderCamera& camera,
                             const rhi::RenderEnvironmentData& environment,
                             std::span<const RenderLightInstance> local_shadow_lights) {
    if (!data_buffer_.is_valid()) {
        return core::Status::failure("directional_shadows.not_initialized",
                                     "directional shadow system must be initialized first");
    }
    const auto near_plane = camera.near_plane;
    const auto far_plane = std::min(camera.far_plane, config_.distance);
    float previous_split = near_plane;
    for (std::uint32_t cascade = 0; cascade < config_.cascade_count; ++cascade) {
        const auto fraction =
            static_cast<float>(cascade + 1U) / static_cast<float>(config_.cascade_count);
        const auto logarithmic = near_plane * std::pow(far_plane / near_plane, fraction);
        const auto uniform = near_plane + (far_plane - near_plane) * fraction;
        const auto split =
            config_.split_lambda * logarithmic + (1.0F - config_.split_lambda) * uniform;
        gpu_data_.split_distances[cascade] = split;

        const auto center_distance = (previous_split + split) * 0.5F;
        const auto half_depth = (split - previous_split) * 0.5F;
        const auto half_height = split * std::tan(camera.vertical_fov_radians * 0.5F);
        const auto half_width = half_height * camera.aspect_ratio;
        auto radius = std::sqrt(half_depth * half_depth + half_height * half_height +
                                half_width * half_width);
        const auto texel_world = (2.0F * radius) / static_cast<float>(config_.resolution);
        radius = std::ceil(radius / texel_world) * texel_world;
        auto center = camera.local_position + camera.forward() * center_distance;
        auto view = light_view(center, environment.sun_direction);
        auto light_center = view * math::Vec4f{center.x, center.y, center.z, 1.0F};
        light_center.x = std::floor(light_center.x / texel_world) * texel_world;
        light_center.y = std::floor(light_center.y / texel_world) * texel_world;
        // Move the world-space center by the light-space snap delta. This makes the projection
        // stable under sub-texel camera motion.
        const auto right = math::Vec3f{view.at(0, 0), view.at(0, 1), view.at(0, 2)};
        const auto up = math::Vec3f{view.at(1, 0), view.at(1, 1), view.at(1, 2)};
        const auto unsnapped = view * math::Vec4f{center.x, center.y, center.z, 1.0F};
        center =
            center + right * (light_center.x - unsnapped.x) + up * (light_center.y - unsnapped.y);
        view = light_view(center, environment.sun_direction);
        gpu_data_.light_view_projection[cascade] =
            orthographic(radius, -radius * 2.0F, radius * 2.0F) * view;
        previous_split = split;
    }
    for (std::uint32_t cascade = config_.cascade_count; cascade < directional_shadow_cascade_count;
         ++cascade) {
        gpu_data_.split_distances[cascade] = far_plane;
        gpu_data_.light_view_projection[cascade] =
            gpu_data_.light_view_projection[config_.cascade_count - 1U];
    }
    gpu_data_.parameters[0] = config_.constant_bias;
    gpu_data_.parameters[1] = config_.normal_bias;
    gpu_data_.parameters[2] = config_.fade_fraction;
    gpu_data_.parameters[3] = static_cast<float>(debug_view_);
    gpu_data_.environment_parameters[0] = environment.sky_diffuse_intensity;
    gpu_data_.environment_parameters[1] = environment.environment_specular_intensity;
    gpu_data_.environment_parameters[2] = environment.environment_rotation_radians;
    gpu_data_.camera_position[0] = camera.local_position.x;
    gpu_data_.camera_position[1] = camera.local_position.y;
    gpu_data_.camera_position[2] = camera.local_position.z;
    const auto camera_forward = camera.forward();
    gpu_data_.camera_forward[0] = camera_forward.x;
    gpu_data_.camera_forward[1] = camera_forward.y;
    gpu_data_.camera_forward[2] = camera_forward.z;
    gpu_data_.atmosphere_parameters[0] = environment.elapsed_seconds;
    gpu_data_.atmosphere_parameters[1] = environment.height_fog_density;
    gpu_data_.atmosphere_parameters[2] = environment.height_fog_falloff;
    gpu_data_.atmosphere_parameters[3] = environment.aerial_perspective;
    gpu_data_.wind_parameters[0] = environment.wind_velocity.x;
    gpu_data_.wind_parameters[1] = environment.wind_velocity.y;
    gpu_data_.wind_parameters[2] = environment.wind_velocity.z;
    gpu_data_.wind_parameters[3] = environment.wind_gust_strength;
    gpu_data_.weather_parameters[0] = environment.precipitation_intensity;
    gpu_data_.weather_parameters[1] = environment.wetness;
    gpu_data_.weather_parameters[2] = environment.snow;
    gpu_data_.weather_parameters[3] = environment.storm_intensity;
    gpu_data_.sky_zenith_cloud[0] = environment.sky_zenith_color.x;
    gpu_data_.sky_zenith_cloud[1] = environment.sky_zenith_color.y;
    gpu_data_.sky_zenith_cloud[2] = environment.sky_zenith_color.z;
    gpu_data_.sky_zenith_cloud[3] = environment.cloud_coverage;
    gpu_data_.sky_horizon_cloud[0] = environment.sky_horizon_color.x;
    gpu_data_.sky_horizon_cloud[1] = environment.sky_horizon_color.y;
    gpu_data_.sky_horizon_cloud[2] = environment.sky_horizon_color.z;
    gpu_data_.sky_horizon_cloud[3] = environment.cloud_density;
    gpu_data_.water_shallow_absorption[0] = environment.water_shallow_color.x;
    gpu_data_.water_shallow_absorption[1] = environment.water_shallow_color.y;
    gpu_data_.water_shallow_absorption[2] = environment.water_shallow_color.z;
    gpu_data_.water_shallow_absorption[3] = environment.water_absorption_distance;
    gpu_data_.water_deep_scattering[0] = environment.water_deep_color.x;
    gpu_data_.water_deep_scattering[1] = environment.water_deep_color.y;
    gpu_data_.water_deep_scattering[2] = environment.water_deep_color.z;
    gpu_data_.water_deep_scattering[3] = environment.water_scattering_strength;
    gpu_data_.water_scattering_refraction[0] = environment.water_scattering_color.x;
    gpu_data_.water_scattering_refraction[1] = environment.water_scattering_color.y;
    gpu_data_.water_scattering_refraction[2] = environment.water_scattering_color.z;
    gpu_data_.water_scattering_refraction[3] = environment.water_refraction_strength;
    gpu_data_.water_foam_strength[0] = environment.water_foam_color.x;
    gpu_data_.water_foam_strength[1] = environment.water_foam_color.y;
    gpu_data_.water_foam_strength[2] = environment.water_foam_color.z;
    gpu_data_.water_foam_strength[3] = environment.water_foam_strength;
    gpu_data_.water_parameters[0] = environment.water_normal_strength;
    gpu_data_.water_parameters[1] = environment.water_normal_speed;
    gpu_data_.water_parameters[2] = environment.water_fresnel_f0;
    gpu_data_.water_parameters[3] = environment.water_ripple_strength;
    for (std::size_t slot = 0; slot < local_shadow_map_count; ++slot) {
        gpu_data_.local_light_view_projection[slot] = math::Mat4f::identity();
        gpu_data_.local_parameters[slot] = {};
    }
    const auto active_local_count =
        std::min(local_shadow_lights.size(), static_cast<std::size_t>(local_shadow_map_count));
    for (std::size_t slot = 0; slot < active_local_count; ++slot) {
        const auto& light = local_shadow_lights[slot];
        if (light.kind != RenderLightKind::spot || !light.casts_shadow ||
            light.radius <= camera.near_plane) {
            return core::Status::failure(
                "directional_shadows.invalid_local_shadow_light",
                "local shadow maps require selected spotlights with a valid range");
        }
        const auto half_angle = std::acos(std::clamp(light.outer_cone_cosine, -0.99F, 0.99F));
        const auto field_of_view = std::clamp(half_angle * 2.0F, 0.1F, 3.0F);
        const auto projection =
            math::perspective_projection(field_of_view, 1.0F, camera.near_plane, light.radius);
        const auto view = light_view(light.camera_relative_position, light.direction * -1.0F);
        gpu_data_.local_light_view_projection[slot] = projection * view;
        gpu_data_.local_parameters[slot][0] = config_.constant_bias * 2.0F;
        gpu_data_.local_parameters[slot][1] = config_.normal_bias * 2.0F;
        gpu_data_.local_parameters[slot][2] = light.radius;
        gpu_data_.local_parameters[slot][3] = 1.0F;
    }
    const rhi::RenderBufferWrite write{data_buffer_, 0, std::as_bytes(std::span{&gpu_data_, 1})};
    auto uploaded = device_->upload_buffer_batch(std::span{&write, 1});
    return uploaded ? core::Status::ok()
                    : core::Status::failure(uploaded.error().code, uploaded.error().message);
}

core::Status CascadedShadowSystem::bind(core::PrototypeId material, std::string_view binding) {
    const rhi::RenderDescriptorWrite write{material, std::string(binding), data_buffer_, 0,
                                           sizeof(GpuDirectionalShadowData)};
    auto result = device_->write_descriptors(std::span{&write, 1});
    return result ? core::Status::ok()
                  : core::Status::failure(result.error().code, result.error().message);
}

core::Status CascadedShadowSystem::shutdown() {
    if (!data_buffer_.is_valid()) {
        return core::Status::ok();
    }
    auto status = device_->release_resource(data_buffer_);
    data_buffer_ = {};
    gpu_data_ = {};
    return status;
}

void CascadedShadowSystem::set_debug_view(LightingDebugView view) noexcept {
    debug_view_ = view;
}

const DirectionalShadowConfig& CascadedShadowSystem::config() const noexcept {
    return config_;
}

const GpuDirectionalShadowData& CascadedShadowSystem::gpu_data() const noexcept {
    return gpu_data_;
}

rhi::RenderResourceHandle CascadedShadowSystem::data_buffer() const noexcept {
    return data_buffer_;
}

} // namespace heartstead::renderer
