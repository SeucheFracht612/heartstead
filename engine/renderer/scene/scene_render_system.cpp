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
    return opaque.is_valid() && alpha_tested.is_valid() && transparent.is_valid();
}

rhi::RenderResourceHandle ScenePipelineSet::for_layer(RenderLayer layer) const noexcept {
    switch (layer) {
    case RenderLayer::opaque:
        return opaque;
    case RenderLayer::alpha_tested:
        return alpha_tested;
    case RenderLayer::transparent:
        return transparent;
    }
    return {};
}

core::Status SceneRenderConfig::validate() const {
    constexpr auto maximum_size = std::numeric_limits<std::size_t>::max();
    if (maximum_instances_per_frame == 0 || buffered_frames < 2 || buffered_frames > 8 ||
        maximum_skin_matrices_per_frame == 0 ||
        maximum_instances_per_frame > maximum_size / sizeof(GpuObjectInstance) / buffered_frames ||
        maximum_skin_matrices_per_frame > maximum_size / sizeof(math::Mat4f) / buffered_frames ||
        static_cast<std::uint64_t>(maximum_skin_matrices_per_frame) * buffered_frames >
            std::numeric_limits<std::uint32_t>::max()) {
        return core::Status::failure(
            "scene_render.invalid_config",
            "scene instance/palette capacity must be nonzero and use two to eight buffered frames");
    }
    return core::Status::ok();
}

SceneRenderSystem::SceneRenderSystem(rhi::IRenderDevice& device, MeshManager& meshes,
                                     ScenePipelineSet pipelines,
                                     core::PrototypeId pipeline_material)
    : device_(&device), meshes_(&meshes), pipelines_(pipelines),
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
    const auto skin_matrix_count =
        static_cast<std::size_t>(config_.maximum_skin_matrices_per_frame) * config_.buffered_frames;
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
    const std::array writes{
        rhi::RenderDescriptorWrite{pipeline_material_, "object_instances", instance_buffer_, 0,
                                   byte_size},
        rhi::RenderDescriptorWrite{pipeline_material_, "skin_matrices", skin_matrix_buffer_, 0,
                                   skin_matrix_byte_size},
    };
    auto descriptor = device_->write_descriptors(writes);
    if (!descriptor) {
        const auto error = descriptor.error();
        (void)device_->release_resource(instance_buffer_);
        (void)device_->release_resource(skin_matrix_buffer_);
        instance_buffer_ = {};
        skin_matrix_buffer_ = {};
        return core::Status::failure(error.code, error.message);
    }
    instance_scratch_.reserve(config_.maximum_instances_per_frame);
    skin_matrix_scratch_.reserve(config_.maximum_skin_matrices_per_frame);
    uploaded_skin_palettes_.reserve(config_.maximum_instances_per_frame);
    stats_.instance_buffer_bytes = byte_size;
    stats_.skin_matrix_buffer_bytes = skin_matrix_byte_size;
    return core::Status::ok();
}

core::Result<SceneDrawCommands> SceneRenderSystem::build_draw_commands(const RenderScene& scene,
                                                                       const RenderCamera& camera,
                                                                       float simulation_alpha,
                                                                       SceneDrawCommands scratch) {
    if (!instance_buffer_.is_valid()) {
        return core::Result<SceneDrawCommands>::failure(
            "scene_render.not_initialized", "scene render system must be initialized first");
    }
    auto extracted = scene.extract(camera, simulation_alpha);
    if (!extracted) {
        return core::Result<SceneDrawCommands>::failure(extracted.error().code,
                                                        extracted.error().message);
    }
    scratch.opaque_and_cutout.clear();
    scratch.transparent.clear();
    instance_scratch_.clear();
    skin_matrix_scratch_.clear();
    uploaded_skin_palettes_.clear();
    stats_ = {};
    stats_.scene = extracted.value().stats;
    stats_.instance_buffer_bytes = static_cast<std::uint64_t>(config_.maximum_instances_per_frame) *
                                   config_.buffered_frames * sizeof(GpuObjectInstance);
    stats_.skin_matrix_buffer_bytes =
        static_cast<std::uint64_t>(config_.maximum_skin_matrices_per_frame) *
        config_.buffered_frames * sizeof(math::Mat4f);

    const auto frame_slot = frame_number_ % config_.buffered_frames;
    const auto segment_instance_offset =
        static_cast<std::uint64_t>(frame_slot) * config_.maximum_instances_per_frame;
    const auto segment_byte_offset = segment_instance_offset * sizeof(GpuObjectInstance);
    const auto segment_skin_matrix_offset =
        static_cast<std::uint64_t>(frame_slot) * config_.maximum_skin_matrices_per_frame;
    const auto segment_skin_matrix_byte_offset = segment_skin_matrix_offset * sizeof(math::Mat4f);

    for (auto& batch : extracted.value().batches) {
        if (instance_scratch_.size() >= config_.maximum_instances_per_frame) {
            stats_.dropped_instances += static_cast<std::uint32_t>(batch.instances.size());
            continue;
        }
        if (batch.layer == RenderLayer::transparent) {
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
            std::uint32_t skin_matrix_offset = 0;
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
                    skin_matrix_offset = found->offset;
                    skin_matrix_count = found->count;
                } else {
                    const auto remaining =
                        static_cast<std::size_t>(config_.maximum_skin_matrices_per_frame) -
                        skin_matrix_scratch_.size();
                    if (palette->joint_matrices.size() > remaining ||
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
                    uploaded_skin_palettes_.push_back(
                        {instance.skin_palette, skin_matrix_offset, skin_matrix_count});
                    ++stats_.submitted_skin_palettes;
                    stats_.submitted_skin_matrices += skin_matrix_count;
                }
            }
            if (instance_scratch_.size() == first_instance_in_segment) {
                accepted_sort_depth = bounds_depth_squared(instance.camera_relative_bounds);
            }
            GpuObjectInstance gpu;
            gpu.camera_relative_transform = instance.camera_relative_transform;
            std::ranges::copy(instance.color, gpu.color);
            gpu.metadata[0] = static_cast<std::uint32_t>(instance.layer);
            gpu.metadata[1] = skin_matrix_offset;
            gpu.metadata[2] = skin_matrix_count;
            gpu.metadata[3] = instance.sprite_frame;
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
        draw.pipeline = pipelines_.for_layer(batch.layer);
        draw.vertex_buffer = mesh->vertices.buffer;
        draw.index_buffer = mesh->indices.buffer;
        draw.index_count = mesh->index_count;
        draw.first_index = static_cast<std::uint32_t>(first_index);
        draw.vertex_offset = static_cast<std::int32_t>(vertex_offset);
        draw.instance_count = static_cast<std::uint32_t>(accepted);
        draw.first_instance = static_cast<std::uint32_t>(first_instance);
        draw.index_type = mesh->index_type;
        if (batch.layer == RenderLayer::transparent) {
            draw.sort_depth = accepted_sort_depth;
            scratch.transparent.push_back(draw);
        } else {
            scratch.opaque_and_cutout.push_back(draw);
        }
        stats_.submitted_instances += static_cast<std::uint32_t>(accepted);
        ++stats_.draw_calls;
    }
    std::ranges::stable_sort(scratch.transparent, [](const auto& left, const auto& right) {
        return left.sort_depth > right.sort_depth;
    });
    const auto instance_bytes =
        std::as_bytes(std::span<const GpuObjectInstance>{instance_scratch_});
    const auto skin_matrix_bytes =
        std::as_bytes(std::span<const math::Mat4f>{skin_matrix_scratch_});
    std::array<rhi::RenderBufferWrite, 2> writes;
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
    if (write_count > 0) {
        auto upload = device_->upload_buffer_batch(
            std::span<const rhi::RenderBufferWrite>{writes.data(), write_count});
        if (!upload) {
            return core::Result<SceneDrawCommands>::failure(upload.error().code,
                                                            upload.error().message);
        }
        stats_.uploaded_instance_bytes = instance_bytes.size();
        stats_.uploaded_skin_matrix_bytes = skin_matrix_bytes.size();
    }
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

core::Status SceneRenderSystem::shutdown() {
    instance_scratch_.clear();
    skin_matrix_scratch_.clear();
    uploaded_skin_palettes_.clear();
    stats_ = {};
    frame_number_ = 0;
    auto status = core::Status::ok();
    if (instance_buffer_.is_valid()) {
        status = device_->release_resource(instance_buffer_);
        instance_buffer_ = {};
    }
    if (skin_matrix_buffer_.is_valid()) {
        auto skin_status = device_->release_resource(skin_matrix_buffer_);
        skin_matrix_buffer_ = {};
        if (!skin_status && status) {
            status = skin_status;
        }
    }
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

} // namespace heartstead::renderer
