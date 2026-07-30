#include "engine/renderer/assets/sampler_cache.hpp"
#include "engine/renderer/assets/shader_manager.hpp"
#include "engine/renderer/assets/surface_texture_array.hpp"
#include "engine/renderer/assets/texture_manager.hpp"
#include "engine/renderer/materials/material_runtime_cache.hpp"
#include "engine/renderer/materials/pipeline_cache.hpp"
#include "engine/renderer/terrain/gpu_chunk_vertex.hpp"
#include "engine/renderer/terrain/terrain_mapping.hpp"

#include <array>
#include <cassert>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] renderer::ShaderProgramDesc make_program(std::uint32_t bound,
                                                       std::string id = "terrain") {
    const std::vector<std::uint32_t> spirv{0x07230203, 0x00010000, 0, bound, 0};
    renderer::ShaderProgramDesc program;
    program.id = std::move(id);
    program.stages = {
        {renderer::rhi::RenderShaderStage::vertex, "main", spirv, "terrain.vert.spv"},
        {renderer::rhi::RenderShaderStage::fragment, "main", spirv, "terrain.frag.spv"},
    };
    program.interface.vertex_stride = sizeof(renderer::terrain::GpuChunkVertex);
    for (const auto& attribute : renderer::terrain::gpu_chunk_vertex_attributes) {
        program.interface.vertex_inputs.push_back({attribute.location, attribute.format});
    }
    program.interface.push_constant_ranges.push_back({renderer::rhi::RenderShaderStageFlags::vertex,
                                                      0,
                                                      sizeof(renderer::rhi::ChunkPushConstants)});
    program.dependencies = {"terrain.common", "camera.contract"};
    return program;
}

[[nodiscard]] renderer::rhi::RenderPipelineLayoutDesc make_layout() {
    auto material = core::PrototypeId::parse("base:materials/runtime_terrain");
    assert(material);
    renderer::rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/terrain.vert"};
    layout.push_constant_ranges.push_back({renderer::rhi::RenderShaderStageFlags::vertex, 0,
                                           sizeof(renderer::rhi::ChunkPushConstants)});
    layout.debug_name = "runtime_terrain_layout";
    return layout;
}

[[nodiscard]] renderer::rhi::RenderGraphicsPipelineDesc
make_pipeline(const renderer::rhi::RenderPipelineLayoutDesc& layout) {
    renderer::rhi::RenderGraphicsPipelineDesc pipeline;
    pipeline.material_id = layout.material_id;
    pipeline.debug_name = "runtime_terrain_pipeline";
    pipeline.vertex_stride = sizeof(renderer::terrain::GpuChunkVertex);
    pipeline.vertex_attributes.assign(renderer::terrain::gpu_chunk_vertex_attributes.begin(),
                                      renderer::terrain::gpu_chunk_vertex_attributes.end());
    pipeline.color_target_format = renderer::rhi::RenderImageFormat::rgba8_unorm;
    pipeline.depth_target_format = renderer::rhi::RenderImageFormat::d32_sfloat;
    return pipeline;
}

void test_shader_hot_reload_and_pipeline_cache() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline = device.value()->live_resource_count();

    renderer::ShaderManager shaders(*device.value(), true);
    auto program = shaders.create_program(make_program(1));
    assert(program);
    assert(shaders.stats().resident_program_count == 1);
    assert(shaders.stats().resident_module_count == 2);
    assert(shaders.find(program.value())->revision == 1);
    assert(device.value()->live_resource_count() == baseline + 2);

    renderer::PipelineCache pipelines(*device.value(), shaders);
    const auto layout = make_layout();
    const auto pipeline_desc = make_pipeline(layout);
    renderer::GraphicsPipelineKey key;
    key.shader_program = program.value();
    key.vertex_layout =
        renderer::hash_vertex_layout(pipeline_desc.vertex_stride, pipeline_desc.vertex_attributes);
    auto pipeline = pipelines.prewarm(key, layout, pipeline_desc);
    assert(pipeline);
    const auto first_pipeline = pipeline.value();
    assert(pipelines.prewarm(key, layout, pipeline_desc).value() == first_pipeline);
    assert(pipelines.stats().resident_pipeline_count == 1);
    assert(pipelines.stats().created_pipeline_count == 1);
    assert(device.value()->live_resource_count() == baseline + 3);

    pipelines.seal();
    auto missing_key = key;
    missing_key.feature_flags = 1;
    auto rejected = pipelines.prewarm(missing_key, layout, pipeline_desc);
    assert(!rejected);
    assert(rejected.error().code == "pipeline_cache.runtime_creation_rejected");
    assert(pipelines.stats().unexpected_creation_rejection_count == 1);

    auto invalid_reload = make_program(2);
    invalid_reload.stages[1].spirv[0] = 0;
    auto reload_status = shaders.reload_program(program.value(), std::move(invalid_reload));
    assert(!reload_status);
    assert(shaders.find(program.value())->revision == 1);
    assert(shaders.stats().failed_reload_count == 1);
    assert(device.value()->live_resource_count() == baseline + 3);

    assert(shaders.reload_program(program.value(), make_program(3)));
    assert(shaders.find(program.value())->revision == 2);
    assert(shaders.stats().successful_reload_count == 1);
    assert(shaders.stats().superseded_module_count == 2);
    assert(device.value()->live_resource_count() == baseline + 5);
    assert(pipelines.rebuild_program(program.value()));
    auto rebuilt = pipelines.find(key);
    assert(rebuilt);
    assert(rebuilt.value() != first_pipeline);
    assert(pipelines.stats().rebuilt_pipeline_count == 1);
    assert(device.value()->live_resource_count() == baseline + 3);
    assert(shaders.stats().superseded_module_count == 0);

    assert(pipelines.shutdown());
    assert(shaders.shutdown());
    assert(device.value()->live_resource_count() == baseline);
}

void test_shader_interface_rejects_mismatch() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    renderer::ShaderManager shaders(*device.value());
    auto program = shaders.create_program(make_program(1));
    assert(program);
    auto layout = make_layout();
    auto pipeline = make_pipeline(layout);
    pipeline.vertex_stride -= 4;
    auto status = renderer::validate_shader_interface(
        *shaders.find(program.value()), layout, pipeline.vertex_stride, pipeline.vertex_attributes);
    assert(!status);
    assert(status.error().code == "shader_manager.vertex_stride_mismatch");
    assert(shaders.shutdown());
}

void test_shader_views_stay_valid_across_growth() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);

    renderer::ShaderManager shaders(*device.value());
    auto first_handle = shaders.create_program(make_program(1, "p0"));
    assert(first_handle);
    const auto* first_view = shaders.find(first_handle.value());
    assert(first_view != nullptr);
    assert(first_view->id == "p0");
    assert(first_view->vertex_entry_point == "main");
    assert(first_view->dependencies.size() == 2);

    for (std::uint32_t index = 1; index <= 512; ++index) {
        auto created = shaders.create_program(make_program(index + 1, "p" + std::to_string(index)));
        assert(created);
    }

    const auto* reacquired = shaders.find(first_handle.value());
    assert(reacquired == first_view);
    assert(reacquired->id == "p0");
    assert(reacquired->vertex_entry_point == "main");
    assert(reacquired->fragment_entry_point == "main");
    assert(reacquired->dependencies.front() == "terrain.common");
    assert(shaders.shutdown());
}

void test_shader_slot_reuse_clears_superseded_modules() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline = device.value()->live_resource_count();

    renderer::ShaderManager shaders(*device.value(), true);
    auto original = shaders.create_program(make_program(1, "original"));
    assert(original);
    assert(shaders.reload_program(original.value(), make_program(2, "original")));
    assert(shaders.stats().superseded_module_count == 2);
    assert(shaders.release_program(original.value()));
    assert(shaders.stats().resident_program_count == 0);
    assert(shaders.stats().superseded_module_count == 0);
    assert(device.value()->live_resource_count() == baseline);

    auto replacement = shaders.create_program(make_program(3, "replacement"));
    assert(replacement);
    assert(replacement.value().index == original.value().index);
    assert(replacement.value().generation != original.value().generation);
    assert(shaders.release_program(replacement.value()));
    assert(shaders.shutdown());
    assert(device.value()->live_resource_count() == baseline);
}

void test_push_constant_stage_coverage_requires_every_stage() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);

    auto desc = make_program(1);
    desc.interface.push_constant_ranges.front().stages =
        renderer::rhi::RenderShaderStageFlags::vertex |
        renderer::rhi::RenderShaderStageFlags::fragment;
    renderer::ShaderManager shaders(*device.value());
    auto program = shaders.create_program(std::move(desc));
    assert(program);

    auto layout = make_layout();
    auto pipeline = make_pipeline(layout);
    auto status = renderer::validate_shader_interface(
        *shaders.find(program.value()), layout, pipeline.vertex_stride, pipeline.vertex_attributes);
    assert(!status);
    assert(status.error().code == "shader_manager.push_constant_interface_mismatch");

    layout.push_constant_ranges.front().stages = renderer::rhi::RenderShaderStageFlags::vertex |
                                                 renderer::rhi::RenderShaderStageFlags::fragment;
    assert(renderer::validate_shader_interface(*shaders.find(program.value()), layout,
                                               pipeline.vertex_stride, pipeline.vertex_attributes));
    assert(shaders.shutdown());
}

[[nodiscard]] renderer::TextureUploadDesc make_texture_array(std::string id, std::uint8_t bias) {
    renderer::TextureUploadDesc desc;
    desc.id = std::move(id);
    desc.width = 4;
    desc.height = 4;
    desc.array_layers = 2;
    desc.rgba8.resize(4U * 4U * 2U * 4U);
    for (std::size_t index = 0; index < desc.rgba8.size(); index += 4) {
        desc.rgba8[index] = static_cast<std::byte>(bias + static_cast<std::uint8_t>(index % 31U));
        desc.rgba8[index + 1] = static_cast<std::byte>(64U);
        desc.rgba8[index + 2] = static_cast<std::byte>(128U);
        desc.rgba8[index + 3] = static_cast<std::byte>(255U);
    }
    return desc;
}

void test_texture_arrays_mips_fallbacks_and_sampler_cache() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline = device.value()->live_resource_count();

    renderer::TextureManager textures(*device.value());
    assert(textures.initialize_fallbacks());
    assert(textures.stats().resident_texture_count == 4);
    assert(textures.error_texture().is_valid());
    assert(textures.resolve_or_error({}).handle == textures.error_texture());
    assert(device.value()->live_resource_count() == baseline + 4);

    auto terrain = textures.create_texture(make_texture_array("terrain_array", 7));
    assert(terrain);
    const auto* view = textures.find(terrain.value());
    assert(view != nullptr);
    assert(view->array_layers == 2);
    assert(view->mip_levels == 3);
    assert(view->color_space == renderer::TextureColorSpace::srgb);
    assert(view->resident_bytes == (4U * 4U * 2U + 2U * 2U * 2U + 1U * 1U * 2U) * 4U);
    const auto first_image = view->image;

    auto invalid_replacement = make_texture_array("terrain_array", 11);
    invalid_replacement.rgba8.pop_back();
    auto status = textures.replace_texture(terrain.value(), std::move(invalid_replacement));
    assert(!status);
    assert(textures.find(terrain.value())->image == first_image);
    assert(textures.stats().failed_reload_count == 1);

    assert(textures.replace_texture(terrain.value(), make_texture_array("terrain_array", 13)));
    assert(textures.find(terrain.value())->image != first_image);
    assert(textures.find(terrain.value())->revision == 2);
    assert(textures.stats().successful_reload_count == 1);

    renderer::SamplerCache samplers(*device.value());
    renderer::rhi::RenderSamplerDesc sampler_desc;
    sampler_desc.min_filter = renderer::rhi::RenderSamplerFilter::linear;
    sampler_desc.mag_filter = renderer::rhi::RenderSamplerFilter::nearest;
    sampler_desc.mipmap_mode = renderer::rhi::RenderSamplerMipmapMode::linear;
    sampler_desc.max_anisotropy = 8.0F;
    sampler_desc.max_lod = 8.0F;
    auto sampler = samplers.get(sampler_desc);
    assert(sampler);
    assert(samplers.get(sampler_desc).value() == sampler.value());
    assert(samplers.stats().resident_sampler_count == 1);
    assert(samplers.stats().cache_hit_count == 1);
    auto invalid_anisotropy = sampler_desc;
    invalid_anisotropy.max_anisotropy = 17.0F;
    assert(!samplers.get(invalid_anisotropy));

    auto material = core::PrototypeId::parse("base:materials/texture_test");
    assert(material);
    renderer::rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/texture_test.frag"};
    layout.descriptors.push_back(
        {"terrain_textures", renderer::rhi::RenderDescriptorKind::sampled_texture, 0, true});
    assert(device.value()->bind_pipeline_layout(layout));
    const renderer::rhi::RenderDescriptorWrite texture_write{
        material.value(), "terrain_textures", textures.find(terrain.value())->image, 0, 0,
        sampler.value()};
    auto written = device.value()->write_descriptors(
        std::span<const renderer::rhi::RenderDescriptorWrite>{&texture_write, 1});
    assert(written);
    assert(written.value().sampled_texture_write_count == 1);

    assert(samplers.shutdown());
    assert(textures.release_texture(terrain.value()));
    assert(textures.shutdown());
    assert(device.value()->live_resource_count() == baseline);
}

void test_srgb_mip_generation() {
    const std::array<std::byte, 16> base{
        std::byte{0},   std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
    };
    const auto mips =
        renderer::generate_rgba8_mip_chain(2, 2, 1, renderer::TextureColorSpace::srgb, base);
    assert(mips.size() == 20);
    // Gamma-correct averaging is brighter than a naive byte average of 191.
    assert(std::to_integer<std::uint8_t>(mips[16]) > 220);
    assert(std::to_integer<std::uint8_t>(mips[19]) == 255);

    renderer::TextureUploadDesc overflowing;
    overflowing.id = "overflowing";
    overflowing.width = std::numeric_limits<std::uint32_t>::max();
    overflowing.height = std::numeric_limits<std::uint32_t>::max();
    overflowing.array_layers = std::numeric_limits<std::uint32_t>::max();
    const auto overflow_status = renderer::validate_texture_upload_desc(overflowing);
    assert(!overflow_status);
    assert(overflow_status.error().code == "texture_manager.extent_overflow");
}

void test_surface_texture_array_resizes_and_reuses_layers() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline = device.value()->live_resource_count();
    renderer::TextureManager textures(*device.value());
    renderer::SurfaceTextureArray surface(textures);
    assert(surface.initialize({2, 2, 8}));
    assert(surface.layer_count() == 4);
    assert(surface.fallbacks().error == 0);
    assert(surface.fallbacks().white == 1);
    assert(surface.fallbacks().normal == 2);
    const std::array<std::uint8_t, 8> pixels{255, 0, 0, 255, 0, 0, 255, 128};
    auto layer = surface.add("base:test/non_square", 1, 2, pixels);
    assert(layer && layer.value() == 4);
    assert(surface.add("base:test/non_square", 1, 2, pixels).value() == layer.value());
    assert(surface.synchronize());
    assert(surface.texture_view() != nullptr);
    assert(surface.texture_view()->array_layers == 5);
    assert(device.value()->live_resource_count() == baseline + 1);
    auto changed = pixels;
    changed[0] = 0;
    auto conflict = surface.add("base:test/non_square", 1, 2, changed);
    assert(!conflict);
    assert(conflict.error().code == "terrain_texture_array.layer_id_conflict");
    assert(surface.shutdown());
    assert(textures.shutdown());
    assert(device.value()->live_resource_count() == baseline);
}

void test_material_table_updates_without_mesh_changes() {
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline = device.value()->live_resource_count();

    renderer::TerrainTextureArrayBuilder array_builder(2, 2);
    const std::array<std::byte, 16> error_layer{
        std::byte{255}, std::byte{0}, std::byte{255}, std::byte{255},
        std::byte{0},   std::byte{0}, std::byte{0},   std::byte{255},
        std::byte{0},   std::byte{0}, std::byte{0},   std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{255}, std::byte{255},
    };
    const std::array<std::byte, 16> grass_layer{
        std::byte{32}, std::byte{160}, std::byte{48}, std::byte{255}, std::byte{36}, std::byte{170},
        std::byte{52}, std::byte{255}, std::byte{28}, std::byte{150}, std::byte{40}, std::byte{255},
        std::byte{40}, std::byte{180}, std::byte{56}, std::byte{255},
    };
    assert(array_builder.add_layer("error", error_layer).value() == 0);
    assert(array_builder.add_layer("grass", grass_layer).value() == 1);
    assert(array_builder.add_layer("grass", grass_layer).value() == 1);
    assert(array_builder.layer_count() == 2);
    auto array_desc = array_builder.build("terrain_tiles");
    assert(array_desc);
    assert(array_desc.value().array_layers == 2);

    renderer::MaterialRuntimeCache materials(*device.value());
    auto fallback_id = core::PrototypeId::parse("base:materials/error");
    assert(fallback_id);
    renderer::MaterialRuntimeDesc fallback;
    fallback.id = fallback_id.value();
    fallback.domain = renderer::MaterialRuntimeDomain::surface;
    auto fallback_handle = materials.upsert(fallback);
    assert(fallback_handle);
    assert(materials.find(fallback_handle.value())->domain ==
           renderer::MaterialRuntimeDomain::surface);
    assert(materials.find(fallback_handle.value())->material_index == 0);
    assert(materials.block_render_table().revision() == 1);
    auto material_id = core::PrototypeId::parse("base:materials/grass");
    assert(material_id);
    renderer::MaterialRuntimeDesc material;
    material.id = material_id.value();
    material.voxel_type = 1;
    material.face_texture_starts = {1, 2, 3, 4, 5, 6};
    material.face_texture_counts = {1, 2, 3, 4, 2, 1};
    auto handle = materials.upsert(material);
    assert(handle);
    const auto revision = materials.block_render_table().revision();
    assert(materials.upsert(material).value() == handle.value());
    assert(materials.block_render_table().revision() == revision);
    assert(materials.synchronize_gpu());
    assert(materials.gpu_table_buffer().is_valid());
    assert(materials.surface_gpu_table_buffer().is_valid());
    assert(materials.stats().gpu_table.material_count == 2);
    assert(materials.stats().gpu_table.upload_count == 1);
    assert(materials.stats().surface_gpu_table.material_count == 1);
    assert(materials.stats().surface_gpu_table.upload_count == 1);
    const auto gpu_material = renderer::gpu_voxel_material(material);
    assert(std::ranges::equal(
        material.face_texture_starts,
        std::span{gpu_material.face_texture_starts}.first(renderer::voxel_material_face_count)));
    assert(std::ranges::equal(
        material.face_texture_counts,
        std::span{gpu_material.face_texture_counts}.first(renderer::voxel_material_face_count)));
    const auto table_buffer = materials.gpu_table_buffer();
    const auto surface_table_buffer = materials.surface_gpu_table_buffer();
    assert(device.value()->live_resource_count() == baseline + 2);

    auto pipeline_material = core::PrototypeId::parse("base:materials/terrain_pipeline");
    assert(pipeline_material);
    renderer::rhi::RenderPipelineLayoutDesc layout;
    layout.material_id = pipeline_material.value();
    layout.shader_template = {"base", "shaders/terrain.frag"};
    layout.descriptors.push_back(
        {"voxel_materials", renderer::rhi::RenderDescriptorKind::storage_buffer, 0, true});
    layout.descriptors.push_back(
        {"surface_materials", renderer::rhi::RenderDescriptorKind::storage_buffer, 1, true});
    assert(device.value()->bind_pipeline_layout(layout));
    assert(materials.write_gpu_table_descriptor(pipeline_material.value(), "voxel_materials"));
    assert(materials.write_gpu_surface_table_descriptor(pipeline_material.value(),
                                                        "surface_materials"));

    material.base_color = {0.8F, 0.9F, 0.7F, 1.0F};
    assert(materials.upsert(material).value() == handle.value());
    assert(materials.find(handle.value())->revision == 2);
    assert(materials.synchronize_gpu());
    assert(materials.gpu_table_buffer() == table_buffer);
    assert(materials.surface_gpu_table_buffer() == surface_table_buffer);
    assert(materials.stats().gpu_table.upload_count == 2);
    assert(materials.stats().gpu_table.resident_revision ==
           materials.block_render_table().revision());

    assert(materials.shutdown());
    assert(device.value()->live_resource_count() == baseline);
}

void test_terrain_variant_selection_is_coordinate_stable() {
    using namespace heartstead;
    constexpr world::ChunkCoord chunk{-4'000'000'001LL, 17, 9'000'000'003LL};
    constexpr world::VoxelCoord local{31, 7, 0};
    constexpr auto first =
        renderer::terrain::select_terrain_variant(chunk, local, 3, 7, true, true);
    constexpr auto reloaded =
        renderer::terrain::select_terrain_variant(chunk, local, 3, 7, true, true);
    static_assert(first == reloaded);
    static_assert(first.variant < 7);
    static_assert(first.quarter_turns < 4);
    // The exact period used by the shader preserves the selection after very large coordinate
    // rebases; camera and unrelated voxel state are deliberately not inputs.
    constexpr world::ChunkCoord period_shifted{chunk.x + 1024, chunk.y, chunk.z - 2048};
    static_assert(renderer::terrain::select_terrain_variant(period_shifted, local, 3, 7, true,
                                                            true) == first);
    constexpr auto fixed =
        renderer::terrain::select_terrain_variant(chunk, local, 3, 7, false, false);
    static_assert(fixed.quarter_turns == 0);
    static_assert(!fixed.mirrored);
}

} // namespace

int main() {
    test_shader_hot_reload_and_pipeline_cache();
    test_shader_interface_rejects_mismatch();
    test_shader_views_stay_valid_across_growth();
    test_shader_slot_reuse_clears_superseded_modules();
    test_push_constant_stage_coverage_requires_every_stage();
    test_texture_arrays_mips_fallbacks_and_sampler_cache();
    test_srgb_mip_generation();
    test_surface_texture_array_resizes_and_reuses_layers();
    test_material_table_updates_without_mesh_changes();
    test_terrain_variant_selection_is_coordinate_stable();
    return 0;
}
