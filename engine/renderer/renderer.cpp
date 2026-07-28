#include "engine/renderer/renderer.hpp"

#include "engine/renderer/terrain/gpu_chunk_vertex.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>

namespace heartstead::renderer {

namespace {

const ChunkRenderStats empty_chunk_stats{};

[[nodiscard]] ShaderProgramDesc
make_sky_shader_program(std::span<const std::uint32_t> vertex_spirv,
                        std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "sky_gradient";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "sky.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "sky.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuSkyVertex);
    for (const auto& attribute : gpu_sky_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_sky_vertex_v1", "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] std::vector<std::byte> make_terrain_tile(std::array<std::uint8_t, 3> color,
                                                       bool error = false) {
    constexpr std::uint32_t tile_size = 16;
    std::vector<std::byte> pixels(tile_size * tile_size * 4U);
    for (std::uint32_t y = 0; y < tile_size; ++y) {
        for (std::uint32_t x = 0; x < tile_size; ++x) {
            const auto offset = static_cast<std::size_t>(y * tile_size + x) * 4U;
            const bool alternate = ((x / 4U) + (y / 4U)) % 2U != 0;
            if (error) {
                pixels[offset] = static_cast<std::byte>(alternate ? 20U : 255U);
                pixels[offset + 1] = static_cast<std::byte>(0U);
                pixels[offset + 2] = static_cast<std::byte>(alternate ? 20U : 255U);
            } else {
                const auto scale = alternate ? 0.82F : 1.0F;
                for (std::size_t channel = 0; channel < color.size(); ++channel) {
                    pixels[offset + channel] = static_cast<std::byte>(
                        static_cast<std::uint8_t>(static_cast<float>(color[channel]) * scale));
                }
            }
            pixels[offset + 3] = static_cast<std::byte>(255U);
        }
    }
    return pixels;
}

[[nodiscard]] ShaderProgramDesc
make_terrain_shader_program(std::span<const std::uint32_t> vertex_spirv,
                            std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "terrain";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "terrain.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "terrain.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(terrain::GpuChunkVertex);
    for (const auto& attribute : terrain::gpu_chunk_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.descriptors = {
        {"terrain_textures", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
        {"voxel_materials", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_chunk_vertex_v1", "gpu_voxel_material_v1",
                                   "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] ShaderProgramDesc
make_static_mesh_shader_program(std::span<const std::uint32_t> vertex_spirv,
                                std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "static_mesh";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "static_mesh.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "static_mesh.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuStaticMeshVertex);
    for (const auto& attribute : gpu_static_mesh_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.descriptors = {
        {"object_instances", rhi::RenderDescriptorKind::storage_buffer, 0, true,
         rhi::RenderShaderStageFlags::vertex},
    };
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_static_mesh_vertex_v2", "gpu_object_instance_v1",
                                   "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] ShaderProgramDesc
make_debug_shader_program(std::span<const std::uint32_t> vertex_spirv,
                          std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "debug_lines";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "debug_line.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "debug_line.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuDebugVertex);
    for (const auto& attribute : gpu_debug_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_debug_vertex_v1", "chunk_push_constants_v2"};
    return shader_program;
}

[[nodiscard]] ShaderProgramDesc
make_ui_shader_program(std::span<const std::uint32_t> vertex_spirv,
                       std::span<const std::uint32_t> fragment_spirv) {
    ShaderProgramDesc shader_program;
    shader_program.id = "ui";
    shader_program.stages = {
        {rhi::RenderShaderStage::vertex,
         "main",
         {vertex_spirv.begin(), vertex_spirv.end()},
         "ui.vert.spv"},
        {rhi::RenderShaderStage::fragment,
         "main",
         {fragment_spirv.begin(), fragment_spirv.end()},
         "ui.frag.spv"},
    };
    shader_program.interface.vertex_stride = sizeof(GpuUiVertex);
    for (const auto& attribute : gpu_ui_vertex_attributes) {
        shader_program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    shader_program.interface.descriptors = {
        {"ui_atlas", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    shader_program.interface.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    shader_program.dependencies = {"gpu_ui_vertex_v1", "ui_atlas_v1"};
    return shader_program;
}

[[nodiscard]] std::array<std::uint8_t, 7> fallback_glyph_rows(unsigned char character) {
    if (character >= 'a' && character <= 'z') {
        character = static_cast<unsigned char>(character - 'a' + 'A');
    }
    switch (character) {
    case '0':
        return {14, 17, 19, 21, 25, 17, 14};
    case '1':
        return {4, 12, 4, 4, 4, 4, 14};
    case '2':
        return {14, 17, 1, 2, 4, 8, 31};
    case '3':
        return {30, 1, 1, 14, 1, 1, 30};
    case '4':
        return {2, 6, 10, 18, 31, 2, 2};
    case '5':
        return {31, 16, 16, 30, 1, 1, 30};
    case '6':
        return {14, 16, 16, 30, 17, 17, 14};
    case '7':
        return {31, 1, 2, 4, 8, 8, 8};
    case '8':
        return {14, 17, 17, 14, 17, 17, 14};
    case '9':
        return {14, 17, 17, 15, 1, 1, 14};
    case 'A':
        return {14, 17, 17, 31, 17, 17, 17};
    case 'B':
        return {30, 17, 17, 30, 17, 17, 30};
    case 'C':
        return {15, 16, 16, 16, 16, 16, 15};
    case 'D':
        return {30, 17, 17, 17, 17, 17, 30};
    case 'E':
        return {31, 16, 16, 30, 16, 16, 31};
    case 'F':
        return {31, 16, 16, 30, 16, 16, 16};
    case 'G':
        return {15, 16, 16, 23, 17, 17, 14};
    case 'H':
        return {17, 17, 17, 31, 17, 17, 17};
    case 'I':
        return {14, 4, 4, 4, 4, 4, 14};
    case 'J':
        return {7, 2, 2, 2, 18, 18, 12};
    case 'K':
        return {17, 18, 20, 24, 20, 18, 17};
    case 'L':
        return {16, 16, 16, 16, 16, 16, 31};
    case 'M':
        return {17, 27, 21, 21, 17, 17, 17};
    case 'N':
        return {17, 25, 21, 19, 17, 17, 17};
    case 'O':
        return {14, 17, 17, 17, 17, 17, 14};
    case 'P':
        return {30, 17, 17, 30, 16, 16, 16};
    case 'Q':
        return {14, 17, 17, 17, 21, 18, 13};
    case 'R':
        return {30, 17, 17, 30, 20, 18, 17};
    case 'S':
        return {15, 16, 16, 14, 1, 1, 30};
    case 'T':
        return {31, 4, 4, 4, 4, 4, 4};
    case 'U':
        return {17, 17, 17, 17, 17, 17, 14};
    case 'V':
        return {17, 17, 17, 17, 10, 10, 4};
    case 'W':
        return {17, 17, 17, 21, 21, 27, 17};
    case 'X':
        return {17, 17, 10, 4, 10, 17, 17};
    case 'Y':
        return {17, 17, 10, 4, 4, 4, 4};
    case 'Z':
        return {31, 1, 2, 4, 8, 16, 31};
    case '-':
        return {0, 0, 0, 31, 0, 0, 0};
    case '_':
        return {0, 0, 0, 0, 0, 0, 31};
    case '.':
        return {0, 0, 0, 0, 0, 12, 12};
    case ':':
        return {0, 12, 12, 0, 12, 12, 0};
    case '/':
        return {1, 2, 2, 4, 8, 8, 16};
    case '?':
        return {14, 17, 1, 2, 4, 0, 4};
    default:
        return {};
    }
}

[[nodiscard]] std::vector<std::byte> make_ui_atlas() {
    constexpr std::size_t width = 128;
    constexpr std::size_t height = 64;
    constexpr std::size_t layer_size = width * height * 4U;
    std::vector<std::byte> pixels(layer_size * 2U, std::byte{0});
    std::fill_n(pixels.begin(), static_cast<std::ptrdiff_t>(layer_size), std::byte{0xff});
    for (std::uint32_t character = 0; character < 128U; ++character) {
        const auto rows = fallback_glyph_rows(static_cast<unsigned char>(character));
        const auto cell_x = (character % 16U) * 8U;
        const auto cell_y = (character / 16U) * 8U;
        for (std::uint32_t row = 0; row < rows.size(); ++row) {
            for (std::uint32_t column = 0; column < 5U; ++column) {
                if ((rows[row] & (1U << (4U - column))) == 0U) {
                    continue;
                }
                const auto offset =
                    layer_size +
                    (static_cast<std::size_t>(cell_y + row) * width + cell_x + column + 1U) * 4U;
                pixels[offset] = std::byte{0xff};
                pixels[offset + 1U] = std::byte{0xff};
                pixels[offset + 2U] = std::byte{0xff};
                pixels[offset + 3U] = std::byte{0xff};
            }
        }
    }
    return pixels;
}

} // namespace

Renderer::~Renderer() {
    (void)shutdown();
}

core::Status Renderer::initialize(RendererInitDesc desc) {
    if (is_initialized()) {
        return core::Status::failure("renderer.already_initialized",
                                     "renderer cannot be initialized twice");
    }
    if (desc.device == nullptr) {
        return core::Status::failure("renderer.missing_device",
                                     "renderer initialization requires a render device");
    }
    owner_thread_ = std::this_thread::get_id();
    auto config_status = desc.chunk_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.chunk_gpu_cache_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.mesh_manager_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.scene_render_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.debug_renderer_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = desc.ui_renderer_config.validate();
    if (!config_status) {
        return config_status;
    }
    config_status = rhi::validate_render_environment(desc.environment);
    if (!config_status) {
        return config_status;
    }

    device_ = std::move(desc.device);
    shader_manager_ = std::make_unique<ShaderManager>(*device_, desc.development_shader_hot_reload);
    sampler_cache_ = std::make_unique<SamplerCache>(*device_);
    texture_manager_ = std::make_unique<TextureManager>(*device_);
    material_cache_ = std::make_unique<MaterialRuntimeCache>(*device_);
    pipeline_cache_ = std::make_unique<PipelineCache>(*device_, *shader_manager_);
    auto fallback_status = texture_manager_->initialize_fallbacks();
    if (!fallback_status) {
        const auto error = fallback_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    auto pipeline_status = create_sky_pipeline(desc.sky_vertex_spirv, desc.sky_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status = create_terrain_pipeline(desc.terrain_vertex_spirv,
                                              desc.terrain_fragment_spirv, desc.voxel_palette);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status =
        create_scene_pipelines(desc.static_mesh_vertex_spirv, desc.static_mesh_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status = create_debug_pipelines(desc.debug_vertex_spirv, desc.debug_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_status = create_ui_pipeline(desc.ui_vertex_spirv, desc.ui_fragment_spirv);
    if (!pipeline_status) {
        const auto error = pipeline_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    pipeline_cache_->seal();

    sky_renderer_ = std::make_unique<SkyRenderer>(*device_, sky_pipeline_);
    auto sky_status = sky_renderer_->initialize();
    if (!sky_status) {
        const auto error = sky_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    mesh_manager_ = std::make_unique<MeshManager>(*device_);
    auto mesh_status = mesh_manager_->initialize(desc.mesh_manager_config);
    if (!mesh_status) {
        const auto error = mesh_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    const auto scene_material = core::PrototypeId::parse("base:materials/static_instances");
    if (!scene_material) {
        (void)shutdown();
        return core::Status::failure("renderer.invalid_scene_material",
                                     "internal static-instance material id is invalid");
    }
    scene_render_system_ = std::make_unique<SceneRenderSystem>(
        *device_, *mesh_manager_, scene_pipelines_, scene_material.value());
    auto scene_status = scene_render_system_->initialize(desc.scene_render_config);
    if (!scene_status) {
        const auto error = scene_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    debug_renderer_ = std::make_unique<DebugRenderer>(*device_, debug_pipelines_);
    auto debug_status = debug_renderer_->initialize(desc.debug_renderer_config);
    if (!debug_status) {
        const auto error = debug_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    ui_renderer_ = std::make_unique<UiRenderer>(*device_, ui_pipeline_);
    auto ui_status = ui_renderer_->initialize(device_->current_extent(), desc.ui_renderer_config);
    if (!ui_status) {
        const auto error = ui_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }

    chunk_cache_ = std::make_unique<ChunkGpuCache>(*device_);
    auto cache_status = chunk_cache_->initialize(desc.chunk_gpu_cache_config);
    if (!cache_status) {
        const auto error = cache_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    chunk_system_ = std::make_unique<ChunkRenderSystem>(*chunk_cache_, terrain_pipelines_,
                                                        desc.voxel_palette, desc.chunk_config);
    auto chunk_system_status = chunk_system_->initialize();
    if (!chunk_system_status) {
        const auto error = chunk_system_status.error();
        (void)shutdown();
        return core::Status::failure(error.code, error.message);
    }
    frame_builder_ = std::make_unique<FrameBuilder>(device_->current_extent(), desc.clear_color);
    environment_ = desc.environment;
    return core::Status::ok();
}

core::Status Renderer::shutdown() {
    core::Status first_failure = core::Status::ok();
    const auto remember_failure = [&first_failure](core::Status status) {
        if (!status && first_failure) {
            first_failure = status;
        }
    };
    chunk_draw_scratch_.clear();
    draw_command_scratch_ = {};
    scene_draw_scratch_ = {};
    debug_frame_scratch_ = {};
    ui_frame_scratch_ = {};
    debug_text_labels_.clear();
    scene_.clear();
    frame_builder_.reset();
    if (sky_renderer_ != nullptr) {
        remember_failure(sky_renderer_->shutdown());
        sky_renderer_.reset();
    }
    if (ui_renderer_ != nullptr) {
        remember_failure(ui_renderer_->shutdown());
        ui_renderer_.reset();
    }
    if (debug_renderer_ != nullptr) {
        remember_failure(debug_renderer_->shutdown());
        debug_renderer_.reset();
    }
    if (scene_render_system_ != nullptr) {
        remember_failure(scene_render_system_->shutdown());
        scene_render_system_.reset();
    }
    chunk_system_.reset();
    if (chunk_cache_ != nullptr) {
        auto status = chunk_cache_->clear();
        if (!status) {
            first_failure = status;
        }
        chunk_cache_.reset();
    }
    if (mesh_manager_ != nullptr) {
        remember_failure(mesh_manager_->shutdown());
        mesh_manager_.reset();
    }
    if (pipeline_cache_ != nullptr) {
        remember_failure(pipeline_cache_->shutdown());
        pipeline_cache_.reset();
    }
    if (material_cache_ != nullptr) {
        remember_failure(material_cache_->shutdown());
        material_cache_.reset();
    }
    if (texture_manager_ != nullptr) {
        remember_failure(texture_manager_->shutdown());
        texture_manager_.reset();
    }
    if (sampler_cache_ != nullptr) {
        remember_failure(sampler_cache_->shutdown());
        sampler_cache_.reset();
    }
    if (shader_manager_ != nullptr) {
        remember_failure(shader_manager_->shutdown());
        shader_manager_.reset();
    }
    terrain_pipelines_ = {};
    sky_pipeline_ = {};
    sky_pipeline_key_ = {};
    terrain_pipeline_keys_ = {};
    scene_pipelines_ = {};
    scene_pipeline_keys_ = {};
    debug_pipelines_ = {};
    debug_pipeline_keys_ = {};
    ui_pipeline_ = {};
    ui_pipeline_key_ = {};
    terrain_shader_program_ = {};
    sky_shader_program_ = {};
    scene_shader_program_ = {};
    debug_shader_program_ = {};
    ui_shader_program_ = {};
    terrain_texture_array_ = {};
    ui_texture_atlas_ = {};
    terrain_sampler_ = {};
    environment_ = {};
    device_.reset();
    owner_thread_ = {};
    return first_failure;
}

core::Status Renderer::synchronize_chunks(world::WorldState& world, const RenderCamera& camera) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before chunk synchronization");
    }
    cpu_timings_.reset();
    frame_started_at_ = std::chrono::steady_clock::now();
    frame_timing_active_ = true;
    auto status = core::Status::ok();
    {
        profiling::ScopedCpuTimingZone synchronization_zone(
            cpu_timings_, profiling::CpuTimingZone::chunk_synchronization);
        status = chunk_system_->synchronize(world, camera);
    }
    update_frontend_stats(world.chunks().chunk_count());
    return status;
}

core::Status Renderer::process_chunk_loads(std::span<const world::ChunkStreamLoadReport> loads) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before processing chunk loads");
    }
    return chunk_system_->process_chunk_loads(loads);
}

core::Status Renderer::process_chunk_evictions(std::span<const world::ChunkIdentity> evictions) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure(
            "renderer.not_initialized",
            "renderer must be initialized before processing chunk evictions");
    }
    return chunk_system_->process_chunk_evictions(evictions);
}

core::Status Renderer::process_chunk_evictions(const world::ChunkStreamEvictionReport& eviction) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure(
            "renderer.not_initialized",
            "renderer must be initialized before processing chunk evictions");
    }
    return chunk_system_->process_chunk_evictions(eviction);
}

core::Status Renderer::process_world_render_updates(std::span<const ChunkRenderUpdate> updates) {
    if (chunk_system_ == nullptr) {
        return core::Status::failure(
            "renderer.not_initialized",
            "renderer must be initialized before processing world render updates");
    }
    for (const auto& update : updates) {
        auto status = core::Status::ok();
        switch (update.kind) {
        case ChunkRenderUpdateKind::loaded:
            status =
                process_chunk_loads(std::span<const world::ChunkStreamLoadReport>{&update.load, 1});
            break;
        case ChunkRenderUpdateKind::evicted:
            status =
                process_chunk_evictions(std::span<const world::ChunkIdentity>{&update.identity, 1});
            break;
        default:
            return core::Status::failure("renderer.invalid_world_render_update",
                                         "world render update kind is invalid");
        }
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Result<rhi::RenderFrameStats> Renderer::render(const RenderCamera& camera,
                                                     float simulation_alpha, float delta_seconds) {
    if (device_ == nullptr || chunk_system_ == nullptr || scene_render_system_ == nullptr ||
        sky_renderer_ == nullptr || debug_renderer_ == nullptr || ui_renderer_ == nullptr ||
        frame_builder_ == nullptr) {
        return core::Result<rhi::RenderFrameStats>::failure(
            "renderer.not_initialized", "renderer must be initialized before rendering");
    }
    if (!frame_timing_active_) {
        cpu_timings_.reset();
        frame_started_at_ = std::chrono::steady_clock::now();
        frame_timing_active_ = true;
    }
    ChunkDrawList draws;
    {
        profiling::ScopedCpuTimingZone extraction_zone(cpu_timings_,
                                                       profiling::CpuTimingZone::render_extraction);
        draws = chunk_system_->build_draw_list(camera, std::move(chunk_draw_scratch_));
    }
    chunk_draw_scratch_ = std::move(draws.draws);
    RenderCommandLists command_lists;
    auto sky_draw = sky_renderer_->build_draw();
    if (!sky_draw) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(sky_draw.error().code,
                                                            sky_draw.error().message);
    }
    command_lists.sky_draws.push_back(sky_draw.value());
    command_lists.opaque_terrain_draws = std::move(draw_command_scratch_.opaque_terrain_draws);
    command_lists.alpha_tested_terrain_draws =
        std::move(draw_command_scratch_.alpha_tested_terrain_draws);
    command_lists.transparent_terrain_draws =
        std::move(draw_command_scratch_.transparent_terrain_draws);
    debug_frame_scratch_.draws = std::move(draw_command_scratch_.debug_draws);
    ui_frame_scratch_.draws = std::move(draw_command_scratch_.ui_draws);
    command_lists.opaque_terrain_draws.clear();
    command_lists.alpha_tested_terrain_draws.clear();
    command_lists.transparent_terrain_draws.clear();
    for (auto& draw : chunk_draw_scratch_) {
        if (draw.pipeline == terrain_pipelines_.opaque) {
            command_lists.opaque_terrain_draws.push_back(draw);
        } else if (draw.pipeline == terrain_pipelines_.alpha_tested) {
            command_lists.alpha_tested_terrain_draws.push_back(draw);
        } else {
            command_lists.transparent_terrain_draws.push_back(draw);
        }
    }
    auto scene_draws = scene_render_system_->build_draw_commands(scene_, camera, simulation_alpha,
                                                                 std::move(scene_draw_scratch_));
    if (!scene_draws) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(scene_draws.error().code,
                                                            scene_draws.error().message);
    }
    command_lists.rich_instance_draws = std::move(scene_draws.value().opaque_and_cutout);
    auto& transparent_instances = scene_draws.value().transparent;
    command_lists.transparent_terrain_draws.insert(
        command_lists.transparent_terrain_draws.end(),
        std::make_move_iterator(transparent_instances.begin()),
        std::make_move_iterator(transparent_instances.end()));
    auto debug_frame =
        debug_renderer_->build_frame(camera, delta_seconds, std::move(debug_frame_scratch_));
    if (!debug_frame) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(debug_frame.error().code,
                                                            debug_frame.error().message);
    }
    command_lists.debug_draws = std::move(debug_frame.value().draws);
    debug_text_labels_ = std::move(debug_frame.value().text_labels);
    auto ui_frame = ui_renderer_->build_frame(std::move(ui_frame_scratch_));
    if (!ui_frame) {
        frame_timing_active_ = false;
        return core::Result<rhi::RenderFrameStats>::failure(ui_frame.error().code,
                                                            ui_frame.error().message);
    }
    command_lists.ui_draws = std::move(ui_frame.value().draws);
    auto frame = [&]() {
        profiling::ScopedCpuTimingZone command_zone(cpu_timings_,
                                                    profiling::CpuTimingZone::command_build);
        return frame_builder_->build(camera, std::move(command_lists), environment_);
    }();
    if (!frame) {
        return core::Result<rhi::RenderFrameStats>::failure(frame.error().code,
                                                            frame.error().message);
    }
    auto executed = device_->execute_frame(frame.value());
    draw_command_scratch_ = {};
    for (auto& pass : frame.value().pass_commands) {
        switch (pass.pass_index) {
        case 1:
            draw_command_scratch_.opaque_terrain_draws = std::move(pass.draws);
            break;
        case 2:
            draw_command_scratch_.alpha_tested_terrain_draws = std::move(pass.draws);
            break;
        case 3:
            draw_command_scratch_.rich_instance_draws = std::move(pass.draws);
            break;
        case 4:
            draw_command_scratch_.transparent_terrain_draws = std::move(pass.draws);
            break;
        case 5:
            draw_command_scratch_.debug_draws = std::move(pass.draws);
            break;
        case 6:
            draw_command_scratch_.ui_draws = std::move(pass.draws);
            break;
        default:
            break;
        }
    }
    if (!executed) {
        frame_timing_active_ = false;
        return executed;
    }
    const auto complete_frame_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - frame_started_at_)
                                       .count();
    cpu_timings_.add(profiling::CpuTimingZone::complete_frame, complete_frame_ms);
    stats_.cpu_frame_ms = cpu_timings_.milliseconds(profiling::CpuTimingZone::complete_frame);
    update_frontend_stats(stats_.loaded_chunks);
    update_backend_stats(executed.value());
    frame_timing_active_ = false;
    return executed;
}

core::Result<RenderFrameResult> Renderer::render_frame(const RenderFrameInput& input) {
    if (!std::isfinite(input.simulation_alpha) || input.simulation_alpha < 0.0F ||
        input.simulation_alpha > 1.0F || !std::isfinite(input.delta_seconds) ||
        input.delta_seconds < 0.0F) {
        return core::Result<RenderFrameResult>::failure(
            "renderer.invalid_frame_input",
            "frame interpolation must be in [0,1] and delta time must be finite and nonnegative");
    }
    auto rendered = render(input.camera, input.simulation_alpha, input.delta_seconds);
    if (!rendered) {
        return core::Result<RenderFrameResult>::failure(rendered.error().code,
                                                        rendered.error().message);
    }
    return core::Result<RenderFrameResult>::success({rendered.value(), stats_});
}

core::Status Renderer::resize(rhi::RenderExtent extent) {
    if (device_ == nullptr || frame_builder_ == nullptr || ui_renderer_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before resizing");
    }
    auto status = device_->resize(extent);
    if (!status) {
        return status;
    }
    status = frame_builder_->resize(extent);
    if (!status) {
        return status;
    }
    return ui_renderer_->resize(extent);
}

core::Status Renderer::reload_terrain_shaders(std::span<const std::uint32_t> vertex_spirv,
                                              std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        chunk_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        terrain_shader_program_, make_terrain_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(terrain_shader_program_);
    if (!status) {
        return status;
    }
    std::array<rhi::RenderResourceHandle, 4> rebuilt{};
    for (std::size_t index = 0; index < terrain_pipeline_keys_.size(); ++index) {
        auto pipeline = pipeline_cache_->find(terrain_pipeline_keys_[index]);
        if (!pipeline) {
            return core::Status::failure(pipeline.error().code, pipeline.error().message);
        }
        rebuilt[index] = pipeline.value();
    }
    terrain_pipelines_ = {rebuilt[0], rebuilt[1], rebuilt[2], rebuilt[3]};
    return chunk_system_->set_terrain_pipelines(terrain_pipelines_);
}

core::Status Renderer::reload_static_mesh_shaders(std::span<const std::uint32_t> vertex_spirv,
                                                  std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        scene_render_system_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        scene_shader_program_, make_static_mesh_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(scene_shader_program_);
    if (!status) {
        return status;
    }
    std::array<rhi::RenderResourceHandle, 3> rebuilt{};
    for (std::size_t index = 0; index < scene_pipeline_keys_.size(); ++index) {
        auto pipeline = pipeline_cache_->find(scene_pipeline_keys_[index]);
        if (!pipeline) {
            return core::Status::failure(pipeline.error().code, pipeline.error().message);
        }
        rebuilt[index] = pipeline.value();
    }
    scene_pipelines_ = {rebuilt[0], rebuilt[1], rebuilt[2]};
    return scene_render_system_->set_pipelines(scene_pipelines_);
}

core::Status Renderer::reload_debug_shaders(std::span<const std::uint32_t> vertex_spirv,
                                            std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        debug_renderer_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        debug_shader_program_, make_debug_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(debug_shader_program_);
    if (!status) {
        return status;
    }
    std::array<rhi::RenderResourceHandle, 2> rebuilt{};
    for (std::size_t index = 0; index < debug_pipeline_keys_.size(); ++index) {
        auto pipeline = pipeline_cache_->find(debug_pipeline_keys_[index]);
        if (!pipeline) {
            return core::Status::failure(pipeline.error().code, pipeline.error().message);
        }
        rebuilt[index] = pipeline.value();
    }
    debug_pipelines_ = {rebuilt[0], rebuilt[1]};
    return debug_renderer_->set_pipelines(debug_pipelines_);
}

core::Status Renderer::reload_ui_shaders(std::span<const std::uint32_t> vertex_spirv,
                                         std::span<const std::uint32_t> fragment_spirv) {
    if (!is_initialized() || shader_manager_ == nullptr || pipeline_cache_ == nullptr ||
        ui_renderer_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before shader reload");
    }
    auto status = shader_manager_->reload_program(
        ui_shader_program_, make_ui_shader_program(vertex_spirv, fragment_spirv));
    if (!status) {
        return status;
    }
    status = pipeline_cache_->rebuild_program(ui_shader_program_);
    if (!status) {
        return status;
    }
    auto pipeline = pipeline_cache_->find(ui_pipeline_key_);
    if (!pipeline) {
        return core::Status::failure(pipeline.error().code, pipeline.error().message);
    }
    ui_pipeline_ = pipeline.value();
    return ui_renderer_->set_pipeline(ui_pipeline_);
}

core::Status Renderer::set_environment(rhi::RenderEnvironmentData environment) {
    if (!is_initialized()) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized before setting environment");
    }
    auto status = rhi::validate_render_environment(environment);
    if (!status) {
        return status;
    }
    environment_ = environment;
    if (frame_builder_ != nullptr) {
        frame_builder_->set_clear_color(
            {environment.fog_color.x, environment.fog_color.y, environment.fog_color.z, 1.0F});
    }
    return core::Status::ok();
}

void Renderer::set_voxel_lighting_stats(const world::ChunkLightSystemStats& lighting) noexcept {
    stats_.voxel_relight_solve_ms = lighting.last_solve_ms;
    stats_.voxel_relight_apply_ms = lighting.last_apply_ms;
    stats_.voxel_relight_changed_chunks = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        lighting.changed_chunks_this_update,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.voxel_relight_backlog_cells = lighting.snapshot_pending_cell_count;
    stats_.voxel_relight_visited_cells =
        lighting.last_sunlight_queue_visits + lighting.last_block_light_queue_visits;
    stats_.voxel_relight_stale_results = lighting.stale_results;
    stats_.voxel_relight_apply_budget_overruns = lighting.apply_budget_overruns;
}

void Renderer::set_voxel_fluid_stats(const world::ChunkFluidSystemStats& fluids) noexcept {
    stats_.voxel_fluid_snapshot_ms = fluids.last_snapshot_ms;
    stats_.voxel_fluid_simulation_ms = fluids.last_simulation_ms;
    stats_.voxel_fluid_apply_ms = fluids.last_apply_ms;
    stats_.voxel_fluid_changed_chunks = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        fluids.changed_chunks_this_update,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.voxel_fluid_active_cells = fluids.active_cell_count;
    stats_.voxel_fluid_processed_cells = fluids.processed_cells_this_update;
    stats_.voxel_fluid_budget_exhaustions = fluids.budget_exhaustions;
    stats_.voxel_fluid_apply_budget_overruns = fluids.apply_budget_overruns;
}

RenderObjectId Renderer::reserve_object_id() {
    return scene_.reserve_object_id();
}

RenderLightId Renderer::reserve_light_id() {
    return scene_.reserve_light_id();
}

core::Result<RenderObjectId> Renderer::create_object(RenderObjectProxy object) {
    if (!is_initialized()) {
        return core::Result<RenderObjectId>::failure("renderer.not_initialized",
                                                     "renderer must be initialized first");
    }
    return scene_.create_object(std::move(object));
}

core::Result<RenderLightId> Renderer::create_light(RenderLightProxy light) {
    if (!is_initialized()) {
        return core::Result<RenderLightId>::failure("renderer.not_initialized",
                                                    "renderer must be initialized first");
    }
    return scene_.create_light(std::move(light));
}

core::Status Renderer::apply_scene_updates(std::span<const RenderSceneUpdate> updates) {
    if (!is_initialized()) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized first");
    }
    return scene_.apply(updates);
}

core::Result<RenderMeshHandle> Renderer::create_static_mesh(const StaticMeshUploadDesc& desc) {
    if (mesh_manager_ == nullptr) {
        return core::Result<RenderMeshHandle>::failure("renderer.not_initialized",
                                                       "renderer must be initialized first");
    }
    return mesh_manager_->create_mesh(desc);
}

core::Result<RenderMeshHandle> Renderer::create_model_primitive(std::string id,
                                                                const assets::ModelAsset& model,
                                                                std::uint32_t primitive_index) {
    if (mesh_manager_ == nullptr) {
        return core::Result<RenderMeshHandle>::failure("renderer.not_initialized",
                                                       "renderer must be initialized first");
    }
    return mesh_manager_->create_model_primitive(std::move(id), model, primitive_index);
}

core::Status Renderer::release_static_mesh(RenderMeshHandle handle) {
    if (mesh_manager_ == nullptr) {
        return core::Status::failure("renderer.not_initialized",
                                     "renderer must be initialized first");
    }
    return mesh_manager_->release(handle);
}

bool Renderer::is_initialized() const noexcept {
    return device_ != nullptr && chunk_cache_ != nullptr && chunk_system_ != nullptr &&
           frame_builder_ != nullptr && shader_manager_ != nullptr && texture_manager_ != nullptr &&
           material_cache_ != nullptr && pipeline_cache_ != nullptr && mesh_manager_ != nullptr &&
           scene_render_system_ != nullptr && sky_renderer_ != nullptr &&
           sky_renderer_->is_initialized() && debug_renderer_ != nullptr &&
           ui_renderer_ != nullptr && sky_pipeline_.is_valid() && terrain_pipelines_.is_valid() &&
           scene_pipelines_.is_valid() && debug_pipelines_.is_valid() && ui_pipeline_.is_valid();
}

bool Renderer::is_owner_thread() const noexcept {
    return owner_thread_ != std::thread::id{} && owner_thread_ == std::this_thread::get_id();
}

const ChunkRenderStats& Renderer::chunk_stats() const noexcept {
    return chunk_system_ == nullptr ? empty_chunk_stats : chunk_system_->stats();
}

const RendererStats& Renderer::stats() const noexcept {
    return stats_;
}

const SceneRenderStats& Renderer::scene_stats() const noexcept {
    static const SceneRenderStats empty;
    return scene_render_system_ == nullptr ? empty : scene_render_system_->stats();
}

RenderMeshHandle Renderer::fallback_mesh() const noexcept {
    return mesh_manager_ == nullptr ? RenderMeshHandle{} : mesh_manager_->fallback_mesh();
}

DebugRenderer* Renderer::debug_renderer() noexcept {
    return debug_renderer_.get();
}

const DebugRenderer* Renderer::debug_renderer() const noexcept {
    return debug_renderer_.get();
}

std::span<const DebugTextLabelFrame> Renderer::debug_text_labels() const noexcept {
    return debug_text_labels_;
}

UiRenderer* Renderer::ui_renderer() noexcept {
    return ui_renderer_.get();
}

const UiRenderer* Renderer::ui_renderer() const noexcept {
    return ui_renderer_.get();
}

rhi::IRenderDevice* Renderer::device() noexcept {
    return device_.get();
}

const rhi::IRenderDevice* Renderer::device() const noexcept {
    return device_.get();
}

void Renderer::update_frontend_stats(std::size_t loaded_chunk_count) noexcept {
    const auto saturating_u32 = [](std::size_t value) noexcept {
        return static_cast<std::uint32_t>(
            std::min(value, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    };
    const auto& chunks = chunk_stats();
    stats_.render_extraction_ms =
        cpu_timings_.milliseconds(profiling::CpuTimingZone::render_extraction);
    stats_.chunk_synchronization_ms =
        cpu_timings_.milliseconds(profiling::CpuTimingZone::chunk_synchronization);
    stats_.culling_ms = chunks.culling_ms;
    stats_.draw_list_ms = chunks.draw_list_ms;
    stats_.command_build_ms = cpu_timings_.milliseconds(profiling::CpuTimingZone::command_build);
    stats_.chunk_snapshot_ms = chunks.chunk_snapshot_ms;
    stats_.meshing_ms = chunks.meshing_ms;
    stats_.upload_preparation_ms = chunks.upload_preparation_ms;
    stats_.upload_ms = chunks.upload_ms;
    stats_.gpu_wait_ms = chunks.gpu_wait_ms;
    stats_.loaded_chunks = saturating_u32(loaded_chunk_count);
    stats_.mesh_pending_chunks = saturating_u32(chunks.pending_mesh_count);
    stats_.upload_pending_chunks = saturating_u32(chunks.pending_upload_count);
    stats_.resident_chunks = saturating_u32(chunks.cache.resident_chunk_count);
    stats_.visible_chunks = saturating_u32(chunks.visible_chunk_count);
    stats_.culled_chunks = saturating_u32(chunks.culled_chunk_count);
    stats_.drawn_chunks = saturating_u32(chunks.drawn_chunk_count);
    stats_.residency_suppressed_chunks = saturating_u32(chunks.residency_suppressed_chunk_count);
    if (texture_manager_ != nullptr) {
        stats_.resident_textures = saturating_u32(texture_manager_->stats().resident_texture_count);
        stats_.resident_texture_bytes = texture_manager_->stats().resident_texture_bytes;
    }
    if (material_cache_ != nullptr) {
        stats_.runtime_materials = saturating_u32(material_cache_->stats().resident_material_count);
    }
    if (pipeline_cache_ != nullptr) {
        stats_.resident_pipelines =
            saturating_u32(pipeline_cache_->stats().resident_pipeline_count);
    }
    if (scene_render_system_ != nullptr) {
        const auto& scene = scene_render_system_->stats();
        stats_.retained_objects = scene.scene.retained_objects;
        stats_.visible_objects = scene.scene.visible_objects;
        stats_.culled_objects = scene.scene.culled_objects;
        stats_.instance_batches = scene.scene.instance_batches;
        stats_.submitted_instances = scene.submitted_instances;
        stats_.instance_draw_calls = scene.draw_calls;
        stats_.dropped_instances = scene.dropped_instances;
        stats_.uploaded_instance_bytes = scene.uploaded_instance_bytes;
    }
    if (mesh_manager_ != nullptr) {
        const auto meshes = mesh_manager_->stats();
        stats_.resident_static_meshes = saturating_u32(meshes.resident_mesh_count);
        stats_.resident_static_mesh_bytes = meshes.resident_mesh_bytes;
    }
    if (debug_renderer_ != nullptr) {
        const auto& debug = debug_renderer_->stats();
        stats_.debug_lines = debug.submitted_lines;
        stats_.debug_draw_calls = debug.draw_calls;
        stats_.debug_labels = debug.active_text_labels;
        stats_.debug_overflow = debug.overflowed_lines + debug.overflowed_text_labels;
        stats_.debug_uploaded_bytes = debug.uploaded_bytes;
    }
    if (ui_renderer_ != nullptr) {
        const auto& ui = ui_renderer_->stats();
        stats_.ui_draw_calls = ui.draw_calls;
        stats_.ui_clipped_draw_calls = ui.clipped_draw_calls;
        stats_.ui_vertices = ui.submitted_vertices;
        stats_.ui_glyphs = ui.submitted_glyphs;
        stats_.ui_uploaded_bytes = ui.uploaded_bytes;
        stats_.ui_overflow = ui.overflowed_batches;
    }
    stats_.vertices = chunks.visible_vertex_count;
    stats_.triangles = chunks.visible_index_count / 3;
    stats_.resident_mesh_bytes = chunks.cache.resident_bytes;
    stats_.gpu_terrain_budget_bytes = chunks.gpu_terrain_budget_bytes;
    stats_.distance_evicted_meshes = chunks.distance_evicted_mesh_count;
    stats_.memory_pressure_evicted_meshes = chunks.memory_pressure_evicted_mesh_count;
    stats_.gpu_arena_capacity_bytes =
        chunks.cache.vertex_arena.capacity_bytes + chunks.cache.index_arena.capacity_bytes;
    stats_.gpu_arena_used_bytes =
        chunks.cache.vertex_arena.used_bytes + chunks.cache.index_arena.used_bytes;
    stats_.gpu_arena_free_bytes =
        chunks.cache.vertex_arena.free_bytes + chunks.cache.index_arena.free_bytes;
    const auto arena_free = stats_.gpu_arena_free_bytes;
    const auto arena_largest = chunks.cache.vertex_arena.largest_free_range_bytes +
                               chunks.cache.index_arena.largest_free_range_bytes;
    stats_.gpu_arena_fragmentation = arena_free == 0 ? 0.0
                                                     : 1.0 - static_cast<double>(arena_largest) /
                                                                 static_cast<double>(arena_free);
    stats_.pending_upload_bytes = chunks.pending_upload_bytes;
    stats_.uploaded_bytes_this_frame = chunks.uploaded_bytes;
}

void Renderer::update_backend_stats(const rhi::RenderFrameStats& frame) noexcept {
    stats_.frame_index = frame.frame_index;
    stats_.submission_serial = frame.submission_serial;
    stats_.completed_submission_serial = frame.completed_submission_serial;
    stats_.draw_calls = static_cast<std::uint32_t>(std::min(
        frame.draw_count, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.opaque_terrain_draws = static_cast<std::uint32_t>(
        std::min(frame.opaque_terrain_draw_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.alpha_tested_terrain_draws = static_cast<std::uint32_t>(
        std::min(frame.alpha_tested_terrain_draw_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.transparent_terrain_draws = static_cast<std::uint32_t>(
        std::min(frame.transparent_terrain_draw_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.pipeline_switches = static_cast<std::uint32_t>(
        std::min(frame.pipeline_bind_count,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    stats_.triangles = frame.total_indices / 3U;
    stats_.command_recording_ms = frame.cpu_command_recording_ms;
    stats_.gpu_wait_ms += frame.cpu_gpu_wait_ms;
    stats_.gpu_timing_valid = frame.gpu_timing_valid;
    stats_.gpu_timing_frame_index = frame.gpu_timing_frame_index;
    stats_.gpu_timing_latency_frames = frame.gpu_timing_latency_frames;
    stats_.gpu_upload_timing_valid = frame.gpu_upload_timing_valid;
    stats_.gpu_upload_submission_serial = frame.gpu_upload_submission_serial;
    stats_.gpu_frame_ms = frame.gpu_frame_ms;
    stats_.gpu_opaque_terrain_ms = frame.gpu_opaque_terrain_ms;
    stats_.gpu_alpha_tested_terrain_ms = frame.gpu_alpha_tested_terrain_ms;
    stats_.gpu_transparent_terrain_ms = frame.gpu_transparent_terrain_ms;
    stats_.gpu_upload_ms = frame.gpu_upload_ms;
    stats_.gpu_transfer_ms = frame.gpu_transfer_ms;
    stats_.gpu_final_copy_ms = frame.gpu_final_copy_ms;
}

core::Status Renderer::create_sky_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                           std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "sky runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/sky");
    if (!material) {
        return core::Status::failure("renderer.invalid_sky_material",
                                     "internal sky material id is invalid");
    }
    auto shader =
        shader_manager_->create_program(make_sky_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    sky_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/sky.vert"};
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "sky_gradient_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.debug_name = "sky_gradient_pipeline";
    pipeline.vertex_stride = sizeof(GpuSkyVertex);
    pipeline.vertex_attributes.assign(gpu_sky_vertex_attributes.begin(),
                                      gpu_sky_vertex_attributes.end());
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = rhi::RenderCompareOperation::always;
    pipeline.blend_mode = rhi::RenderBlendMode::disabled;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;

    sky_pipeline_key_.shader_program = sky_shader_program_;
    sky_pipeline_key_.vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    sky_pipeline_key_.render_phase = RenderPhase::sky;
    sky_pipeline_key_.color_format = pipeline.color_target_format;
    sky_pipeline_key_.depth_format = pipeline.depth_target_format;
    sky_pipeline_key_.cull_mode = pipeline.cull_mode;
    sky_pipeline_key_.front_face = pipeline.front_face;
    sky_pipeline_key_.depth_test = pipeline.depth_test_enable;
    sky_pipeline_key_.depth_write = pipeline.depth_write_enable;
    sky_pipeline_key_.depth_compare = pipeline.depth_compare;
    sky_pipeline_key_.blend_mode = pipeline.blend_mode;
    auto created = pipeline_cache_->prewarm(sky_pipeline_key_, layout, std::move(pipeline));
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    sky_pipeline_ = created.value();
    return core::Status::ok();
}

core::Status Renderer::create_terrain_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                               std::span<const std::uint32_t> fragment_spirv,
                                               const world::VoxelPalette* voxel_palette) {
    if (shader_manager_ == nullptr || sampler_cache_ == nullptr || texture_manager_ == nullptr ||
        material_cache_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "terrain runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/milestone_terrain");
    if (!material) {
        return core::Status::failure("renderer.invalid_terrain_material",
                                     "internal terrain material prototype id is invalid");
    }

    TerrainTextureArrayBuilder texture_builder(16, 16);
    auto layer = texture_builder.add_layer("error", make_terrain_tile({255, 0, 255}, true));
    if (!layer) {
        return core::Status::failure(layer.error().code, layer.error().message);
    }
    constexpr std::array<std::array<std::uint8_t, 3>, 6> terrain_colors{
        std::array<std::uint8_t, 3>{74, 145, 57},   std::array<std::uint8_t, 3>{118, 78, 46},
        std::array<std::uint8_t, 3>{112, 116, 124}, std::array<std::uint8_t, 3>{184, 162, 98},
        std::array<std::uint8_t, 3>{54, 111, 48},   std::array<std::uint8_t, 3>{127, 91, 59},
    };
    for (std::size_t index = 0; index < terrain_colors.size(); ++index) {
        layer = texture_builder.add_layer("terrain_" + std::to_string(index + 1),
                                          make_terrain_tile(terrain_colors[index]));
        if (!layer) {
            return core::Status::failure(layer.error().code, layer.error().message);
        }
    }
    auto texture_desc = texture_builder.build("terrain_texture_array");
    if (!texture_desc) {
        return core::Status::failure(texture_desc.error().code, texture_desc.error().message);
    }
    auto texture = texture_manager_->create_texture(std::move(texture_desc).value());
    if (!texture) {
        return core::Status::failure(texture.error().code, texture.error().message);
    }
    terrain_texture_array_ = texture.value();
    const auto* texture_view = texture_manager_->find(terrain_texture_array_);
    if (texture_view == nullptr) {
        return core::Status::failure("renderer.terrain_texture_missing",
                                     "terrain texture array disappeared after creation");
    }

    rhi::RenderSamplerDesc sampler_desc;
    sampler_desc.min_filter = rhi::RenderSamplerFilter::nearest;
    sampler_desc.mag_filter = rhi::RenderSamplerFilter::nearest;
    sampler_desc.mipmap_mode = rhi::RenderSamplerMipmapMode::linear;
    sampler_desc.max_lod = static_cast<float>(texture_view->mip_levels - 1U);
    sampler_desc.debug_name = "terrain_sampler";
    auto sampler = sampler_cache_->get(std::move(sampler_desc));
    if (!sampler) {
        return core::Status::failure(sampler.error().code, sampler.error().message);
    }
    terrain_sampler_ = sampler.value();

    std::vector<std::uint16_t> voxel_types;
    if (voxel_palette != nullptr && !voxel_palette->empty()) {
        const auto definitions = voxel_palette->definitions();
        voxel_types.reserve(definitions.size());
        for (const auto* definition : definitions) {
            voxel_types.push_back(definition->type);
        }
    } else {
        voxel_types.reserve(255);
        for (std::uint16_t type = 1; type <= 255; ++type) {
            voxel_types.push_back(type);
        }
    }
    for (const auto type : voxel_types) {
        auto runtime_id =
            core::PrototypeId::parse("base:materials/runtime_voxel_" + std::to_string(type));
        if (!runtime_id) {
            return core::Status::failure("renderer.invalid_runtime_material_id",
                                         "generated voxel material id is invalid");
        }
        MaterialRuntimeDesc runtime_material;
        runtime_material.id = runtime_id.value();
        runtime_material.voxel_type = type;
        runtime_material.side_texture = 1U + (static_cast<std::uint32_t>(type) - 1U) % 6U;
        runtime_material.top_texture = runtime_material.side_texture;
        runtime_material.bottom_texture = runtime_material.side_texture;
        if (voxel_palette != nullptr) {
            const auto* definition = voxel_palette->find_by_type(type);
            if (definition != nullptr) {
                const auto& model = voxel_palette->model_for(*definition);
                if (definition->logical_occupancy == world::BlockLogicalOccupancy::fluid) {
                    runtime_material.flags =
                        runtime_material.flags | VoxelMaterialFlags::translucent;
                    runtime_material.base_color[3] = 0.68F;
                    runtime_material.roughness = 0.2F;
                }
                if (model.kind == world::BlockModelKind::cross_plane) {
                    runtime_material.flags = runtime_material.flags |
                                             VoxelMaterialFlags::alpha_tested |
                                             VoxelMaterialFlags::two_sided;
                }
                if (definition->light_emission > 0) {
                    runtime_material.flags = runtime_material.flags | VoxelMaterialFlags::emissive;
                    runtime_material.emissive_strength =
                        static_cast<float>(definition->light_emission) / 255.0F;
                }
            }
        }
        auto inserted = material_cache_->upsert(std::move(runtime_material));
        if (!inserted) {
            return core::Status::failure(inserted.error().code, inserted.error().message);
        }
    }
    auto material_status = material_cache_->synchronize_gpu();
    if (!material_status) {
        return material_status;
    }

    auto shader =
        shader_manager_->create_program(make_terrain_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    terrain_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/terrain.vert"};
    layout.descriptors = {
        {"terrain_textures", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
        {"voxel_materials", rhi::RenderDescriptorKind::storage_buffer, 1, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "terrain_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.debug_name = "opaque_terrain_pipeline";
    pipeline.vertex_stride = sizeof(terrain::GpuChunkVertex);
    pipeline.vertex_attributes.assign(terrain::gpu_chunk_vertex_attributes.begin(),
                                      terrain::gpu_chunk_vertex_attributes.end());
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::back;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = true;
    pipeline.depth_compare = rhi::RenderCompareOperation::less;
    pipeline.blend_mode = rhi::RenderBlendMode::disabled;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    const auto vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    const auto prewarm = [&](std::size_t index, RenderPhase phase,
                             rhi::RenderGraphicsPipelineDesc phase_pipeline)
        -> core::Result<rhi::RenderResourceHandle> {
        GraphicsPipelineKey key;
        key.shader_program = terrain_shader_program_;
        key.vertex_layout = vertex_layout;
        key.render_phase = phase;
        key.color_format = phase_pipeline.color_target_format;
        key.depth_format = phase_pipeline.depth_target_format;
        key.cull_mode = phase_pipeline.cull_mode;
        key.front_face = phase_pipeline.front_face;
        key.depth_test = phase_pipeline.depth_test_enable;
        key.depth_write = phase_pipeline.depth_write_enable;
        key.depth_compare = phase_pipeline.depth_compare;
        key.blend_mode = phase_pipeline.blend_mode;
        terrain_pipeline_keys_[index] = key;
        return pipeline_cache_->prewarm(key, layout, std::move(phase_pipeline));
    };

    auto opaque = prewarm(0, RenderPhase::opaque_terrain, pipeline);
    if (!opaque) {
        return core::Status::failure(opaque.error().code, opaque.error().message);
    }
    auto alpha_pipeline = pipeline;
    alpha_pipeline.debug_name = "alpha_tested_terrain_pipeline";
    alpha_pipeline.cull_mode = rhi::RenderCullMode::none;
    auto alpha = prewarm(1, RenderPhase::alpha_tested_terrain, std::move(alpha_pipeline));
    if (!alpha) {
        return core::Status::failure(alpha.error().code, alpha.error().message);
    }
    auto transparent_pipeline = pipeline;
    transparent_pipeline.debug_name = "transparent_terrain_pipeline";
    transparent_pipeline.depth_write_enable = false;
    transparent_pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    auto transparent =
        prewarm(2, RenderPhase::transparent_terrain, std::move(transparent_pipeline));
    if (!transparent) {
        return core::Status::failure(transparent.error().code, transparent.error().message);
    }
    auto fluid_pipeline = pipeline;
    fluid_pipeline.debug_name = "fluid_terrain_pipeline";
    fluid_pipeline.depth_write_enable = false;
    fluid_pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    auto fluid = prewarm(3, RenderPhase::fluid_terrain, std::move(fluid_pipeline));
    if (!fluid) {
        return core::Status::failure(fluid.error().code, fluid.error().message);
    }
    terrain_pipelines_ = {opaque.value(), alpha.value(), transparent.value(), fluid.value()};

    const rhi::RenderDescriptorWrite texture_write{
        material.value(), "terrain_textures", texture_view->image, 0, 0, terrain_sampler_};
    auto texture_binding =
        device_->write_descriptors(std::span<const rhi::RenderDescriptorWrite>{&texture_write, 1});
    if (!texture_binding) {
        return core::Status::failure(texture_binding.error().code, texture_binding.error().message);
    }
    material_status =
        material_cache_->write_gpu_table_descriptor(material.value(), "voxel_materials");
    if (!material_status) {
        return material_status;
    }
    return core::Status::ok();
}

core::Status Renderer::create_scene_pipelines(std::span<const std::uint32_t> vertex_spirv,
                                              std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "scene runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/static_instances");
    if (!material) {
        return core::Status::failure("renderer.invalid_scene_material",
                                     "internal static-instance material id is invalid");
    }
    auto shader = shader_manager_->create_program(
        make_static_mesh_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    scene_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/static_mesh.vert"};
    layout.descriptors = {
        {"object_instances", rhi::RenderDescriptorKind::storage_buffer, 0, true,
         rhi::RenderShaderStageFlags::vertex},
    };
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "static_instances_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.vertex_stride = sizeof(GpuStaticMeshVertex);
    pipeline.vertex_attributes.assign(std::begin(gpu_static_mesh_vertex_attributes),
                                      std::end(gpu_static_mesh_vertex_attributes));
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::back;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = true;
    pipeline.depth_compare = rhi::RenderCompareOperation::less;
    pipeline.blend_mode = rhi::RenderBlendMode::disabled;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    const auto vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    const auto prewarm =
        [&](std::size_t index, RenderPhase phase,
            rhi::RenderGraphicsPipelineDesc desc) -> core::Result<rhi::RenderResourceHandle> {
        GraphicsPipelineKey key;
        key.shader_program = scene_shader_program_;
        key.vertex_layout = vertex_layout;
        key.render_phase = phase;
        key.color_format = desc.color_target_format;
        key.depth_format = desc.depth_target_format;
        key.cull_mode = desc.cull_mode;
        key.front_face = desc.front_face;
        key.depth_test = desc.depth_test_enable;
        key.depth_write = desc.depth_write_enable;
        key.depth_compare = desc.depth_compare;
        key.blend_mode = desc.blend_mode;
        scene_pipeline_keys_[index] = key;
        return pipeline_cache_->prewarm(key, layout, std::move(desc));
    };
    pipeline.debug_name = "opaque_static_instances_pipeline";
    auto opaque = prewarm(0, RenderPhase::static_instances, pipeline);
    if (!opaque) {
        return core::Status::failure(opaque.error().code, opaque.error().message);
    }
    auto alpha_desc = pipeline;
    alpha_desc.debug_name = "alpha_tested_static_instances_pipeline";
    alpha_desc.cull_mode = rhi::RenderCullMode::none;
    auto alpha = prewarm(1, RenderPhase::static_instances, std::move(alpha_desc));
    if (!alpha) {
        return core::Status::failure(alpha.error().code, alpha.error().message);
    }
    auto transparent_desc = pipeline;
    transparent_desc.debug_name = "transparent_static_instances_pipeline";
    transparent_desc.depth_write_enable = false;
    transparent_desc.blend_mode = rhi::RenderBlendMode::alpha;
    auto transparent = prewarm(2, RenderPhase::transparent_terrain, std::move(transparent_desc));
    if (!transparent) {
        return core::Status::failure(transparent.error().code, transparent.error().message);
    }
    scene_pipelines_ = {opaque.value(), alpha.value(), transparent.value()};
    return core::Status::ok();
}

core::Status Renderer::create_debug_pipelines(std::span<const std::uint32_t> vertex_spirv,
                                              std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "debug runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/debug_lines");
    if (!material) {
        return core::Status::failure("renderer.invalid_debug_material",
                                     "internal debug-line material id is invalid");
    }
    auto shader =
        shader_manager_->create_program(make_debug_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    debug_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/debug_line.vert"};
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "debug_lines_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.vertex_stride = sizeof(GpuDebugVertex);
    pipeline.vertex_attributes.assign(std::begin(gpu_debug_vertex_attributes),
                                      std::end(gpu_debug_vertex_attributes));
    pipeline.topology = rhi::RenderPrimitiveTopology::line_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = rhi::RenderCompareOperation::less_or_equal;
    pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    const auto vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    const auto prewarm =
        [&](std::size_t index,
            rhi::RenderGraphicsPipelineDesc desc) -> core::Result<rhi::RenderResourceHandle> {
        GraphicsPipelineKey key;
        key.shader_program = debug_shader_program_;
        key.vertex_layout = vertex_layout;
        key.render_phase = RenderPhase::debug;
        key.color_format = desc.color_target_format;
        key.depth_format = desc.depth_target_format;
        key.cull_mode = desc.cull_mode;
        key.front_face = desc.front_face;
        key.depth_test = desc.depth_test_enable;
        key.depth_write = desc.depth_write_enable;
        key.depth_compare = desc.depth_compare;
        key.blend_mode = desc.blend_mode;
        debug_pipeline_keys_[index] = key;
        return pipeline_cache_->prewarm(key, layout, std::move(desc));
    };
    pipeline.debug_name = "depth_tested_debug_lines_pipeline";
    auto depth = prewarm(0, pipeline);
    if (!depth) {
        return core::Status::failure(depth.error().code, depth.error().message);
    }
    pipeline.debug_name = "overlay_debug_lines_pipeline";
    pipeline.depth_compare = rhi::RenderCompareOperation::always;
    auto overlay = prewarm(1, std::move(pipeline));
    if (!overlay) {
        return core::Status::failure(overlay.error().code, overlay.error().message);
    }
    debug_pipelines_ = {depth.value(), overlay.value()};
    return core::Status::ok();
}

core::Status Renderer::create_ui_pipeline(std::span<const std::uint32_t> vertex_spirv,
                                          std::span<const std::uint32_t> fragment_spirv) {
    if (shader_manager_ == nullptr || pipeline_cache_ == nullptr || texture_manager_ == nullptr ||
        !terrain_sampler_.is_valid()) {
        return core::Status::failure("renderer.runtime_assets_uninitialized",
                                     "UI runtime asset managers must be initialized first");
    }
    const auto material = core::PrototypeId::parse("base:materials/ui");
    if (!material) {
        return core::Status::failure("renderer.invalid_ui_material",
                                     "internal UI material id is invalid");
    }
    TextureUploadDesc atlas_desc;
    atlas_desc.id = "builtin:ui_atlas";
    atlas_desc.width = 128;
    atlas_desc.height = 64;
    atlas_desc.array_layers = 2;
    atlas_desc.color_space = TextureColorSpace::srgb;
    atlas_desc.generate_mipmaps = true;
    atlas_desc.rgba8 = make_ui_atlas();
    auto atlas = texture_manager_->create_texture(std::move(atlas_desc));
    if (!atlas) {
        return core::Status::failure(atlas.error().code, atlas.error().message);
    }
    ui_texture_atlas_ = atlas.value();
    const auto* atlas_view = texture_manager_->find(ui_texture_atlas_);
    if (atlas_view == nullptr) {
        return core::Status::failure("renderer.ui_atlas_missing",
                                     "UI atlas disappeared after creation");
    }
    auto shader =
        shader_manager_->create_program(make_ui_shader_program(vertex_spirv, fragment_spirv));
    if (!shader) {
        return core::Status::failure(shader.error().code, shader.error().message);
    }
    ui_shader_program_ = shader.value();

    rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/ui.vert"};
    layout.descriptors = {
        {"ui_atlas", rhi::RenderDescriptorKind::sampled_texture, 0, true,
         rhi::RenderShaderStageFlags::fragment},
    };
    layout.push_constant_ranges.push_back(
        {rhi::RenderShaderStageFlags::vertex | rhi::RenderShaderStageFlags::fragment, 0,
         sizeof(rhi::ChunkPushConstants)});
    layout.debug_name = "ui_layout";

    rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = material.value();
    pipeline.debug_name = "ui_pipeline";
    pipeline.vertex_stride = sizeof(GpuUiVertex);
    pipeline.vertex_attributes.assign(gpu_ui_vertex_attributes.begin(),
                                      gpu_ui_vertex_attributes.end());
    pipeline.topology = rhi::RenderPrimitiveTopology::triangle_list;
    pipeline.polygon_mode = rhi::RenderPolygonMode::fill;
    pipeline.cull_mode = rhi::RenderCullMode::none;
    pipeline.front_face = rhi::RenderFrontFace::counter_clockwise;
    // Keep a compatible physical render pass while always accepting UI fragments.
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = rhi::RenderCompareOperation::always;
    pipeline.blend_mode = rhi::RenderBlendMode::alpha;
    pipeline.color_target_format = rhi::RenderImageFormat::rgba8_unorm;
    pipeline.depth_target_format = rhi::RenderImageFormat::d32_sfloat;
    ui_pipeline_key_.shader_program = ui_shader_program_;
    ui_pipeline_key_.vertex_layout =
        hash_vertex_layout(pipeline.vertex_stride, pipeline.vertex_attributes);
    ui_pipeline_key_.render_phase = RenderPhase::ui;
    ui_pipeline_key_.color_format = pipeline.color_target_format;
    ui_pipeline_key_.depth_format = pipeline.depth_target_format;
    ui_pipeline_key_.cull_mode = pipeline.cull_mode;
    ui_pipeline_key_.front_face = pipeline.front_face;
    ui_pipeline_key_.depth_test = pipeline.depth_test_enable;
    ui_pipeline_key_.depth_write = pipeline.depth_write_enable;
    ui_pipeline_key_.depth_compare = pipeline.depth_compare;
    ui_pipeline_key_.blend_mode = pipeline.blend_mode;
    auto created = pipeline_cache_->prewarm(ui_pipeline_key_, layout, std::move(pipeline));
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    ui_pipeline_ = created.value();
    const rhi::RenderDescriptorWrite atlas_write{
        material.value(), "ui_atlas", atlas_view->image, 0, 0, terrain_sampler_};
    auto binding =
        device_->write_descriptors(std::span<const rhi::RenderDescriptorWrite>{&atlas_write, 1});
    if (!binding) {
        return core::Status::failure(binding.error().code, binding.error().message);
    }
    return core::Status::ok();
}

} // namespace heartstead::renderer
