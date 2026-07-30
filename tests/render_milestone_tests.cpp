#include "engine/math/matrix.hpp"
#include "engine/platform/platform.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"
#include "engine/renderer/terrain/gpu_chunk_vertex.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/meshing/chunk_mesher.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace {

[[nodiscard]] bool nearly_equal(float left, float right, float epsilon = 0.0001F) {
    return std::abs(left - right) <= epsilon;
}

void test_matrix_composition() {
    using namespace heartstead::math;

    const auto model = translation_matrix({5.0F, -2.0F, 3.0F}) * scale_matrix({2.0F, 3.0F, 4.0F});
    const auto transformed = model * Vec4f{1.0F, 2.0F, -1.0F, 1.0F};
    assert(nearly_equal(transformed.x, 7.0F));
    assert(nearly_equal(transformed.y, 4.0F));
    assert(nearly_equal(transformed.z, -1.0F));
    assert(nearly_equal(transformed.w, 1.0F));

    const auto identity_composition = Mat4f::identity() * model;
    assert(identity_composition == model);
}

void test_vulkan_projection() {
    using namespace heartstead::math;

    constexpr float near_plane = 0.1F;
    constexpr float far_plane = 100.0F;
    const auto projection =
        perspective_projection(std::numbers::pi_v<float> * 0.5F, 2.0F, near_plane, far_plane);
    const auto near_clip = projection * Vec4f{0.0F, 0.0F, -near_plane, 1.0F};
    const auto far_clip = projection * Vec4f{0.0F, 0.0F, -far_plane, 1.0F};
    assert(nearly_equal(near_clip.z / near_clip.w, 0.0F));
    assert(nearly_equal(far_clip.z / far_clip.w, 1.0F));

    const auto right_edge = projection * Vec4f{2.0F, 0.0F, -1.0F, 1.0F};
    assert(nearly_equal(right_edge.x / right_edge.w, 1.0F));
    const auto upper_edge = projection * Vec4f{0.0F, 1.0F, -1.0F, 1.0F};
    assert(nearly_equal(upper_edge.y / upper_edge.w, -1.0F));
}

void test_view_and_camera_resize() {
    using namespace heartstead;

    const auto view = math::view_matrix({3.0F, 4.0F, 5.0F}, 0.0F, 0.0F);
    const auto camera_origin = view * math::Vec4f{3.0F, 4.0F, 5.0F, 1.0F};
    assert(nearly_equal(camera_origin.x, 0.0F));
    assert(nearly_equal(camera_origin.y, 0.0F));
    assert(nearly_equal(camera_origin.z, 0.0F));

    renderer::RenderCamera camera;
    camera.local_position = {3.0F, 4.0F, 5.0F};
    assert(camera.update_matrices());
    const auto old_horizontal_scale = camera.projection.at(0, 0);
    assert(camera.set_aspect_ratio(4.0F / 3.0F));
    assert(camera.projection.at(0, 0) > old_horizontal_scale);
    assert(camera.view_projection == camera.projection * camera.view);
    assert(!camera.set_aspect_ratio(0.0F));
}

void test_gpu_chunk_vertex_contract() {
    using namespace heartstead;

    const world::ChunkMeshVertex cpu_vertex{
        {1.0F, 2.0F, 3.0F}, {0.0F, 1.0F, 0.0F}, 0.25F, 0.75F, 42, 190, 0x1234};
    const auto gpu_vertex = renderer::terrain::to_gpu_chunk_vertex(cpu_vertex);
    assert(sizeof(renderer::terrain::GpuTerrainVertex) == 24);
    assert(renderer::terrain::decode_terrain_vertex_position(gpu_vertex) == cpu_vertex.position);
    assert(renderer::terrain::decode_terrain_vertex_normal(gpu_vertex) == cpu_vertex.normal);
    assert(renderer::terrain::decode_terrain_vertex_uv(gpu_vertex)[0] == cpu_vertex.u);
    assert(gpu_vertex.material == 42);
    assert(gpu_vertex.lighting[0] == 190);
    assert(gpu_vertex.lighting[1] == 255);
    assert(gpu_vertex.state_bits == 0x1234);
    assert(renderer::terrain::gpu_chunk_vertex_attributes[0].format ==
           renderer::rhi::RenderVertexAttributeFormat::sint16x4);
    assert(renderer::rhi::RenderVertexAttributeFormat::uint16x4 !=
           renderer::rhi::RenderVertexAttributeFormat::sint16x4);
    assert(renderer::terrain::gpu_chunk_vertex_attributes[2].format ==
           renderer::rhi::RenderVertexAttributeFormat::uint16);
    assert(renderer::terrain::gpu_chunk_vertex_attributes[5].format ==
           renderer::rhi::RenderVertexAttributeFormat::uint8x4);

    const world::ChunkMeshVertex fractional{
        {-0.35F, 17.125F, 40.0F}, {0.70710677F, 0.0F, -0.70710677F}, 31.5F, 2.25F, 9, 255, 0};
    const auto packed = renderer::terrain::to_gpu_chunk_vertex(fractional, 3);
    const auto decoded_position = renderer::terrain::decode_terrain_vertex_position(packed);
    const auto decoded_normal = renderer::terrain::decode_terrain_vertex_normal(packed);
    const auto decoded_uv = renderer::terrain::decode_terrain_vertex_uv(packed);
    assert(std::abs(decoded_position.x - fractional.position.x) <= 1.0F / 256.0F);
    assert(std::abs(decoded_position.y - fractional.position.y) <= 1.0F / 256.0F);
    assert(std::abs(decoded_normal.x - fractional.normal.x) <= 1.0F / 127.0F);
    assert(std::abs(decoded_normal.z - fractional.normal.z) <= 1.0F / 127.0F);
    assert(decoded_uv[0] == fractional.u);
    assert(decoded_uv[1] == fractional.v);
    assert(packed.lighting[2] == 3);
}

void test_spirv_validation() {
    using namespace heartstead::renderer::shaders;

    constexpr std::uint32_t valid_header[]{0x07230203, 0x00010000, 0, 1, 0};
    assert(validate_spirv(valid_header));
    constexpr std::uint32_t bad_magic[]{0xdeadbeef, 0x00010000, 0, 1, 0};
    const auto invalid = validate_spirv(bad_magic);
    assert(!invalid);
    assert(invalid.error().code == "renderer.invalid_spirv_magic");
}

void test_render_environment_validation() {
    using namespace heartstead::renderer::rhi;

    const RenderEnvironmentData environment{};
    assert(validate_render_environment(environment));

    auto invalid_sun = environment;
    invalid_sun.sun_direction = {};
    const auto sun_status = validate_render_environment(invalid_sun);
    assert(!sun_status);
    assert(sun_status.error().code == "renderer.invalid_environment_range");

    auto invalid_fog = environment;
    invalid_fog.fog_end = invalid_fog.fog_start;
    const auto fog_status = validate_render_environment(invalid_fog);
    assert(!fog_status);
    assert(fog_status.error().code == "renderer.invalid_environment_range");

    auto invalid_color = environment;
    invalid_color.ambient_color.x = -0.01F;
    assert(!validate_render_environment(invalid_color));
}

void test_minimized_window_and_idempotent_quit() {
    using namespace heartstead::platform;

    HeadlessPlatform platform;
    const auto window = platform.create_window({"render milestone platform", 640, 360, true});
    assert(window);
    assert(platform.poll_event()); // window_created
    assert(platform.queue_event(
        {PlatformEventKind::window_resized, window.value(), KeyCode::unknown, 0, 0, {}}));
    const auto* minimized = platform.find_window(window.value());
    assert(minimized != nullptr);
    assert(minimized->width == 0);
    assert(minimized->height == 0);

    platform.request_quit();
    platform.request_quit();
    std::size_t quit_events = 0;
    while (const auto event = platform.poll_event()) {
        if (event->kind == PlatformEventKind::quit_requested) {
            ++quit_events;
        }
    }
    assert(platform.should_quit());
    assert(quit_events == 1);
}

void test_unified_headless_frame_submission() {
    using namespace heartstead;
    using namespace renderer::rhi;

    auto device = create_render_device(RenderDeviceDesc{});
    assert(device);
    const auto material = core::PrototypeId::parse("base:materials/terrain_test");
    assert(material);

    RenderPipelineLayoutDesc layout;
    layout.material_id = material.value();
    layout.shader_template = {"base", "shaders/terrain_test.glsl"};
    layout.push_constant_ranges.push_back(
        {RenderShaderStageFlags::vertex, 0, sizeof(ChunkPushConstants)});
    assert(device.value()->bind_pipeline_layout(layout));

    constexpr std::array<std::uint32_t, 5> spirv_header{
        0x07230203, 0x00010000, 0, 1, 0,
    };
    auto vertex_shader = device.value()->create_shader_module(
        {RenderShaderStage::vertex, "headless_terrain_vertex"}, spirv_header);
    auto fragment_shader = device.value()->create_shader_module(
        {RenderShaderStage::fragment, "headless_terrain_fragment"}, spirv_header);
    assert(vertex_shader);
    assert(fragment_shader);

    RenderGraphicsPipelineDesc pipeline_desc;
    pipeline_desc.vertex_shader = vertex_shader.value().handle;
    pipeline_desc.fragment_shader = fragment_shader.value().handle;
    pipeline_desc.material_id = material.value();
    pipeline_desc.vertex_stride = sizeof(renderer::terrain::GpuChunkVertex);
    pipeline_desc.vertex_attributes.assign(renderer::terrain::gpu_chunk_vertex_attributes.begin(),
                                           renderer::terrain::gpu_chunk_vertex_attributes.end());
    auto pipeline = device.value()->create_graphics_pipeline(pipeline_desc);
    assert(pipeline);

    constexpr std::array<renderer::terrain::GpuChunkVertex, 3> vertices{};
    constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
    auto vertex_upload = device.value()->upload_buffer(
        {RenderBufferUsage::vertex, sizeof(vertices), "headless_terrain_vertices"},
        std::as_bytes(std::span(vertices)));
    auto index_upload = device.value()->upload_buffer(
        {RenderBufferUsage::index, sizeof(indices), "headless_terrain_indices"},
        std::as_bytes(std::span(indices)));
    assert(vertex_upload);
    assert(index_upload);

    RenderFramePlanBuilder builder({640, 360});
    assert(builder.add_resource(
        {"output", {640, 360}, RenderResourceLifetime::external, RenderImageFormat::rgba8_unorm}));
    assert(builder.add_resource(
        {"depth", {640, 360}, RenderResourceLifetime::transient, RenderImageFormat::d32_sfloat}));
    assert(builder.add_pass({"world",
                             RenderPassKind::world,
                             {},
                             {"output", "depth"},
                             {0.05F, 0.1F, 0.2F, 1.0F},
                             false}));
    assert(builder.add_pass({"present", RenderPassKind::present, {"output"}, {}, {}, true}));
    auto plan = builder.build();
    assert(plan);

    RenderFrameSubmission frame;
    frame.plan = plan.value();
    frame.camera.view_projection = math::Mat4f::identity();
    RenderDrawCommand milestone_draw;
    milestone_draw.pipeline = pipeline.value().handle;
    milestone_draw.vertex_buffer = vertex_upload.value().handle;
    milestone_draw.index_buffer = index_upload.value().handle;
    milestone_draw.index_count = 3;
    milestone_draw.instance_count = 1;
    milestone_draw.camera_relative_origin = {32.0F, 0.0F, -32.0F};
    frame.pass_commands.push_back({0, {milestone_draw}});

    auto stats = device.value()->execute_frame(frame);
    assert(stats);
    assert(stats.value().draw_count == 1);
    assert(stats.value().indexed_draw_count == 1);
    assert(stats.value().pipeline_bind_count == 1);
    assert(stats.value().total_indices == 3);
    assert(stats.value().presented);

    frame.pass_commands.front().draws.front().index_type = static_cast<RenderIndexType>(0xff);
    auto invalid_index_type = device.value()->execute_frame(frame);
    assert(!invalid_index_type);
    assert(invalid_index_type.error().code == "renderer.invalid_index_type");
    frame.pass_commands.front().draws.front().index_type = RenderIndexType::uint32;

    frame.pass_commands.front().draws.front().first_index = 1;
    auto out_of_bounds = device.value()->execute_frame(frame);
    assert(!out_of_bounds);
    assert(out_of_bounds.error().code == "renderer.draw_index_range_out_of_bounds");
    frame.pass_commands.front().draws.front().first_index = 0;
    frame.pass_commands.front().draws.front().scissor_enabled = true;
    frame.pass_commands.front().draws.front().scissor = {600, 0, 64, 64};
    auto invalid_scissor = device.value()->execute_frame(frame);
    assert(!invalid_scissor);
    assert(invalid_scissor.error().code == "renderer.invalid_draw_scissor");
}

void test_immutable_chunk_meshing_snapshot() {
    using namespace heartstead;
    world::ChunkDatabase chunks;
    const world::ChunkCoord center_coord{7, 0, -3};
    const world::ChunkCoord neighbor_coord{8, 0, -3};
    auto& center = chunks.get_or_create(center_coord);
    auto& neighbor = chunks.get_or_create(neighbor_coord);
    assert(center.set({31, 4, 5}, world::VoxelCell{1, 255}));
    assert(neighbor.set({0, 4, 5}, world::VoxelCell{1, 255}));

    auto table = world::build_block_render_table_snapshot(nullptr);
    assert(table);
    auto snapshot =
        world::build_chunk_neighborhood_snapshot(chunks, center.identity(), table.value());
    assert(snapshot);
    assert(snapshot.value().halo_radius == 1);
    assert(snapshot.value().side_length == 34);
    assert(snapshot.value().cell_count() == 34U * 34U * 34U);
    assert(snapshot.value().dependencies.size() == 27);
    assert(world::dependency_revisions_match(chunks, snapshot.value().dependencies));

    auto immutable_mesh = world::ChunkMesher::build_surface_mesh(snapshot.value(), table.value());
    assert(immutable_mesh);
    world::ChunkMeshingContext live_context{center, nullptr, &chunks, 1};
    auto reference_mesh = world::ChunkMesher::build_surface_mesh(live_context);
    assert(reference_mesh);
    assert(immutable_mesh.value().face_count == reference_mesh.value().face_count);
    assert(immutable_mesh.value().vertices == reference_mesh.value().vertices);
    assert(immutable_mesh.value().indices == reference_mesh.value().indices);

    assert(neighbor.set({0, 4, 5}, world::VoxelCell::air()));
    assert(!world::dependency_revisions_match(chunks, snapshot.value().dependencies));
    auto unchanged_old_snapshot =
        world::ChunkMesher::build_surface_mesh(snapshot.value(), table.value());
    assert(unchanged_old_snapshot);
    assert(unchanged_old_snapshot.value().face_count == immutable_mesh.value().face_count);

    const auto old_neighbor_identity = neighbor.identity();
    assert(chunks.erase(neighbor_coord));
    auto& reloaded_neighbor = chunks.get_or_create(neighbor_coord);
    assert(reloaded_neighbor.identity() != old_neighbor_identity);
    assert(!world::dependency_revisions_match(chunks, snapshot.value().dependencies));
}

} // namespace

int main() {
    test_matrix_composition();
    test_vulkan_projection();
    test_view_and_camera_resize();
    test_gpu_chunk_vertex_contract();
    test_spirv_validation();
    test_render_environment_validation();
    test_minimized_window_and_idempotent_quit();
    test_unified_headless_frame_submission();
    test_immutable_chunk_meshing_snapshot();
    return 0;
}
