#include "engine/renderer/renderer.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

int main() {
    using namespace heartstead;

    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const std::vector<std::uint32_t> spirv{0x07230203, 0x00010000, 0, 1, 0};

    renderer::RendererInitDesc init;
    init.device = std::move(device).value();
    init.sky_vertex_spirv = spirv;
    init.sky_fragment_spirv = spirv;
    init.terrain_vertex_spirv = spirv;
    init.terrain_fragment_spirv = spirv;
    init.static_mesh_vertex_spirv = spirv;
    init.static_mesh_fragment_spirv = spirv;
    init.debug_vertex_spirv = spirv;
    init.debug_fragment_spirv = spirv;
    init.ui_vertex_spirv = spirv;
    init.ui_fragment_spirv = spirv;
    init.tone_map_vertex_spirv = spirv;
    init.tone_map_fragment_spirv = spirv;
    init.streaming_residency_config.job_backend = jobs::JobBackend::immediate;
    init.streaming_residency_config.worker_count = 1;
    init.streaming_residency_loader =
        [](const renderer::ResidencyRequest& request, const std::atomic_bool&) {
            renderer::ResidencyPayload payload;
            payload.id = request.id;
            payload.resource_class = request.resource_class;
            payload.detail_level = request.detail_level;
            payload.bytes.resize(request.estimated_gpu_bytes, std::byte{0x2a});
            payload.estimated_gpu_bytes = request.estimated_gpu_bytes;
            return core::Result<renderer::ResidencyPayload>::success(std::move(payload));
        };

    renderer::Renderer active_renderer;
    assert(active_renderer.initialize(std::move(init)));
    assert(active_renderer.streaming_residency() != nullptr);
    active_renderer.streaming_residency()->begin_frame(4);
    assert(active_renderer.streaming_residency()->request(
        {"test:streamed-mesh", renderer::ResidencyResourceClass::mesh, 1, 10.0F, 256, false}));
    assert(active_renderer.streaming_residency()->stats().queued_resources == 1);
    assert(active_renderer.shutdown());
    assert(active_renderer.streaming_residency() == nullptr);
    return 0;
}
