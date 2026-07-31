#include "apps/asset_lab/asset_lab_mode.hpp"

#include "engine/assets/cooked_asset_store.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <array>
#include <cassert>
#include <filesystem>

int main() {
    using namespace heartstead;
    using namespace heartstead::asset_lab;

    assert(parse_preview_kind("visual-prefab") == PreviewKind::visual_prefab);
    assert(parse_preview_kind("texture") == PreviewKind::texture);
    assert(!parse_preview_kind("unknown"));
    assert(parse_lighting_preset("fire-lit-interior") == LightingPreset::fire_lit_interior);
    assert(parse_asset_lab_debug_view("skin-weights") == renderer::LightingDebugView::skin_weights);
    assert(!parse_asset_lab_debug_view("wireframe"));

    constexpr std::array presets{
        LightingPreset::studio,     LightingPreset::overcast,     LightingPreset::noon,
        LightingPreset::sunset,     LightingPreset::night,        LightingPreset::fire_lit_interior,
        LightingPreset::cave,       LightingPreset::forest_shade, LightingPreset::rain_wetness,
        LightingPreset::snow_frost, LightingPreset::underwater,
    };
    for (const auto preset : presets) {
        assert(renderer::rhi::validate_render_environment(asset_lab_lighting_environment(preset)));
    }

    const auto content =
        content::ContentValidation::validate(std::filesystem::path{HEARTSTEAD_TEST_SOURCE_DIR});
    assert(!content.has_errors());
    auto store =
        assets::CookedAssetStore::load(std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR});
    assert(store);

    AssetLabModeConfig prefab;
    prefab.content = &content;
    prefab.preview = PreviewKind::visual_prefab;
    prefab.selection_id = "base:visuals/player";
    auto inspected = inspect_asset_lab_selection(prefab, store.value());
    assert(inspected);
    assert(inspected.value().format == "heartstead.model.v5");
    assert(inspected.value().runtime_bytes > 0U);
    assert(!inspected.value().details.empty());

    AssetLabModeConfig texture;
    texture.content = &content;
    texture.preview = PreviewKind::texture;
    texture.selection_id = "base:textures/voxels/grass.png";
    inspected = inspect_asset_lab_selection(texture, store.value());
    assert(inspected);
    assert(inspected.value().format.starts_with("heartstead.texture.v2/"));
    assert(inspected.value().runtime_bytes > 0U);

    prefab.forced_lod = 99U;
    assert(!inspect_asset_lab_selection(prefab, store.value()));
    return 0;
}
