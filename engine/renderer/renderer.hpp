#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/cpu_timing.hpp"
#include "engine/renderer/assets/mesh_manager.hpp"
#include "engine/renderer/assets/sampler_cache.hpp"
#include "engine/renderer/assets/shader_manager.hpp"
#include "engine/renderer/assets/surface_texture_array.hpp"
#include "engine/renderer/assets/texture_manager.hpp"
#include "engine/renderer/chunks/chunk_render_system.hpp"
#include "engine/renderer/debug/debug_renderer.hpp"
#include "engine/renderer/environment/environment_lighting.hpp"
#include "engine/renderer/environment/sky_renderer.hpp"
#include "engine/renderer/frame/frame_builder.hpp"
#include "engine/renderer/lighting/cascaded_shadows.hpp"
#include "engine/renderer/lighting/clustered_lighting.hpp"
#include "engine/renderer/materials/material_runtime_cache.hpp"
#include "engine/renderer/materials/pipeline_cache.hpp"
#include "engine/renderer/materials/terrain_material_assets.hpp"
#include "engine/renderer/memory/streaming_residency.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/renderer/quality/renderer_quality.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/renderer_stats.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/scene/render_scene.hpp"
#include "engine/renderer/scene/scene_render_system.hpp"
#include "engine/renderer/terrain/far_terrain_renderer.hpp"
#include "engine/renderer/terrain/far_terrain_world_surface.hpp"
#include "engine/renderer/ui/ui_renderer.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/world_state.hpp"

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
    std::vector<std::uint32_t> far_terrain_vertex_spirv;
    std::vector<std::uint32_t> terrain_fragment_spirv;
    std::vector<std::uint32_t> low_terrain_fragment_spirv;
    std::vector<std::uint32_t> static_mesh_vertex_spirv;
    std::vector<std::uint32_t> static_mesh_fragment_spirv;
    std::vector<std::uint32_t> shadow_terrain_fragment_spirv;
    std::vector<std::uint32_t> shadow_static_fragment_spirv;
    std::vector<std::uint32_t> debug_vertex_spirv;
    std::vector<std::uint32_t> debug_fragment_spirv;
    std::vector<std::uint32_t> ui_vertex_spirv;
    std::vector<std::uint32_t> ui_fragment_spirv;
    std::vector<std::uint8_t> ui_font_bytes;
    // Resolves the linear scene target to the display format. Only required by the linear HDR
    // frame graph; initialization fails rather than rendering an unresolved frame if that graph is
    // selected without them.
    std::vector<std::uint32_t> tone_map_vertex_spirv;
    std::vector<std::uint32_t> tone_map_fragment_spirv;
    std::vector<std::uint32_t> ssao_fragment_spirv;
    std::vector<std::uint32_t> ao_composite_fragment_spirv;
    std::vector<std::uint32_t> fxaa_fragment_spirv;
    std::vector<std::uint32_t> bloom_fragment_spirv;
    const world::VoxelPalette* voxel_palette = nullptr;
    materials::TerrainMaterialAssetSet terrain_material_assets;
    ChunkRenderConfig chunk_config{};
    ChunkGpuCacheConfig chunk_gpu_cache_config{};
    FarTerrainRendererConfig far_terrain_config{};
    MeshManagerConfig mesh_manager_config{};
    SceneRenderConfig scene_render_config{};
    ClusteredLightingConfig clustered_lighting_config{};
    DirectionalShadowConfig directional_shadow_config{};
    DebugRendererConfig debug_renderer_config{};
    UiRendererConfig ui_renderer_config{};
    StreamingResidencyConfig streaming_residency_config{};
    ResidencyLoadFunction streaming_residency_loader;
    rhi::ClearColor clear_color{0.055F, 0.09F, 0.14F, 1.0F};
    rhi::RenderEnvironmentData environment{};
    rhi::RenderExposureSettings exposure{};
    bool development_shader_hot_reload = false;
    std::optional<RendererQualityPreset> quality_preset;
};

struct RendererFallbackResources {
    RenderMeshHandle error_mesh;
    MaterialRuntimeHandle error_material;
    TextureHandle error_texture;
    TextureHandle white_texture;
    TextureHandle black_texture;
    TextureHandle normal_texture;

    [[nodiscard]] bool is_valid() const noexcept {
        return error_mesh.is_valid() && error_material.is_valid() && error_texture.is_valid() &&
               white_texture.is_valid() && black_texture.is_valid() && normal_texture.is_valid();
    }
};

struct ModelRenderMaterialBinding {
    MaterialRuntimeHandle material;
    RenderLayer layer = RenderLayer::opaque;
    RenderObjectFlags flags = RenderObjectFlags::none;
};

class Renderer {
  public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    [[nodiscard]] core::Status initialize(RendererInitDesc desc);
    [[nodiscard]] core::Status wait_idle();
    [[nodiscard]] core::Status clear_session_resources();
    [[nodiscard]] core::Status shutdown();
    // Clears chunk-pipeline counters and latency samples without disturbing resident or queued
    // work. This is intended for benchmark warm-up boundaries.
    void reset_chunk_performance_stats() noexcept;

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
    // Exposure and tone curve for the resolve pass. Validated at the boundary, so an out-of-range
    // or non-finite value is rejected here rather than becoming a NaN frame.
    [[nodiscard]] core::Status set_exposure(rhi::RenderExposureSettings exposure);
    [[nodiscard]] core::Status set_lighting_debug_view(LightingDebugView view);
    // Replaces the application-owned UI preview layer. The source is tightly packed RGBA8 and is
    // resampled to the UI atlas without changing authoritative renderer/session resources.
    [[nodiscard]] core::Status set_ui_preview_image(rhi::RenderExtent source_extent,
                                                    std::span<const std::uint8_t> rgba8);
    [[nodiscard]] core::Status clear_ui_preview_image();
    [[nodiscard]] rhi::RenderExposureSettings exposure() const noexcept;
    [[nodiscard]] const rhi::RenderEnvironmentData& environment() const noexcept;
    [[nodiscard]] const RendererQualitySettings& quality_settings() const noexcept;
    void set_voxel_fluid_stats(const world::ChunkFluidSystemStats& fluids) noexcept;
    void set_voxel_lighting_stats(const world::ChunkLightSystemStats& lighting) noexcept;
    void set_particle_stats(const ParticleSystemStats& particles, double presentation_ms,
                            std::uint32_t material_groups,
                            std::uint64_t presentation_dropped = 0) noexcept;
    void set_ui_widget_stats(double layout_ms, double paint_ms,
                             std::uint32_t widget_count) noexcept;
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
    [[nodiscard]] core::Result<std::uint32_t>
    create_surface_texture(std::string id, std::uint32_t width, std::uint32_t height,
                           std::span<const std::uint8_t> rgba8);
    [[nodiscard]] core::Result<MaterialRuntimeHandle>
    create_surface_material(MaterialRuntimeDesc desc);
    [[nodiscard]] core::Result<std::vector<ModelRenderMaterialBinding>>
    create_model_materials(std::string_view asset_id, const assets::ModelAsset& model);
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
    [[nodiscard]] const ClusteredLightingStats& lighting_stats() const noexcept;
    [[nodiscard]] FrameImageQualitySettings image_quality_settings() const noexcept;
    [[nodiscard]] RendererFallbackResources fallback_resources() const noexcept;
    [[nodiscard]] RenderMeshHandle fallback_mesh() const noexcept;
    [[nodiscard]] MaterialRuntimeHandle fallback_material() const noexcept;
    [[nodiscard]] std::optional<MaterialRuntimeDesc>
    describe_material(MaterialRuntimeHandle handle) const noexcept;
    [[nodiscard]] std::optional<MaterialRuntimeDesc>
    describe_voxel_material(std::uint16_t voxel_type) const noexcept;
    [[nodiscard]] std::optional<TextureView> describe_terrain_texture() const noexcept;
    [[nodiscard]] std::optional<TextureView> describe_surface_texture() const noexcept;
    [[nodiscard]] DebugRenderer* debug_renderer() noexcept;
    [[nodiscard]] const DebugRenderer* debug_renderer() const noexcept;
    [[nodiscard]] std::span<const DebugTextLabelFrame> debug_text_labels() const noexcept;
    [[nodiscard]] UiRenderer* ui_renderer() noexcept;
    [[nodiscard]] const UiRenderer* ui_renderer() const noexcept;
    [[nodiscard]] rhi::IRenderDevice* device() noexcept;
    [[nodiscard]] const rhi::IRenderDevice* device() const noexcept;
    [[nodiscard]] StreamingResidencyManager* streaming_residency() noexcept;
    [[nodiscard]] const StreamingResidencyManager* streaming_residency() const noexcept;

  private:
    // Format of the graph resource world shading writes into. Pipelines must be created against
    // the format of the attachment they will be bound to.
    [[nodiscard]] rhi::RenderImageFormat scene_color_format() const noexcept;
    [[nodiscard]] core::Status create_sky_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                                   std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status
    create_terrain_pipeline(std::span<const std::uint32_t> vertex_spirv,
                            std::span<const std::uint32_t> fragment_spirv,
                            const world::VoxelPalette* voxel_palette,
                            const materials::TerrainMaterialAssetSet& material_assets);
    [[nodiscard]] core::Status
    create_far_terrain_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status
    create_scene_pipelines(std::span<const std::uint32_t> vertex_spirv,
                           std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status
    create_shadow_pipelines(std::span<const std::uint32_t> terrain_vertex_spirv,
                            std::span<const std::uint32_t> terrain_fragment_spirv,
                            std::span<const std::uint32_t> static_vertex_spirv,
                            std::span<const std::uint32_t> static_fragment_spirv);
    [[nodiscard]] core::Status bind_shadow_resources();
    [[nodiscard]] core::Status bind_scene_surface_resources();
    [[nodiscard]] core::Status bind_clustered_lighting_resources();
    [[nodiscard]] core::Status
    create_debug_pipelines(std::span<const std::uint32_t> vertex_spirv,
                           std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status create_ui_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                                  std::span<const std::uint32_t> fragment_spirv,
                                                  std::span<const std::uint8_t> font_bytes,
                                                  std::uint16_t atlas_layers);
    [[nodiscard]] core::Status install_ui_atlas(TextureUploadDesc desc);
    // Builds the material that resolves the linear scene target to the display image. Only needed
    // by the linear HDR graph, so it is skipped when no tone map SPIR-V was supplied.
    [[nodiscard]] core::Status
    create_tone_map_pipeline(std::span<const std::uint32_t> vertex_spirv,
                             std::span<const std::uint32_t> fragment_spirv);
    [[nodiscard]] core::Status
    create_image_quality_pipelines(std::span<const std::uint32_t> vertex_spirv,
                                   std::span<const std::uint32_t> ssao_fragment_spirv,
                                   std::span<const std::uint32_t> ao_fragment_spirv,
                                   std::span<const std::uint32_t> fxaa_fragment_spirv,
                                   std::span<const std::uint32_t> bloom_fragment_spirv);
    void update_frontend_stats(std::size_t loaded_chunk_count) noexcept;
    void update_backend_stats(const rhi::RenderFrameStats& frame) noexcept;
    std::unique_ptr<rhi::IRenderDevice> device_;
    rhi::RenderResourceHandle sky_pipeline_{};
    GraphicsPipelineKey sky_pipeline_key_{};
    TerrainPipelineSet terrain_pipelines_{};
    std::array<GraphicsPipelineKey, 4> terrain_pipeline_keys_{};
    rhi::RenderResourceHandle far_terrain_pipeline_{};
    GraphicsPipelineKey far_terrain_pipeline_key_{};
    ScenePipelineSet scene_pipelines_{};
    std::array<GraphicsPipelineKey, 10> scene_pipeline_keys_{};
    std::array<rhi::RenderResourceHandle, 4> shadow_pipelines_{};
    std::array<GraphicsPipelineKey, 4> shadow_pipeline_keys_{};
    DebugPipelineSet debug_pipelines_{};
    std::array<GraphicsPipelineKey, 2> debug_pipeline_keys_{};
    rhi::RenderResourceHandle ui_pipeline_{};
    GraphicsPipelineKey ui_pipeline_key_{};
    // Invalid when no tone map SPIR-V was supplied, which is also what makes the linear HDR graph
    // unavailable.
    rhi::RenderResourceHandle tone_map_pipeline_{};
    std::array<rhi::RenderResourceHandle, 4> image_quality_pipelines_{};
    std::array<GraphicsPipelineKey, 4> image_quality_pipeline_keys_{};
    GraphicsPipelineKey tone_map_pipeline_key_{};
    ShaderProgramHandle terrain_shader_program_;
    ShaderProgramHandle far_terrain_shader_program_;
    std::vector<std::uint32_t> far_terrain_vertex_spirv_;
    ShaderProgramHandle sky_shader_program_;
    ShaderProgramHandle scene_shader_program_;
    ShaderProgramHandle terrain_shadow_shader_program_;
    ShaderProgramHandle static_shadow_shader_program_;
    ShaderProgramHandle debug_shader_program_;
    ShaderProgramHandle ui_shader_program_;
    ShaderProgramHandle tone_map_shader_program_;
    std::array<ShaderProgramHandle, 4> image_quality_shader_programs_{};
    TextureHandle terrain_texture_array_;
    TextureHandle terrain_normal_texture_array_;
    TextureHandle terrain_surface_texture_array_;
    std::unique_ptr<SurfaceTextureArray> surface_texture_array_;
    std::unique_ptr<SurfaceTextureArray> surface_data_texture_array_;
    TextureHandle ui_texture_atlas_;
    std::vector<std::byte> ui_atlas_rgba8_;
    std::uint32_t ui_atlas_width_ = 0;
    std::uint32_t ui_atlas_height_ = 0;
    std::uint16_t ui_atlas_layers_ = 0;
    std::uint64_t ui_atlas_revision_ = 0;
    MaterialRuntimeHandle fallback_material_;
    rhi::RenderResourceHandle terrain_sampler_;
    rhi::RenderResourceHandle ui_sampler_;
    rhi::RenderResourceHandle surface_sampler_;
    std::unique_ptr<ShaderManager> shader_manager_;
    std::unique_ptr<SamplerCache> sampler_cache_;
    std::unique_ptr<TextureManager> texture_manager_;
    std::unique_ptr<MaterialRuntimeCache> material_cache_;
    std::unique_ptr<PipelineCache> pipeline_cache_;
    std::unique_ptr<MeshManager> mesh_manager_;
    std::unique_ptr<StreamingResidencyManager> streaming_residency_;
    std::unique_ptr<ChunkGpuCache> chunk_cache_;
    std::unique_ptr<ChunkRenderSystem> chunk_system_;
    std::unique_ptr<FarTerrainRenderer> far_terrain_renderer_;
    std::unique_ptr<SkyRenderer> sky_renderer_;
    std::unique_ptr<SceneRenderSystem> scene_render_system_;
    std::unique_ptr<ClusteredLightingSystem> clustered_lighting_;
    std::unique_ptr<CascadedShadowSystem> cascaded_shadows_;
    std::unique_ptr<EnvironmentLighting> environment_lighting_;
    std::unique_ptr<DebugRenderer> debug_renderer_;
    std::unique_ptr<UiRenderer> ui_renderer_;
    std::shared_ptr<const UiFont> ui_font_;
    std::unique_ptr<FrameBuilder> frame_builder_;
    RenderScene scene_;
    profiling::CpuTimingRecorder cpu_timings_{};
    std::vector<rhi::RenderDrawCommand> chunk_draw_scratch_;
    std::vector<rhi::RenderDrawCommand> far_terrain_draw_scratch_;
    RenderCommandLists draw_command_scratch_;
    SceneDrawCommands scene_draw_scratch_;
    DebugFrameCommands debug_frame_scratch_;
    UiFrameCommands ui_frame_scratch_;
    std::vector<DebugTextLabelFrame> debug_text_labels_;
    FarTerrainWorldSurfaceCache far_terrain_world_surface_;
    rhi::RenderEnvironmentData environment_{};
    rhi::RenderEnvironmentData default_environment_{};
    rhi::RenderExposureSettings default_exposure_{};
    rhi::ClearColor default_clear_color_{};
    RendererStats stats_{};
    RendererQualitySettings quality_settings_{};
    std::chrono::steady_clock::time_point frame_started_at_{};
    bool frame_timing_active_ = false;
    std::thread::id owner_thread_{};
};

} // namespace heartstead::renderer
