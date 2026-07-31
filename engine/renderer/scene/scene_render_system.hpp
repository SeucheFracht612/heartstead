#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/assets/mesh_manager.hpp"
#include "engine/renderer/materials/material_runtime_cache.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/scene/render_scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace heartstead::renderer {

struct alignas(16) GpuObjectInstance {
    math::Mat4f camera_relative_transform = math::Mat4f::identity();
    float color[4]{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t metadata[4]{};
    std::uint32_t morph_metadata[4]{};
    float effect_parameters[4]{};
    std::uint32_t effect_metadata[4]{};
    float effect_parameters2[4]{};
};

static_assert(sizeof(GpuObjectInstance) == 160);
static_assert(offsetof(GpuObjectInstance, camera_relative_transform) == 0);
static_assert(offsetof(GpuObjectInstance, color) == 64);
static_assert(offsetof(GpuObjectInstance, metadata) == 80);
static_assert(offsetof(GpuObjectInstance, morph_metadata) == 96);
static_assert(offsetof(GpuObjectInstance, effect_parameters) == 112);
static_assert(offsetof(GpuObjectInstance, effect_metadata) == 128);
static_assert(offsetof(GpuObjectInstance, effect_parameters2) == 144);

struct ScenePipelineSet {
    rhi::RenderResourceHandle opaque;
    rhi::RenderResourceHandle alpha_tested;
    rhi::RenderResourceHandle transparent;
    rhi::RenderResourceHandle additive;
    rhi::RenderResourceHandle premultiplied;
    rhi::RenderResourceHandle opaque_two_sided;
    rhi::RenderResourceHandle alpha_tested_two_sided;
    rhi::RenderResourceHandle transparent_two_sided;
    rhi::RenderResourceHandle additive_two_sided;
    rhi::RenderResourceHandle premultiplied_two_sided;

    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle for_layer(RenderLayer layer,
                                                      bool two_sided) const noexcept;
};

struct SceneRenderConfig {
    std::uint32_t maximum_instances_per_frame = 65'536;
    std::uint32_t maximum_skin_matrices_per_frame = 65'536;
    std::uint32_t maximum_morph_weights_per_frame = 1'048'576;
    std::uint32_t buffered_frames = 3;

    [[nodiscard]] core::Status validate() const;
};

struct SceneRenderStats {
    RenderSceneStats scene;
    std::uint32_t submitted_instances = 0;
    std::uint32_t draw_calls = 0;
    std::uint32_t dropped_instances = 0;
    std::uint32_t dropped_skinned_instances = 0;
    std::uint32_t submitted_skin_palettes = 0;
    std::uint32_t submitted_skin_matrices = 0;
    std::uint64_t uploaded_instance_bytes = 0;
    std::uint64_t uploaded_skin_matrix_bytes = 0;
    std::uint64_t uploaded_morph_weight_bytes = 0;
    std::uint64_t instance_buffer_bytes = 0;
    std::uint64_t skin_matrix_buffer_bytes = 0;
    std::uint64_t morph_weight_buffer_bytes = 0;
};

struct SceneDrawCommands {
    std::vector<rhi::RenderDrawCommand> opaque_and_cutout;
    std::vector<rhi::RenderDrawCommand> transparent;
    std::vector<RenderLightInstance> lights;
    SceneRenderStats stats;
};

class SceneRenderSystem {
  public:
    SceneRenderSystem(rhi::IRenderDevice& device, MeshManager& meshes,
                      MaterialRuntimeCache& materials, MaterialRuntimeHandle fallback_material,
                      ScenePipelineSet pipelines, core::PrototypeId pipeline_material);
    ~SceneRenderSystem();

    SceneRenderSystem(const SceneRenderSystem&) = delete;
    SceneRenderSystem& operator=(const SceneRenderSystem&) = delete;

    [[nodiscard]] core::Status initialize(SceneRenderConfig config = {});
    [[nodiscard]] core::Result<SceneDrawCommands>
    build_draw_commands(const RenderScene& scene, const RenderCamera& camera,
                        float simulation_alpha, SceneDrawCommands scratch = {});
    [[nodiscard]] core::Status set_pipelines(ScenePipelineSet pipelines) noexcept;
    [[nodiscard]] core::Status shutdown();
    [[nodiscard]] const SceneRenderStats& stats() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle instance_buffer() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle skin_matrix_buffer() const noexcept;
    [[nodiscard]] rhi::RenderResourceHandle morph_weight_buffer() const noexcept;

  private:
    struct UploadedSkinPalette {
        RenderSkinPaletteId id;
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };

    rhi::IRenderDevice* device_ = nullptr;
    MeshManager* meshes_ = nullptr;
    MaterialRuntimeCache* materials_ = nullptr;
    MaterialRuntimeHandle fallback_material_;
    ScenePipelineSet pipelines_{};
    core::PrototypeId pipeline_material_;
    SceneRenderConfig config_{};
    rhi::RenderResourceHandle instance_buffer_;
    rhi::RenderResourceHandle skin_matrix_buffer_;
    rhi::RenderResourceHandle morph_weight_buffer_;
    std::vector<GpuObjectInstance> instance_scratch_;
    std::vector<math::Mat4f> skin_matrix_scratch_;
    std::vector<float> morph_weight_scratch_;
    std::vector<UploadedSkinPalette> uploaded_skin_palettes_;
    std::uint64_t frame_number_ = 0;
    SceneRenderStats stats_{};
};

} // namespace heartstead::renderer
