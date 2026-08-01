#include "engine/renderer/scene/scene_render_system.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] float bounds_depth_squared(const math::Bounds3f& bounds) noexcept {
    const auto center = (bounds.min + bounds.max) * 0.5F;
    return math::length_squared(center);
}

} // namespace

bool ScenePipelineSet::is_valid() const noexcept {
    return opaque.is_valid() && alpha_tested.is_valid() && transparent.is_valid() &&
           additive.is_valid() && premultiplied.is_valid() && opaque_two_sided.is_valid() &&
           alpha_tested_two_sided.is_valid() && transparent_two_sided.is_valid() &&
           additive_two_sided.is_valid() && premultiplied_two_sided.is_valid();
}

rhi::RenderResourceHandle ScenePipelineSet::for_layer(RenderLayer layer,
                                                      bool two_sided) const noexcept {
    switch (layer) {
    case RenderLayer::opaque:
        return two_sided ? opaque_two_sided : opaque;
    case RenderLayer::alpha_tested:
        return two_sided ? alpha_tested_two_sided : alpha_tested;
    case RenderLayer::transparent:
        return two_sided ? transparent_two_sided : transparent;
    case RenderLayer::additive:
        return two_sided ? additive_two_sided : additive;
    case RenderLayer::premultiplied:
        return two_sided ? premultiplied_two_sided : premultiplied;
    }
    return {};
}

core::Status SceneRenderConfig::validate() const {
    constexpr auto maximum_size = std::numeric_limits<std::size_t>::max();
    if (maximum_instances_per_frame == 0 || buffered_frames < 2 || buffered_frames > 8 ||
        maximum_skin_matrices_per_frame == 0 || maximum_morph_weights_per_frame == 0 ||
        maximum_instances_per_frame > maximum_size / sizeof(GpuObjectInstance) / buffered_frames ||
        maximum_skin_matrices_per_frame > maximum_size / sizeof(math::Mat4f) / buffered_frames ||
        maximum_morph_weights_per_frame > maximum_size / sizeof(float) / buffered_frames ||
        static_cast<std::uint64_t>(maximum_skin_matrices_per_frame) * 2U * buffered_frames >
            std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(maximum_morph_weights_per_frame) * 2U * buffered_frames >
            std::numeric_limits<std::uint32_t>::max()) {
        return core::Status::failure(
            "scene_render.invalid_config",
            "scene instance/palette capacity must be nonzero and use two to eight buffered frames");
    }
    return core::Status::ok();
}

SceneRenderSystem::SceneRenderSystem(rhi::IRenderDevice& device, MeshManager& meshes,
                                     MaterialRuntimeCache& materials,
                                     MaterialRuntimeHandle fallback_material,
                                     ScenePipelineSet pipelines,
                                     core::PrototypeId pipeline_material)
    : device_(&device), meshes_(&meshes), materials_(&materials),
      fallback_material_(fallback_material), pipelines_(pipelines),
      pipeline_material_(std::move(pipeline_material)) {}

SceneRenderSystem::~SceneRenderSystem() {
    (void)shutdown();
}

core::Status SceneRenderSystem::initialize(SceneRenderConfig config) {
    if (instance_buffer_.is_valid()) {
        return core::Status::failure("scene_render.already_initialized",
                                     "scene render system cannot be initialized twice");
    }
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (!pipelines_.is_valid() || !pipeline_material_.is_valid()) {
        return core::Status::failure("scene_render.invalid_pipeline",
                                     "scene render system requires retained pipelines");
    }
    config_ = config;
    const auto instance_count =
        static_cast<std::size_t>(config_.maximum_instances_per_frame) * config_.buffered_frames;
    const auto byte_size = instance_count * sizeof(GpuObjectInstance);
    auto buffer =
        device_->create_buffer({rhi::RenderBufferUsage::storage, byte_size, "scene_instance_buffer",
                                rhi::RenderBufferMemory::device_local});
    if (!buffer) {
        return core::Status::failure(buffer.error().code, buffer.error().message);
    }
    instance_buffer_ = buffer.value().handle;
    const auto skin_matrix_count = static_cast<std::size_t>(config_.maximum_skin_matrices_per_frame) *
                                   2U * config_.buffered_frames;
    const auto skin_matrix_byte_size = skin_matrix_count * sizeof(math::Mat4f);
    auto skin_matrix_buffer =
        device_->create_buffer({rhi::RenderBufferUsage::storage, skin_matrix_byte_size,
                                "scene_skin_matrix_buffer", rhi::RenderBufferMemory::device_local});
    if (!skin_matrix_buffer) {
        const auto error = skin_matrix_buffer.error();
        (void)device_->release_resource(instance_buffer_);
        instance_buffer_ = {};
        return core::Status::failure(error.code, error.message);
    }
    skin_matrix_buffer_ = skin_matrix_buffer.value().handle;
    const auto morph_weight_count = static_cast<std::size_t>(config_.maximum_morph_weights_per_frame) *
                                    2U * config_.buffered_frames;
    const auto morph_weight_byte_size = morph_weight_count * sizeof(float);
    auto morph_weight_buffer = device_->create_buffer(
        {rhi::RenderBufferUsage::storage, morph_weight_byte_size, "scene_morph_weight_buffer",
         rhi::RenderBufferMemory::device_local});
    if (!morph_weight_buffer) {
        const auto error = morph_weight_buffer.error();
        (void)device_->release_resource(instance_buffer_);
        (void)device_->release_resource(skin_matrix_buffer_);
        instance_buffer_ = {};
        skin_matrix_buffer_ = {};
        return core::Status::failure(error.code, error.message);
    }
    morph_weight_buffer_ = morph_weight_buffer.value().handle;
    const std::array writes{
        rhi::RenderDescriptorWrite{pipeline_material_, "object_instances", instance_buffer_, 0,
                                   byte_size},
        rhi::RenderDescriptorWrite{pipeline_material_, "skin_matrices", skin_matrix_buffer_, 0,
                                   skin_matrix_byte_size},
        rhi::RenderDescriptorWrite{
            pipeline_material_, "morph_deltas", meshes_->morph_delta_buffer(), 0,
            static_cast<std::size_t>(meshes_->stats().morph_arena.capacity_bytes)},
        rhi::RenderDescriptorWrite{pipeline_material_, "morph_weights", morph_weight_buffer_, 0,
                                   morph_weight_byte_size},
    };
    auto descriptor = device_->write_descriptors(writes);
    if (!descriptor) {
        const auto error = descriptor.error();
        (void)device_->release_resource(instance_buffer_);
        (void)device_->release_resource(skin_matrix_buffer_);
        (void)device_->release_resource(morph_weight_buffer_);
        instance_buffer_ = {};
        skin_matrix_buffer_ = {};
        morph_weight_buffer_ = {};
        return core::Status::failure(error.code, error.message);
    }
    instance_scratch_.reserve(config_.maximum_instances_per_frame);
    skin_matrix_scratch_.reserve(static_cast<std::size_t>(config_.maximum_skin_matrices_per_frame) *
                                 2U);
    morph_weight_scratch_.reserve(static_cast<std::size_t>(config_.maximum_morph_weights_per_frame) *
                                  2U);
    uploaded_skin_palettes_.reserve(config_.maximum_instances_per_frame);
    stats_.instance_buffer_bytes = byte_size;
    stats_.skin_matrix_buffer_bytes = skin_matrix_byte_size;
    stats_.morph_weight_buffer_bytes = morph_weight_byte_size;
    return core::Status::ok();
}

core::Result<SceneDrawCommands>
SceneRenderSystem::build_draw_commands(const RenderScene& scene, const RenderCamera& camera,
                                       float simulation_alpha, SceneDrawCommands scratch,
                                       std::span<const math::Mat4f> shadow_view_projections) {
    if (!instance_buffer_.is_valid()) {
        return core::Result<SceneDrawCommands>::failure(
            "scene_render.not_initialized", "scene render system must be initialized first");
    }
    auto extracted = scene.extract(camera, simulation_alpha, shadow_view_projections);
    if (!extracted) {
        return core::Result<SceneDrawCommands>::failure(extracted.error().code,
                                                        extracted.error().message);
    }
    scratch.opaque_and_cutout.clear();
    scratch.transparent.clear();
    scratch.shadow_casters.resize(shadow_view_projections.size());
    for (auto& shadow_casters : scratch.shadow_casters) {
        shadow_casters.clear();
    }
    scratch.lights = std::move(extracted.value().lights);
    instance_scratch_.clear();
    skin_matrix_scratch_.clear();
    morph_weight_scratch_.clear();
    uploaded_skin_palettes_.clear();
    stats_ = {};
    stats_.scene = extracted.value().stats;
    stats_.instance_buffer_bytes = static_cast<std::uint64_t>(config_.maximum_instances_per_frame) *
                                   config_.buffered_frames * sizeof(GpuObjectInstance);
    stats_.skin_matrix_buffer_bytes =
        static_cast<std::uint64_t>(config_.maximum_skin_matrices_per_frame) * 2U *
        config_.buffered_frames * sizeof(math::Mat4f);
    stats_.morph_weight_buffer_bytes =
        static_cast<std::uint64_t>(config_.maximum_morph_weights_per_frame) * 2U *
        config_.buffered_frames * sizeof(float);

    const auto frame_slot = frame_number_ % config_.buffered_frames;
    const auto segment_instance_offset =
        static_cast<std::uint64_t>(frame_slot) * config_.maximum_instances_per_frame;
    const auto segment_byte_offset = segment_instance_offset * sizeof(GpuObjectInstance);
    const auto segment_skin_matrix_offset = static_cast<std::uint64_t>(frame_slot) *
                                            config_.maximum_skin_matrices_per_frame * 2U;
    const auto segment_skin_matrix_byte_offset = segment_skin_matrix_offset * sizeof(math::Mat4f);
    const auto segment_morph_weight_offset = static_cast<std::uint64_t>(frame_slot) *
                                             config_.maximum_morph_weights_per_frame * 2U;
    const auto segment_morph_weight_byte_offset = segment_morph_weight_offset * sizeof(float);

    std::size_t submitted_current_skin_matrices = 0;
    std::size_t submitted_current_morph_weights = 0;
    for (auto& batch : extracted.value().batches) {
        if (instance_scratch_.size() >= config_.maximum_instances_per_frame) {
            stats_.dropped_instances += static_cast<std::uint32_t>(batch.instances.size());
            continue;
        }
        if (batch.layer == RenderLayer::transparent || batch.layer == RenderLayer::additive ||
            batch.layer == RenderLayer::premultiplied) {
            std::ranges::stable_sort(batch.instances, [](const RenderObjectInstance& left,
                                                         const RenderObjectInstance& right) {
                return bounds_depth_squared(left.camera_relative_bounds) >
                       bounds_depth_squared(right.camera_relative_bounds);
            });
        }
        const auto* mesh = meshes_->find(batch.mesh);
        if (mesh == nullptr) {
            stats_.dropped_instances += static_cast<std::uint32_t>(batch.instances.size());
            continue;
        }
        const auto first_instance_in_segment = instance_scratch_.size();
        float accepted_sort_depth = 0.0F;
        for (const auto& instance : batch.instances) {
            if (instance_scratch_.size() >= config_.maximum_instances_per_frame) {
                ++stats_.dropped_instances;
                continue;
            }
            const auto object_history = object_motion_history_.find(instance.id);
            const auto object_history_valid =
                !instance.reset_motion_history && object_history != object_motion_history_.end() &&
                object_history->second.last_seen_frame + 1U == frame_number_ &&
                object_history->second.morph_weights.size() == instance.morph_weights.size();
            const auto morph_upload_count =
                instance.morph_weights.size() * (object_history_valid ? 2U : 1U);
            if (instance.morph_weights.size() != mesh->morph_target_count ||
                submitted_current_morph_weights + instance.morph_weights.size() >
                    config_.maximum_morph_weights_per_frame ||
                morph_weight_scratch_.size() + morph_upload_count >
                    static_cast<std::size_t>(config_.maximum_morph_weights_per_frame) * 2U ||
                mesh->vertex_count > 0x00FF'FFFFU) {
                ++stats_.dropped_instances;
                continue;
            }
            std::uint32_t skin_matrix_offset = 0;
            std::uint32_t previous_skin_matrix_offset = 0;
            std::uint32_t skin_matrix_count = 0;
            if (mesh->skin_joint_count > 0) {
                const auto* palette = scene.find_skin_palette(instance.skin_palette);
                if (palette == nullptr ||
                    palette->joint_matrices.size() != mesh->skin_joint_count) {
                    ++stats_.dropped_instances;
                    ++stats_.dropped_skinned_instances;
                    continue;
                }
                const auto found = std::ranges::find_if(
                    uploaded_skin_palettes_, [&](const UploadedSkinPalette& uploaded) {
                        return uploaded.id == instance.skin_palette;
                    });
                if (found != uploaded_skin_palettes_.end()) {
                    skin_matrix_offset = found->current_offset;
                    previous_skin_matrix_offset =
                        instance.reset_motion_history ? found->current_offset : found->previous_offset;
                    skin_matrix_count = found->count;
                } else {
                    const auto skin_history = skin_motion_history_.find(instance.skin_palette);
                    const auto skin_history_valid =
                        skin_history != skin_motion_history_.end() &&
                        skin_history->second.last_seen_frame + 1U == frame_number_ &&
                        skin_history->second.joint_matrices.size() == palette->joint_matrices.size();
                    const auto matrix_upload_count =
                        palette->joint_matrices.size() * (skin_history_valid ? 2U : 1U);
                    if (submitted_current_skin_matrices + palette->joint_matrices.size() >
                            config_.maximum_skin_matrices_per_frame ||
                        skin_matrix_scratch_.size() + matrix_upload_count >
                            static_cast<std::size_t>(config_.maximum_skin_matrices_per_frame) * 2U ||
                        segment_skin_matrix_offset + skin_matrix_scratch_.size() >
                            std::numeric_limits<std::uint32_t>::max()) {
                        ++stats_.dropped_instances;
                        ++stats_.dropped_skinned_instances;
                        continue;
                    }
                    skin_matrix_offset = static_cast<std::uint32_t>(segment_skin_matrix_offset +
                                                                    skin_matrix_scratch_.size());
                    skin_matrix_count = static_cast<std::uint32_t>(palette->joint_matrices.size());
                    skin_matrix_scratch_.insert(skin_matrix_scratch_.end(),
                                                palette->joint_matrices.begin(),
                                                palette->joint_matrices.end());
                    previous_skin_matrix_offset = skin_matrix_offset;
                    if (skin_history_valid) {
                        previous_skin_matrix_offset = static_cast<std::uint32_t>(
                            segment_skin_matrix_offset + skin_matrix_scratch_.size());
                        skin_matrix_scratch_.insert(
                            skin_matrix_scratch_.end(),
                            skin_history->second.joint_matrices.begin(),
                            skin_history->second.joint_matrices.end());
                    }
                    uploaded_skin_palettes_.push_back({instance.skin_palette, skin_matrix_offset,
                                                       previous_skin_matrix_offset,
                                                       skin_matrix_count});
                    submitted_current_skin_matrices += palette->joint_matrices.size();
                    skin_motion_history_[instance.skin_palette] =
                        {palette->joint_matrices, frame_number_};
                    ++stats_.submitted_skin_palettes;
                    stats_.submitted_skin_matrices += skin_matrix_count;
                }
            }
            if (instance_scratch_.size() == first_instance_in_segment) {
                accepted_sort_depth = bounds_depth_squared(instance.camera_relative_bounds);
            }
            GpuObjectInstance gpu;
            gpu.camera_relative_transform = instance.camera_relative_transform;
            const auto current_clip_transform =
                camera.view_projection * instance.camera_relative_transform;
            gpu.previous_clip_transform =
                object_history_valid ? object_history->second.clip_transform : current_clip_transform;
            std::ranges::copy(instance.color, gpu.color);
            gpu.metadata[0] = static_cast<std::uint32_t>(instance.layer);
            gpu.metadata[1] = skin_matrix_offset;
            gpu.metadata[2] = skin_matrix_count;
            const auto* material = materials_->find(batch.material);
            if (material == nullptr || material->domain != MaterialRuntimeDomain::surface) {
                material = materials_->find(fallback_material_);
            }
            gpu.metadata[3] = material == nullptr ? 0U : material->material_index;
            gpu.morph_metadata[0] =
                static_cast<std::uint32_t>(mesh->vertices.offset / sizeof(GpuStaticMeshVertex));
            gpu.morph_metadata[1] =
                mesh->morph_deltas.is_valid()
                    ? static_cast<std::uint32_t>(mesh->morph_deltas.offset / sizeof(GpuMorphDelta))
                    : 0U;
            gpu.morph_metadata[2] = static_cast<std::uint32_t>(segment_morph_weight_offset +
                                                               morph_weight_scratch_.size());
            gpu.morph_metadata[3] = (mesh->morph_target_count << 24U) | mesh->vertex_count;
            if (any(instance.effect_flags & RenderEffectFlags::water_surface)) {
                gpu.effect_parameters[0] = instance.water_world_phase.x;
                gpu.effect_parameters[1] = instance.water_world_phase.y;
            } else {
                gpu.effect_parameters[0] = instance.wind_phase;
                gpu.effect_parameters[1] = instance.wind_stiffness;
            }
            gpu.effect_parameters[2] = instance.foliage_transmission;
            gpu.effect_parameters[3] = instance.visibility;
            gpu.effect_metadata[0] = static_cast<std::uint32_t>(instance.effect_flags);
            gpu.effect_metadata[1] = instance.sprite_frame;
            gpu.effect_metadata[2] = instance.atlas_columns;
            gpu.effect_metadata[3] = instance.atlas_rows;
            std::ranges::copy(instance.effect_parameters2, gpu.effect_parameters2);
            gpu.history_metadata[0] = previous_skin_matrix_offset;
            gpu.history_metadata[1] = gpu.morph_metadata[2];
            gpu.history_metadata[2] = object_history_valid ? 1U : 0U;
            morph_weight_scratch_.insert(morph_weight_scratch_.end(),
                                         instance.morph_weights.begin(),
                                         instance.morph_weights.end());
            if (object_history_valid) {
                gpu.history_metadata[1] = static_cast<std::uint32_t>(
                    segment_morph_weight_offset + morph_weight_scratch_.size());
                morph_weight_scratch_.insert(morph_weight_scratch_.end(),
                                             object_history->second.morph_weights.begin(),
                                             object_history->second.morph_weights.end());
            }
            submitted_current_morph_weights += instance.morph_weights.size();
            object_motion_history_[instance.id] =
                {current_clip_transform, instance.morph_weights, frame_number_};
            instance_scratch_.push_back(gpu);
        }
        const auto accepted = instance_scratch_.size() - first_instance_in_segment;
        if (accepted == 0) {
            continue;
        }
        const auto vertex_offset = mesh->vertices.offset / sizeof(GpuStaticMeshVertex);
        const auto first_index =
            mesh->indices.offset / rhi::render_index_type_size(mesh->index_type);
        const auto first_instance = segment_instance_offset + first_instance_in_segment;
        if (vertex_offset > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
            first_index > std::numeric_limits<std::uint32_t>::max() ||
            first_instance > std::numeric_limits<std::uint32_t>::max()) {
            return core::Result<SceneDrawCommands>::failure(
                "scene_render.draw_offset_overflow",
                "scene mesh or instance offset exceeds RHI range");
        }
        rhi::RenderDrawCommand draw;
        draw.pipeline = pipelines_.for_layer(batch.layer, batch.two_sided);
        draw.vertex_buffer = mesh->vertices.buffer;
        draw.index_buffer = mesh->indices.buffer;
        draw.index_count = mesh->index_count;
        draw.first_index = static_cast<std::uint32_t>(first_index);
        draw.vertex_offset = static_cast<std::int32_t>(vertex_offset);
        draw.instance_count = static_cast<std::uint32_t>(accepted);
        draw.first_instance = static_cast<std::uint32_t>(first_instance);
        draw.index_type = mesh->index_type;
        draw.casts_shadow = batch.casts_shadow;
        if (batch.camera_visible &&
            (batch.layer == RenderLayer::transparent || batch.layer == RenderLayer::additive ||
             batch.layer == RenderLayer::premultiplied)) {
            draw.sort_depth = accepted_sort_depth;
            scratch.transparent.push_back(draw);
        } else if (batch.camera_visible) {
            scratch.opaque_and_cutout.push_back(draw);
        }
        for (std::size_t shadow_view = 0; shadow_view < scratch.shadow_casters.size();
             ++shadow_view) {
            if ((batch.shadow_visibility_mask & (std::uint64_t{1} << shadow_view)) != 0) {
                scratch.shadow_casters[shadow_view].push_back(draw);
            }
        }
        stats_.submitted_instances += static_cast<std::uint32_t>(accepted);
        stats_.draw_calls += batch.camera_visible ? 1U : 0U;
    }
    std::ranges::stable_sort(scratch.transparent, [](const auto& left, const auto& right) {
        return left.sort_depth > right.sort_depth;
    });
    const auto instance_bytes =
        std::as_bytes(std::span<const GpuObjectInstance>{instance_scratch_});
    const auto skin_matrix_bytes =
        std::as_bytes(std::span<const math::Mat4f>{skin_matrix_scratch_});
    const auto morph_weight_bytes = std::as_bytes(std::span<const float>{morph_weight_scratch_});
    std::array<rhi::RenderBufferWrite, 3> writes;
    std::size_t write_count = 0;
    if (!instance_bytes.empty()) {
        writes[write_count++] = {instance_buffer_, static_cast<std::size_t>(segment_byte_offset),
                                 instance_bytes};
    }
    if (!skin_matrix_bytes.empty()) {
        writes[write_count++] = {skin_matrix_buffer_,
                                 static_cast<std::size_t>(segment_skin_matrix_byte_offset),
                                 skin_matrix_bytes};
    }
    if (!morph_weight_bytes.empty()) {
        writes[write_count++] = {morph_weight_buffer_,
                                 static_cast<std::size_t>(segment_morph_weight_byte_offset),
                                 morph_weight_bytes};
    }
    if (write_count > 0) {
        auto upload = device_->upload_buffer_batch(
            std::span<const rhi::RenderBufferWrite>{writes.data(), write_count});
        if (!upload) {
            return core::Result<SceneDrawCommands>::failure(upload.error().code,
                                                            upload.error().message);
        }
        stats_.uploaded_instance_bytes = instance_bytes.size();
        stats_.uploaded_skin_matrix_bytes = skin_matrix_bytes.size();
        stats_.uploaded_morph_weight_bytes = morph_weight_bytes.size();
    }
    std::erase_if(object_motion_history_, [this](const auto& entry) {
        return entry.second.last_seen_frame != frame_number_;
    });
    std::erase_if(skin_motion_history_, [this](const auto& entry) {
        return entry.second.last_seen_frame != frame_number_;
    });
    ++frame_number_;
    scratch.stats = stats_;
    return core::Result<SceneDrawCommands>::success(std::move(scratch));
}

core::Status SceneRenderSystem::set_pipelines(ScenePipelineSet pipelines) noexcept {
    if (!pipelines.is_valid()) {
        return core::Status::failure("scene_render.invalid_pipeline",
                                     "scene pipelines must all be valid");
    }
    pipelines_ = pipelines;
    return core::Status::ok();
}

void SceneRenderSystem::reset_session() noexcept {
    instance_scratch_.clear();
    skin_matrix_scratch_.clear();
    morph_weight_scratch_.clear();
    uploaded_skin_palettes_.clear();
    object_motion_history_.clear();
    skin_motion_history_.clear();
    stats_ = {};
    frame_number_ = 0;
}

core::Status SceneRenderSystem::shutdown() {
    reset_session();
    auto status = core::Status::ok();
    if (instance_buffer_.is_valid()) {
        status = device_->release_resource(instance_buffer_);
        instance_buffer_ = {};
    }
    if (skin_matrix_buffer_.is_valid()) {
        auto skin_status = device_->release_resource(skin_matrix_buffer_);
        if (!skin_status && status) {
            status = skin_status;
        }
    }
    skin_matrix_buffer_ = {};
    if (morph_weight_buffer_.is_valid()) {
        auto morph_status = device_->release_resource(morph_weight_buffer_);
        if (!morph_status && status) {
            status = morph_status;
        }
    }
    morph_weight_buffer_ = {};
    return status;
}

const SceneRenderStats& SceneRenderSystem::stats() const noexcept {
    return stats_;
}

rhi::RenderResourceHandle SceneRenderSystem::instance_buffer() const noexcept {
    return instance_buffer_;
}

rhi::RenderResourceHandle SceneRenderSystem::skin_matrix_buffer() const noexcept {
    return skin_matrix_buffer_;
}

rhi::RenderResourceHandle SceneRenderSystem::morph_weight_buffer() const noexcept {
    return morph_weight_buffer_;
}

} // namespace heartstead::renderer
