#include "engine/math/matrix.hpp"
#include "engine/renderer/scene/render_scene.hpp"

#include <array>
#include <cassert>
#include <cmath>

namespace {

using namespace heartstead;

[[nodiscard]] world::WorldPosition anchored(world::BlockCoord anchor) {
    auto result = world::WorldPosition::from_anchor(anchor, {});
    assert(result);
    return result.value();
}

[[nodiscard]] renderer::RenderObjectProxy make_object(world::WorldPosition anchor) {
    renderer::RenderObjectProxy object;
    object.anchor = anchor;
    object.mesh = renderer::RenderMeshHandle{1, 1};
    object.material = renderer::MaterialRuntimeHandle{1, 1};
    object.local_bounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    object.previous_transform.position = {0.0F, 0.0F, -10.0F};
    object.current_transform.position = {2.0F, 0.0F, -10.0F};
    return object;
}

void test_transform_matrix_and_bounds() {
    math::Transform3f transform;
    transform.position = {4.0F, 2.0F, -3.0F};
    transform.rotation_degrees.z = 90.0F;
    transform.scale = {2.0F, 1.0F, 1.0F};
    const auto matrix = math::transform_matrix(transform);
    const auto point = matrix * math::Vec4f{1.0F, 0.0F, 0.0F, 1.0F};
    assert(std::abs(point.x - 4.0F) < 0.0001F);
    assert(std::abs(point.y - 4.0F) < 0.0001F);
    assert(std::abs(point.z + 3.0F) < 0.0001F);

    const auto bounds = math::transform_bounds(matrix, {{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}});
    assert(bounds.is_valid());
    assert(std::abs(bounds.min.x - 3.0F) < 0.0001F);
    assert(std::abs(bounds.max.x - 5.0F) < 0.0001F);
    assert(std::abs(bounds.min.y - 0.0F) < 0.0001F);
    assert(std::abs(bounds.max.y - 4.0F) < 0.0001F);
}

void test_model_local_transform_is_shared_by_rendering_and_bounds() {
    renderer::RenderScene scene;
    auto object = make_object(anchored({0, 0, 0}));
    object.previous_transform.position = {1.0F, 0.0F, -10.0F};
    object.current_transform = object.previous_transform;
    object.model_transform = math::translation_matrix({2.0F, 3.0F, 0.0F});
    auto id = scene.create_object(object);
    assert(id);

    renderer::RenderCamera camera;
    assert(camera.update_matrices());
    auto extracted = scene.extract(camera, 1.0F);
    assert(extracted);
    assert(extracted.value().stats.visible_objects == 1);
    const auto& instance = extracted.value().batches.front().instances.front();
    assert(std::abs(instance.camera_relative_transform.at(0, 3) - 3.0F) < 0.0001F);
    assert(std::abs(instance.camera_relative_transform.at(1, 3) - 3.0F) < 0.0001F);
    assert(std::abs(instance.camera_relative_bounds.min.x - 2.5F) < 0.0001F);
    assert(std::abs(instance.camera_relative_bounds.max.y - 3.5F) < 0.0001F);
}

void test_generation_safe_retained_scene_and_interpolation() {
    constexpr std::int64_t coordinate = 8'000'000'000'000'000LL;
    renderer::RenderScene scene;
    auto object = make_object(anchored({coordinate, 0, coordinate}));
    auto id = scene.create_object(object);
    assert(id);
    assert(scene.find_object(id.value()) != nullptr);

    renderer::RenderCamera camera;
    camera.floating_origin.block = {coordinate, 0, coordinate};
    assert(camera.update_matrices());
    auto extracted = scene.extract(camera, 0.5F);
    assert(extracted);
    assert(extracted.value().stats.retained_objects == 1);
    assert(extracted.value().stats.visible_objects == 1);
    assert(extracted.value().stats.instance_batches == 1);
    assert(extracted.value().batches.front().instances.size() == 1);
    const auto& transform =
        extracted.value().batches.front().instances.front().camera_relative_transform;
    assert(std::abs(transform.at(0, 3) - 1.0F) < 0.0001F);
    assert(std::abs(transform.at(2, 3) + 10.0F) < 0.0001F);

    assert(scene.remove_object(id.value()));
    assert(scene.find_object(id.value()) == nullptr);
    assert(!scene.remove_object(id.value()));
    auto replacement = make_object(anchored({coordinate, 0, coordinate}));
    auto replacement_id = scene.create_object(replacement);
    assert(replacement_id);
    assert(replacement_id.value().index == id.value().index);
    assert(replacement_id.value().generation != id.value().generation);
    scene.clear();
    assert(scene.find_object(replacement_id.value()) == nullptr);
    auto after_clear = scene.create_object(make_object(anchored({coordinate, 0, coordinate})));
    assert(after_clear);
    assert(after_clear.value().generation != replacement_id.value().generation);
}

void test_parent_transforms_batching_culling_and_hidden_objects() {
    renderer::RenderScene scene;
    auto parent = make_object(anchored({0, 0, 0}));
    parent.previous_transform = {};
    parent.current_transform = {};
    parent.previous_transform.position.z = -12.0F;
    parent.current_transform.position.z = -12.0F;
    auto parent_id = scene.create_object(parent);
    assert(parent_id);

    auto child = make_object(anchored({999, 999, 999}));
    child.parent = parent_id.value();
    child.previous_transform.position = {2.0F, 0.0F, 0.0F};
    child.current_transform = child.previous_transform;
    auto child_id = scene.create_object(child);
    assert(child_id);

    auto culled = make_object(anchored({0, 0, 0}));
    culled.previous_transform.position = {10000.0F, 0.0F, -10.0F};
    culled.current_transform = culled.previous_transform;
    auto culled_id = scene.create_object(culled);
    assert(culled_id);

    auto hidden = make_object(anchored({0, 0, 0}));
    hidden.flags = renderer::RenderObjectFlags::hidden;
    auto hidden_id = scene.create_object(hidden);
    assert(hidden_id);

    renderer::RenderCamera camera;
    assert(camera.update_matrices());
    auto extracted = scene.extract(camera, 1.0F);
    assert(extracted);
    assert(extracted.value().stats.retained_objects == 4);
    assert(extracted.value().stats.visible_objects == 2);
    assert(extracted.value().stats.culled_objects == 1);
    assert(extracted.value().stats.hidden_objects == 1);
    assert(extracted.value().stats.instance_batches == 1);
    assert(extracted.value().batches.front().instances.size() == 2);
    const auto& child_instance = extracted.value().batches.front().instances[1];
    assert(child_instance.id == child_id.value());
    assert(std::abs(child_instance.camera_relative_transform.at(0, 3) - 2.0F) < 0.0001F);
    assert(std::abs(child_instance.camera_relative_transform.at(2, 3) + 12.0F) < 0.0001F);
    assert(!scene.remove_object(parent_id.value()));
    assert(scene.remove_object(child_id.value()));
    assert(scene.remove_object(parent_id.value()));
}

void test_teleport_rotation_light_and_batched_updates() {
    renderer::RenderScene scene;
    const auto object_id = scene.reserve_object_id();
    auto object = make_object(anchored({0, 0, 0}));
    object.id = object_id;
    object.previous_transform.rotation_degrees.y = 350.0F;
    object.current_transform.rotation_degrees.y = 10.0F;
    auto interpolated = renderer::interpolate_render_transform(object, 0.5F);
    assert(std::abs(interpolated.rotation_degrees.y - 360.0F) < 0.0001F);
    object.flags = renderer::RenderObjectFlags::teleport;
    interpolated = renderer::interpolate_render_transform(object, 0.25F);
    assert(interpolated.position == object.current_transform.position);

    const auto light_id = scene.reserve_light_id();
    renderer::RenderLightProxy light;
    light.id = light_id;
    light.anchor = anchored({0, 2, -4});
    light.radius = 12.0F;
    renderer::RenderSceneUpdate object_update;
    object_update.kind = renderer::RenderSceneUpdateKind::upsert_object;
    object_update.object = object;
    renderer::RenderSceneUpdate light_update;
    light_update.kind = renderer::RenderSceneUpdateKind::upsert_light;
    light_update.light = light;
    const std::array updates{object_update, light_update};
    assert(scene.apply(updates));
    renderer::RenderCamera camera;
    assert(camera.update_matrices());
    auto extracted = scene.extract(camera, 0.0F);
    assert(extracted);
    assert(extracted.value().lights.size() == 1);
    assert((extracted.value().lights.front().camera_relative_position ==
            math::Vec3f{0.0F, 2.0F, -4.0F}));

    renderer::RenderSceneUpdate object_removal;
    object_removal.kind = renderer::RenderSceneUpdateKind::remove_object;
    object_removal.object_id = object_id;
    renderer::RenderSceneUpdate light_removal;
    light_removal.kind = renderer::RenderSceneUpdateKind::remove_light;
    light_removal.light_id = light_id;
    const std::array removals{object_removal, light_removal};
    assert(scene.apply(removals));
    assert(scene.stats().retained_objects == 0);
    assert(scene.stats().retained_lights == 0);
}

void test_generation_safe_skin_palettes() {
    renderer::RenderScene scene;
    renderer::RenderSkinPaletteProxy palette;
    palette.joint_matrices = {math::Mat4f::identity(),
                              math::translation_matrix({0.0F, 1.0F, 0.0F})};
    auto palette_id = scene.create_skin_palette(palette);
    assert(palette_id);
    assert(scene.find_skin_palette(palette_id.value()) != nullptr);
    assert(scene.stats().retained_skin_palettes == 1);

    auto object = make_object(anchored({0, 0, 0}));
    object.skin_palette = palette_id.value();
    auto object_id = scene.create_object(object);
    assert(object_id);
    assert(!scene.remove_skin_palette(palette_id.value()));

    renderer::RenderCamera camera;
    assert(camera.update_matrices());
    auto extracted = scene.extract(camera, 1.0F);
    assert(extracted);
    assert(extracted.value().batches.front().instances.front().skin_palette == palette_id.value());

    auto updated = *scene.find_skin_palette(palette_id.value());
    updated.joint_matrices[1] = math::translation_matrix({0.0F, 2.0F, 0.0F});
    assert(scene.upsert_skin_palette(updated));
    assert(scene.find_skin_palette(palette_id.value())->joint_matrices[1] ==
           updated.joint_matrices[1]);

    assert(scene.remove_object(object_id.value()));
    assert(scene.remove_skin_palette(palette_id.value()));
    assert(scene.find_skin_palette(palette_id.value()) == nullptr);
    auto replacement = scene.create_skin_palette(palette);
    assert(replacement);
    assert(replacement.value().index == palette_id.value().index);
    assert(replacement.value().generation != palette_id.value().generation);

    auto stale_object = make_object(anchored({0, 0, 0}));
    stale_object.skin_palette = palette_id.value();
    assert(!scene.create_object(stale_object));

    auto invalid_palette = palette;
    invalid_palette.joint_matrices.clear();
    assert(!scene.create_skin_palette(invalid_palette));
    scene.clear();
    assert(scene.stats().retained_skin_palettes == 0);
}

} // namespace

int main() {
    test_transform_matrix_and_bounds();
    test_model_local_transform_is_shared_by_rendering_and_bounds();
    test_generation_safe_retained_scene_and_interpolation();
    test_parent_transforms_batching_culling_and_hidden_objects();
    test_teleport_rotation_light_and_batched_updates();
    test_generation_safe_skin_palettes();
    return 0;
}
