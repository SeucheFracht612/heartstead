#include "engine/renderer/memory/streaming_residency.hpp"

#include <cassert>
#include <cstddef>
#include <string>
#include <vector>

using namespace heartstead;

int main() {
    renderer::StreamingResidencyConfig config;
    config.job_backend = jobs::JobBackend::immediate;
    config.worker_count = 1;
    config.maximum_in_flight_loads = 2;
    config.upload_budget_bytes = 8;
    config.upload_budget_resources = 1;
    config.resident_budget_bytes = 16;
    config.texture_fallback = renderer::rhi::RenderResourceHandle{91};
    config.mesh_fallback = renderer::rhi::RenderResourceHandle{92};

    std::vector<std::string> load_order;
    auto manager_result = renderer::StreamingResidencyManager::create(
        config, [&load_order](const renderer::ResidencyRequest& request,
                              const std::atomic_bool& cancelled) {
            assert(!cancelled.load());
            load_order.push_back(request.id);
            renderer::ResidencyPayload payload;
            payload.id = request.id;
            payload.resource_class = request.resource_class;
            payload.detail_level = request.detail_level;
            payload.bytes.resize(request.estimated_gpu_bytes, std::byte{0x2a});
            payload.estimated_gpu_bytes = request.estimated_gpu_bytes;
            return core::Result<renderer::ResidencyPayload>::success(std::move(payload));
        });
    assert(manager_result);
    auto manager = std::move(manager_result.value());

    std::uint64_t next_handle = 100;
    std::vector<std::uint64_t> released;
    const auto upload = [&next_handle](renderer::ResidencyPayload&& payload) {
        return core::Result<renderer::ResidencyGpuResource>::success(
            {{next_handle++}, payload.estimated_gpu_bytes});
    };
    const auto release = [&released](renderer::ResidencyGpuResource resource) {
        released.push_back(resource.handle.value);
    };

    manager->begin_frame(1);
    assert(manager->request({"far", renderer::ResidencyResourceClass::mesh, 2, 0.1F, 8, false}));
    assert(manager->request({"near", renderer::ResidencyResourceClass::texture, 1, 5.0F, 8, false}));
    assert(manager->process(upload, release));
    assert(load_order.size() == 2U);
    assert(load_order.front() == "near");
    assert(manager->resolve("near", renderer::ResidencyResourceClass::texture).value == 100U);
    assert(manager->resolve("far", renderer::ResidencyResourceClass::mesh).value == 92U);
    assert(manager->stats().uploaded_resources_this_frame == 1U);

    manager->begin_frame(2);
    assert(manager->request({"near", renderer::ResidencyResourceClass::texture, 0, 6.0F, 8, false}));
    assert(manager->process(upload, release));
    assert(manager->resolve("near", renderer::ResidencyResourceClass::texture).value == 101U);
    assert(released == std::vector<std::uint64_t>{100U});

    manager->begin_frame(3);
    assert(manager->process(upload, release));
    assert(manager->resolve("far", renderer::ResidencyResourceClass::mesh).value == 102U);
    assert(manager->stats().resident_bytes == 16U);

    manager->begin_frame(4);
    assert(manager->request({"critical", renderer::ResidencyResourceClass::mesh, 0, 10.0F, 8,
                             true}));
    manager->set_reported_heap_budget(16);
    assert(manager->process(upload, release));
    assert(manager->resolve("critical", renderer::ResidencyResourceClass::mesh).value == 103U);
    assert(manager->stats().resident_bytes <= 13U); // 85% of the reported heap budget.
    assert(manager->stats().evicted_resources >= 2U);

    manager->cancel("critical");
    manager->begin_frame(5);
    assert(manager->process(upload, release));
    assert(manager->resolve("critical", renderer::ResidencyResourceClass::mesh).value == 92U);

    manager->shutdown(release);
    assert(manager->stats().tracked_resources == 0U);
    return 0;
}
