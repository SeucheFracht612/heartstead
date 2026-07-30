#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

namespace {

template <typename Outcome> void expect_wrong_thread(const Outcome& outcome) {
    assert(!outcome);
    assert(outcome.error().code == "renderer.render_device_wrong_thread");
}

void test_render_device_mutations_are_owner_thread_only() {
    using namespace heartstead::renderer::rhi;

    auto device_result = create_render_device({});
    assert(device_result);
    auto& device = device_result.value();

    const auto buffer = device->create_buffer(
        {RenderBufferUsage::storage, 16, "owner_thread_probe", RenderBufferMemory::host_visible});
    assert(buffer);
    const auto initial_extent = device->current_extent();
    const auto initial_live_resources = device->live_resource_count();
    const auto initial_frame_count = device->completed_frame_count();
    const auto initial_submission_serial = device->last_submission_serial();

    std::thread worker([&] {
        constexpr std::array<std::byte, 4> bytes{};
        constexpr std::array<std::uint32_t, 5> spirv_words{
            0x07230203,
            0x00010000,
            0,
            1,
            0,
        };
        const std::array<RenderBufferWrite, 1> buffer_writes{{
            {buffer.value().handle, 0, std::span<const std::byte>(bytes)},
        }};
        const std::vector<RenderDescriptorWrite> descriptor_writes;
        const std::vector<RenderMeshBinding> mesh_draws;
        const auto plan = make_clear_present_frame_plan({320, 180}, {}, false);

        expect_wrong_thread(device->resize({320, 180}));
        expect_wrong_thread(device->render_frame({{}, {320, 180}, false}));
        expect_wrong_thread(device->execute_frame_plan(plan));
        expect_wrong_thread(device->execute_frame({plan, {}, {}, {}, {}}));
        expect_wrong_thread(device->create_buffer(
            {RenderBufferUsage::storage, bytes.size(), "wrong_thread_create"}));
        expect_wrong_thread(device->upload_buffer_batch(buffer_writes));
        expect_wrong_thread(device->upload_buffer(
            {RenderBufferUsage::storage, bytes.size(), "wrong_thread_upload"}, bytes));
        expect_wrong_thread(device->upload_image(
            {RenderImageFormat::rgba8_unorm, 1, 1, "wrong_thread_image"}, bytes));
        expect_wrong_thread(device->create_sampler({}));
        expect_wrong_thread(device->create_shader_module(
            {RenderShaderStage::vertex, "wrong_thread_shader"}, spirv_words));
        expect_wrong_thread(device->bind_pipeline_layout({}));
        expect_wrong_thread(device->create_compute_pipeline({}));
        expect_wrong_thread(device->create_graphics_pipeline({}));
        expect_wrong_thread(device->write_descriptors(descriptor_writes));
        expect_wrong_thread(device->bind_mesh_draws(mesh_draws));
        expect_wrong_thread(device->release_resource(buffer.value().handle));
    });
    worker.join();

    assert(device->current_extent().width == initial_extent.width);
    assert(device->current_extent().height == initial_extent.height);
    assert(device->live_resource_count() == initial_live_resources);
    assert(device->completed_frame_count() == initial_frame_count);
    assert(device->last_submission_serial() == initial_submission_serial);
    assert(device->release_resource(buffer.value().handle));
}

void test_descriptor_stage_and_lifetime_validation() {
    using namespace heartstead;
    using namespace renderer::rhi;

    const auto material = core::PrototypeId::parse("base:materials/descriptor_state_test");
    assert(material);

    RenderPipelineLayoutDesc invalid_layout;
    invalid_layout.material_id = material.value();
    invalid_layout.shader_template = {"base", "shaders/descriptor_state_test.vert"};
    invalid_layout.descriptors.push_back({"frame_data", RenderDescriptorKind::uniform_scalar, 0,
                                          true, RenderShaderStageFlags::none});
    const auto invalid_stages = validate_render_pipeline_layout_shape(invalid_layout);
    assert(!invalid_stages);
    assert(invalid_stages.error().code == "renderer.invalid_descriptor_stages");

    auto vertex_layout = invalid_layout;
    vertex_layout.descriptors.front().stages = RenderShaderStageFlags::vertex;
    auto fragment_layout = vertex_layout;
    fragment_layout.descriptors.front().stages = RenderShaderStageFlags::fragment;
    assert(!equivalent_render_pipeline_layout(vertex_layout, fragment_layout));

    auto device_result = create_render_device({});
    assert(device_result);
    auto& device = device_result.value();
    assert(device->bind_pipeline_layout(vertex_layout));

    constexpr std::array<std::uint32_t, 5> spirv_words{
        0x07230203,
        0x00010000,
        0,
        1,
        0,
    };
    const auto vertex_shader = device->create_shader_module(
        {RenderShaderStage::vertex, "descriptor_state_vertex"}, spirv_words);
    const auto fragment_shader = device->create_shader_module(
        {RenderShaderStage::fragment, "descriptor_state_fragment"}, spirv_words);
    assert(vertex_shader);
    assert(fragment_shader);

    RenderGraphicsPipelineDesc pipeline_desc;
    pipeline_desc.vertex_shader = vertex_shader.value().handle;
    pipeline_desc.fragment_shader = fragment_shader.value().handle;
    pipeline_desc.material_id = material.value();
    const auto pipeline = device->create_graphics_pipeline(pipeline_desc);
    assert(pipeline);

    const auto vertices = device->create_buffer(
        {RenderBufferUsage::vertex, 12, "descriptor_state_vertices"});
    const auto uniform = device->create_buffer(
        {RenderBufferUsage::uniform, 16, "descriptor_state_uniform"});
    assert(vertices);
    assert(uniform);
    const std::array<RenderMeshBinding, 1> draws{{
        {vertices.value().handle, {}, material.value(), 1, 0, 1, "descriptor_state_draw"},
    }};

    const auto missing_required = device->bind_mesh_draws(draws);
    assert(!missing_required);
    assert(missing_required.error().code == "renderer.required_descriptor_unbound");

    const std::array<RenderDescriptorWrite, 1> wrong_type{{
        {material.value(), "frame_data", vertices.value().handle, 0, 12},
    }};
    const auto wrong_type_result = device->write_descriptors(wrong_type);
    assert(!wrong_type_result);
    assert(wrong_type_result.error().code == "renderer.invalid_descriptor_resource_usage");

    const std::array<RenderDescriptorWrite, 1> writes{{
        {material.value(), "frame_data", uniform.value().handle, 0, 16},
    }};
    const std::array<RenderDescriptorWrite, 2> duplicate_writes{{writes.front(), writes.front()}};
    const auto duplicate_write = device->write_descriptors(duplicate_writes);
    assert(!duplicate_write);
    assert(duplicate_write.error().code == "renderer.duplicate_descriptor_write");
    const auto still_missing_after_failed_batch = device->bind_mesh_draws(draws);
    assert(!still_missing_after_failed_batch);
    assert(still_missing_after_failed_batch.error().code ==
           "renderer.required_descriptor_unbound");

    assert(device->write_descriptors(writes));
    assert(device->bind_mesh_draws(draws));

    assert(device->release_resource(uniform.value().handle));
    const auto invalidated_required = device->bind_mesh_draws(draws);
    assert(!invalidated_required);
    assert(invalidated_required.error().code == "renderer.required_descriptor_unbound");
    const auto released_write = device->write_descriptors(writes);
    assert(!released_write);
    assert(released_write.error().code == "renderer.unknown_descriptor_resource");
}

void test_failed_batches_preserve_device_state() {
    using namespace heartstead::renderer::rhi;

    auto device_result = create_render_device({});
    assert(device_result);
    auto& device = device_result.value();
    const auto first = device->create_buffer(
        {RenderBufferUsage::storage, 8, "transaction_first", RenderBufferMemory::host_visible});
    const auto second = device->create_buffer(
        {RenderBufferUsage::storage, 8, "transaction_second", RenderBufferMemory::host_visible});
    assert(first);
    assert(second);

    constexpr std::array<std::byte, 4> bytes{};
    const std::array<RenderBufferWrite, 2> invalid_batch{{
        {first.value().handle, 0, std::span<const std::byte>(bytes)},
        {second.value().handle, 8, std::span<const std::byte>(bytes)},
    }};
    const auto serial_before_batch = device->last_submission_serial();
    const auto failed_batch = device->upload_buffer_batch(invalid_batch);
    assert(!failed_batch);
    assert(failed_batch.error().code == "renderer.buffer_write_out_of_bounds");
    assert(device->last_submission_serial() == serial_before_batch);

    const auto live_before_unknown_release = device->live_resource_count();
    const auto unknown_release = device->release_resource({999999});
    assert(!unknown_release);
    assert(unknown_release.error().code == "renderer.unknown_resource");
    assert(device->live_resource_count() == live_before_unknown_release);

    const auto plan = make_clear_present_frame_plan({64, 64}, {}, false);
    RenderDrawCommand invalid_draw;
    invalid_draw.pipeline = {900001};
    invalid_draw.vertex_buffer = {900002};
    invalid_draw.index_buffer = {900003};
    invalid_draw.index_count = 1;
    const RenderFrameSubmission frame{plan, {}, {}, {}, {{0, {invalid_draw}}}};
    const auto frame_count_before_failure = device->completed_frame_count();
    const auto serial_before_frame = device->last_submission_serial();
    const auto failed_frame = device->execute_frame(frame);
    assert(!failed_frame);
    assert(failed_frame.error().code == "renderer.unknown_graphics_pipeline");
    assert(device->completed_frame_count() == frame_count_before_failure);
    assert(device->last_submission_serial() == serial_before_frame);
}

} // namespace

int main() {
    test_render_device_mutations_are_owner_thread_only();
    test_descriptor_stage_and_lifetime_validation();
    test_failed_batches_preserve_device_state();
    return 0;
}
