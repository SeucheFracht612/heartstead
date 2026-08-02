#include "engine/renderer/rhi/render_device.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

using namespace heartstead;

int main() {
    using namespace renderer::rhi;

    static_assert(sizeof(RenderIndexedIndirectCommand) == 20U);
    static_assert(offsetof(RenderIndexedIndirectCommand, index_count) == 0U);
    static_assert(offsetof(RenderIndexedIndirectCommand, instance_count) == 4U);
    static_assert(offsetof(RenderIndexedIndirectCommand, first_index) == 8U);
    static_assert(offsetof(RenderIndexedIndirectCommand, vertex_offset) == 12U);
    static_assert(offsetof(RenderIndexedIndirectCommand, first_instance) == 16U);

    assert(validate_render_buffer_desc(
        {RenderBufferUsage::indirect, sizeof(RenderIndexedIndirectCommand), "commands"}));
    assert(validate_render_buffer_desc(
        {RenderBufferUsage::storage_indirect, 256, "gpu_generated_commands",
         RenderBufferMemory::device_local}));
    assert(render_buffer_usage_name(RenderBufferUsage::indirect) == "indirect");
    assert(render_buffer_usage_name(RenderBufferUsage::storage_indirect) == "storage_indirect");

    RenderIndirectDrawBinding empty;
    assert(!empty.is_valid());
    assert(empty.is_structurally_valid());

    RenderIndirectDrawBinding malformed;
    malformed.count_buffer = {2};
    assert(!malformed.is_structurally_valid());
    malformed = {};
    malformed.command_buffer = {1};
    malformed.maximum_draw_count = 2;
    malformed.stride = 19;
    assert(!malformed.is_structurally_valid());
    malformed.stride = 22;
    assert(!malformed.is_structurally_valid());
    malformed.stride = sizeof(RenderIndexedIndirectCommand);
    malformed.command_offset = 2;
    assert(!malformed.is_structurally_valid());

    RenderDeviceDesc desc;
    desc.backend = RenderBackend::headless;
    desc.application_name = "heartstead_indirect_draw_tests";
    desc.enable_validation = true;
    auto device_result = create_render_device(desc);
    assert(device_result);
    auto& device = *device_result.value();
    const auto capabilities = device.capabilities();
    assert(capabilities.supports_multi_draw_indirect);
    assert(capabilities.supports_draw_indirect_first_instance);
    assert(capabilities.supports_draw_indirect_count);

    const std::array commands{
        RenderIndexedIndirectCommand{3, 1, 0, 0, 0},
        RenderIndexedIndirectCommand{6, 2, 3, 0, 1},
    };
    const auto command_bytes = std::as_bytes(std::span{commands});
    auto command_buffer = device.upload_buffer(
        {RenderBufferUsage::storage_indirect, command_bytes.size(), "indirect_commands",
         RenderBufferMemory::device_local},
        command_bytes);
    assert(command_buffer);

    const std::uint32_t count = 2;
    auto count_buffer = device.upload_buffer(
        {RenderBufferUsage::storage_indirect, sizeof(count), "indirect_count",
         RenderBufferMemory::device_local},
        std::as_bytes(std::span{&count, 1U}));
    assert(count_buffer);

    RenderIndirectDrawBinding valid;
    valid.command_buffer = command_buffer.value().handle;
    valid.count_buffer = count_buffer.value().handle;
    valid.maximum_draw_count = 2;
    assert(valid.is_valid());
    assert(valid.is_structurally_valid());

    assert(device.release_resource(count_buffer.value().handle));
    assert(device.release_resource(command_buffer.value().handle));
    assert(device.live_resource_count() == 0U);
    return 0;
}
