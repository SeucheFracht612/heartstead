#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/cpu_timing.hpp"
#include "engine/renderer/assets/mesh_manager.hpp"
#include "engine/renderer/assets/sampler_cache.hpp"
#include "engine/renderer/assets/shader_manager.hpp"
#include "engine/renderer/assets/texture_manager.hpp"
#include "engine/renderer/chunks/chunk_render_system.hpp"
#include "engine/renderer/debug/debug_renderer.hpp"
#include "engine/renderer/environment/sky_renderer.hpp"
#include "engine/renderer/frame/frame_builder.hpp"
#include "engine/renderer/materials/material_runtime_cache.hpp"
#include "engine/renderer/materials/pipeline_cache.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/renderer_stats.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/scene/render_scene.hpp"
#include "engine/renderer/scene/scene_render_system.hpp"
#include "engine/renderer/ui/ui_renderer.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/world_state.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace heartstead::renderer {

enum class ChunkRenderUpdateKind : std::uint8_t {
    loaded,
    evicted,
};

struct ChunkRenderUpdate {
    ChunkRenderUpdateKind kind = ChunkRenderUpdateKind::loaded;
    world::ChunkStreamLoadReport load;
    world::ChunkIdentity identity;
};

struct RenderFrameInput {
    RenderCamera camera;
    float simulation_alpha = 1.0F;
    float delta_seconds = 0.0F;
};

struct RenderFrameResult {
    rhi::RenderFrameStats frame;
    RendererStats renderer;
};

struct RendererInitDesc {
    std::unique_ptr<rhi::IRenderDevice> device;
    std::vector<std::uint32_t> sky_vertex_spirv;
    std::vector<std::uint32_t> sky_fragment_spirv;
    std::vector<std::uint32_t> terrain_vertex_spirv;
    std::vector<std::uint32_t> terrain_fragment_spirv;
    std::vector<std::uint32_t> static_mesh_vertex_spirv;
    std::vector<std::uint32_t> static_mesh_fragment_spirv;
    std::vector<std::uint32_t> debug_vertex_spirv;
    std::vector<std::uint32_t> debug_fragment_spirv;
    std::vector<std::uint32_t> ui_vertex_spirv;
    std::vector<std::uint32_t> ui_fragment_spirv;
    const world::VoxelPalette* voxel_palette = nullptr;
    ChunkRenderConfig chunk_config{};
    ChunkGpuCacheConfig chunk_gpu_cache_config{};
    MeshManagerConfig mesh_manager_config{};
    SceneRenderConfig scene_render_config{};
    DebugRendererConfig debug_renderer_config{};
    UiRendererConfig ui_renderer_config{};
    rhi::ClearColor clear_color{0.055F, 0.09F, 0.14F, 1.0F};
    rhi::RenderEnvironmentData environment{};
    bool development_shader_hot_reload = false;
};

class Renderer {
  public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] core::Status initialize(RendererInitDesc desc);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] core::Status synchronize_chunks(world::WorldState& world,
                                                  const RenderCamera& camera);
    [[nodiscard]] core::Status
    process_chunk_loads(std::span<const world::ChunkStreamLoadReport> loads);
    [[nodiscard]] core::Status
    process_chunk_evictions(std::span<const world::ChunkIdentity> evictions);
    [[nodiscard]] core::Status
    process_chunk_evictions(const world::ChunkStreamEvictionReport& eviction);
    [[nodiscard]] core::Status
    process_world_render_updates(std::span<const ChunkRenderUpdate> updates);

    [[nodiscard]] core::Result<rhi::RenderFrameStats>
    render(const RenderCamera& camera, float simulation_alpha = 1.0F, float delta_seconds = 0.0F);
    [[nodiscard]] core::Result<RenderFrameResult> render_frame(const RenderFrameInput& input);
    [[nodiscard]] core::Status resize(rhi::RenderExtent extent);
    [[nodiscard]] core::Status set_environment(rhi::RenderEnvironmentData environment);
    void set_voxel_fluid_stats(const world::ChunkFluidSystemStats& fluids) noexcept;
    void set_voxel_lighting_stats(const world::ChunkLightSystemStats& lighting) noexcept;
    void set_particle_stats(const ParticleSystemStats& particles, double presentation_ms,
                            std::uint32_t material_groups,
                            std::uint64_t presentation_dropped = 0) noexcept;
    [[nodiscard]] RenderObjectId reserve_object_id();
    [[nodiscard]] RenderLightId reserve_light_id();
    [[nodiscard]] RenderSkinPaletteId reserve_skin_palette_id();
    [[nodiscard]] core::Result<RenderObjectId> create_object(RenderObjectProxy object);
    [[nodiscard]] core::Result<RenderLightId> create_light(RenderLightProxy light);
    [[nodiscard]] core::Result<RenderSkinPaletteId>
    create_skin_palette(RenderSkinPaletteProxy palette);
    [[nodiscard]] core::Status apply_scene_updates(std::span<const RenderSceneUpdate> updates);
    [[nodiscard]] core::Result<RenderMeshHandle>
    create_static_mesh(const StaticMeshUploadDesc& desc);
    [[nodiscard]] core::Result<RenderMeshHandle>
    create_model_primitive(std::string id, const assets::ModelAsset& model,
                           std::uint32_t primitive_index);
    [[nodiscard]] core::Status release_static_mesh(RenderMeshHandle handle);
    [[nodiscard]] core::Status
    reload_terrain_shaders(std::span<const std::uint32_t> vertex_spirv,
                           std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status
    reload_static_mesh_shaders(std::span<const std::uint32_t> vertex_spirv,
                               std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status reload_debug_shaders(std::span<const std::uint32_t> vertex_spirv,
                                                    std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status reload_ui_shaders(std::span<const std::uint32_t> vertex_spirv,
                                                 std::span<const std::uint32_t> fragment_spirv);

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] bool is_owner_thread() const noexcept;
    [[nodiscard]] const ChunkRenderStats& chunk_stats() const noexcept;
    [[nodiscard]] const RendererStats& stats() const noexcept;
    [[nodiscard]] const SceneRenderStats& scene_stats() const noexcept;
    [[nodiscard]] RenderMeshHandle fallback_mesh() const noexcept;
    [[nodiscard]] DebugRenderer* debug_renderer() noexcept;
    [[nodiscard]] const DebugRenderer* debug_renderer() const noexcept;
    [[nodiscard]] std::span<const DebugTextLabelFrame> debug_text_labels() const noexcept;
    [[nodiscard]] UiRenderer* ui_renderer() noexcept;
    [[nodiscard]] const UiRenderer* ui_renderer() const noexcept;
    [[nodiscard]] rhi::IRenderDevice* device() noexcept;
    [[nodiscard]] const rhi::IRenderDevice* device() const noexcept;

  private:
    [[nodiscard]] core::Status create_sky_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                                   std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status
    create_terrain_pipeline(std::span<const std::uint32_t> vertex_spirv,
                            std::span<const std::uint32_t> fragment_spirv,
                            const world::VoxelPalette* voxel_palette);
    [[nodiscard]] core::Status
    create_scene_pipelines(std::span<const std::uint32_t> vertex_spirv,
                           std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status
    create_debug_pipelines(std::span<const std::uint32_t> vertex_spirv,
                           std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status create_ui_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                                  std::span<const std::uint32_t> fragment_spirv);
    void update_frontend_stats(std::size_t loaded_chunk_count) noexcept;
    void update_backend_stats(const rhi::RenderFrameStats& frame) noexcept;

    std::unique_ptr<rhi::IRenderDevice> device_;
    rhi::RenderResourceHandle sky_pipeline_{};
    GraphicsPipelineKey sky_pipeline_key_{};
    TerrainPipelineSet terrain_pipelines_{};
    std::array<GraphicsPipelineKey, 4> terrain_pipeline_keys_{};
    ScenePipelineSet scene_pipelines_{};
    std::array<GraphicsPipelineKey, 3> scene_pipeline_keys_{};
    DebugPipelineSet debug_pipelines_{};
    std::array<GraphicsPipelineKey, 2> debug_pipeline_keys_{};
    rhi::RenderResourceHandle ui_pipeline_{};
    GraphicsPipelineKey ui_pipeline_key_{};
    ShaderProgramHandle terrain_shader_program_;
    ShaderProgramHandle sky_shader_program_;
    ShaderProgramHandle scene_shader_program_;
    ShaderProgramHandle debug_shader_program_;
    ShaderProgramHandle ui_shader_program_;
    TextureHandle terrain_texture_array_;
    TextureHandle ui_texture_atlas_;
    rhi::RenderResourceHandle terrain_sampler_;
    std::unique_ptr<ShaderManager> shader_manager_;
    std::unique_ptr<SamplerCache> sampler_cache_;
    std::unique_ptr<TextureManager> texture_manager_;
    std::unique_ptr<MaterialRuntimeCache> material_cache_;
    std::unique_ptr<PipelineCache> pipeline_cache_;
    std::unique_ptr<MeshManager> mesh_manager_;
    std::unique_ptr<ChunkGpuCache> chunk_cache_;
    std::unique_ptr<ChunkRenderSystem> chunk_system_;
    std::unique_ptr<SkyRenderer> sky_renderer_;
    std::unique_ptr<SceneRenderSystem> scene_render_system_;
    std::unique_ptr<DebugRenderer> debug_renderer_;
    std::unique_ptr<UiRenderer> ui_renderer_;
    std::unique_ptr<FrameBuilder> frame_builder_;
    RenderScene scene_;
    profiling::CpuTimingRecorder cpu_timings_{};
    std::vector<rhi::RenderDrawCommand> chunk_draw_scratch_;
    RenderCommandLists draw_command_scratch_;
    SceneDrawCommands scene_draw_scratch_;
    DebugFrameCommands debug_frame_scratch_;
    UiFrameCommands ui_frame_scratch_;
    std::vector<DebugTextLabelFrame> debug_text_labels_;
    rhi::RenderEnvironmentData environment_{};
    RendererStats stats_{};
    std::chrono::steady_clock::time_point frame_started_at_{};
    bool frame_timing_active_ = false;
    std::thread::id owner_thread_{};
};

} // namespace heartstead::renderer
