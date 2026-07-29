#include "engine/renderer/assets/mesh_manager.hpp"

#include <array>
#include <cassert>
#include <string>

namespace {

using namespace heartstead;

constexpr std::array<renderer::GpuStaticMeshVertex, 3> triangle_vertices{{
    {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
    {{1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
    {{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.5F, 1.0F}},
}};
constexpr std::array<std::uint32_t, 3> triangle_indices{0, 1, 2};

void test_mesh_upload_cache_fallback_and_release() {
    renderer::rhi::RenderDeviceDesc desc;
    desc.backend = renderer::rhi::RenderBackend::headless;
    auto device = renderer::rhi::create_render_device(desc);
    assert(device);
    const auto baseline = device.value()->live_resource_count();

    renderer::MeshManager manager(*device.value());
    renderer::MeshManagerConfig config;
    config.vertex_initial_bytes = 4096;
    config.vertex_maximum_bytes = 8192;
    config.index_initial_bytes = 4096;
    config.index_maximum_bytes = 8192;
    assert(manager.initialize(config));
    assert(manager.fallback_mesh().is_valid());
    assert(manager.find_exact(manager.fallback_mesh()) != nullptr);
    assert(manager.stats().resident_mesh_count == 1);
    // Vertex, index, and morph arenas each own one GPU buffer.
    assert(device.value()->live_resource_count() == baseline + 3);

    const renderer::StaticMeshUploadDesc upload{"test_triangle",
                                                triangle_vertices,
                                                triangle_indices,
                                                {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}};
    auto mesh = manager.create_mesh(upload);
    assert(mesh);
    const auto* view = manager.find_exact(mesh.value());
    assert(view != nullptr);
    assert(view->vertex_count == 3);
    assert(view->index_count == 3);
    assert(view->vertices.buffer.is_valid());
    assert(view->indices.buffer.is_valid());
    assert(!view->fallback);
    assert(manager.stats().resident_mesh_count == 2);

    auto duplicate = manager.create_mesh(upload);
    assert(duplicate);
    assert(duplicate.value() == mesh.value());
    assert(manager.stats().uploaded_mesh_count == 2); // fallback plus the triangle
    assert(manager.stats().cache_hit_count == 1);
    assert(manager.find_exact(mesh.value())->reference_count == 2);

    assert(manager.release(mesh.value()));
    assert(manager.find_exact(mesh.value()) != nullptr);
    assert(manager.find_exact(mesh.value())->reference_count == 1);
    assert(manager.release(duplicate.value()));
    assert(manager.find_exact(mesh.value()) == nullptr);
    const auto* fallback = manager.find(mesh.value());
    assert(fallback != nullptr);
    assert(fallback->handle == manager.fallback_mesh());
    assert(fallback->fallback);
    assert(manager.stats().fallback_resolution_count == 1);
    assert(!manager.release(mesh.value()));

    auto replacement = manager.create_mesh(upload);
    assert(replacement);
    assert(replacement.value().index == mesh.value().index);
    assert(replacement.value().generation != mesh.value().generation);

    assert(manager.shutdown());
    assert(device.value()->live_resource_count() == baseline);
}

void test_invalid_mesh_rejected_without_allocating() {
    renderer::rhi::RenderDeviceDesc desc;
    auto device = renderer::rhi::create_render_device(desc);
    assert(device);
    renderer::MeshManager manager(*device.value());
    renderer::MeshManagerConfig config;
    config.vertex_initial_bytes = 4096;
    config.vertex_maximum_bytes = 8192;
    config.index_initial_bytes = 4096;
    config.index_maximum_bytes = 8192;
    assert(manager.initialize(config));
    const auto resources = device.value()->live_resource_count();

    constexpr std::array<std::uint32_t, 3> invalid_indices{0, 1, 99};
    auto invalid = manager.create_mesh({"invalid",
                                        triangle_vertices,
                                        invalid_indices,
                                        {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}});
    assert(!invalid);
    assert(invalid.error().code == "mesh_manager.index_out_of_bounds");
    assert(device.value()->live_resource_count() == resources);
    assert(manager.shutdown());
}

void test_mesh_views_stay_valid_across_growth() {
    renderer::rhi::RenderDeviceDesc desc;
    auto device = renderer::rhi::create_render_device(desc);
    assert(device);

    renderer::MeshManager manager(*device.value());
    renderer::MeshManagerConfig config;
    config.vertex_initial_bytes = 128U * 1024U;
    config.vertex_maximum_bytes = 1024U * 1024U;
    config.index_initial_bytes = 128U * 1024U;
    config.index_maximum_bytes = 1024U * 1024U;
    assert(manager.initialize(config));

    auto first = manager.create_mesh(
        {"m0", triangle_vertices, triangle_indices, {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}});
    assert(first);
    const auto* first_view = manager.find_exact(first.value());
    assert(first_view != nullptr);
    assert(first_view->id == "m0");

    for (std::uint32_t index = 1; index <= 512; ++index) {
        auto created = manager.create_mesh({"m" + std::to_string(index),
                                            triangle_vertices,
                                            triangle_indices,
                                            {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}});
        assert(created);
    }

    const auto* reacquired = manager.find_exact(first.value());
    assert(reacquired == first_view);
    assert(reacquired->id == "m0");
    assert(reacquired->vertex_count == triangle_vertices.size());
    assert(reacquired->index_count == triangle_indices.size());
    assert(manager.shutdown());
}

void test_skinned_model_primitive_upload() {
    using namespace heartstead;
    assets::ModelAsset model;
    model.vertices = {
        {{-1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
        {{1.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {1, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
        {{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 1, 0, 0}, {0.5F, 0.5F, 0.0F, 0.0F}},
    };
    model.indices = {0, 1, 2};
    model.nodes = {
        {"mesh", assets::no_model_index, {}},
        {"joint_0", 0, {}},
        {"joint_1", 1, {}},
    };
    model.primitives = {{"body", 0, 3, 0, 3, 0, 0}};
    model.skins = {{"body_skin", 1, {1, 2}, {math::Mat4f::identity(), math::Mat4f::identity()}}};
    model.bounds = {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}};
    assert(assets::validate_model_asset(model));

    renderer::rhi::RenderDeviceDesc desc;
    auto device = renderer::rhi::create_render_device(desc);
    assert(device);
    renderer::MeshManager manager(*device.value());
    renderer::MeshManagerConfig config;
    config.vertex_initial_bytes = 4096;
    config.vertex_maximum_bytes = 8192;
    config.index_initial_bytes = 4096;
    config.index_maximum_bytes = 8192;
    assert(manager.initialize(config));
    auto mesh = manager.create_model_primitive("animated_body", model, 0);
    assert(mesh);
    const auto* view = manager.find_exact(mesh.value());
    assert(view != nullptr);
    assert(view->vertex_count == 3);
    assert(view->index_count == 3);
    assert(view->skin_joint_count == 2);
    assert(view->vertices.size == sizeof(renderer::GpuStaticMeshVertex) * 3);
    assert(manager.create_model_primitive("animated_body", model, 0).value() == mesh.value());
    assert(!manager.create_model_primitive("missing", model, 1));
    assert(manager.shutdown());
}

} // namespace

int main() {
    test_mesh_upload_cache_fallback_and_release();
    test_invalid_mesh_rejected_without_allocating();
    test_mesh_views_stay_valid_across_growth();
    test_skinned_model_primitive_upload();
    return 0;
}
