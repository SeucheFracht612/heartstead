#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/texture_asset.hpp"
#include "engine/renderer/assets/texture_manager.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

heartstead::assets::ImageAsset make_color_image() {
    heartstead::assets::ImageAsset image;
    image.width = 8;
    image.height = 8;
    image.rgba8.resize(8U * 8U * 4U);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 4U;
            image.rgba8[offset] = static_cast<std::uint8_t>(x * 31U);
            image.rgba8[offset + 1U] = static_cast<std::uint8_t>(y * 31U);
            image.rgba8[offset + 2U] = static_cast<std::uint8_t>((x + y) * 15U);
            image.rgba8[offset + 3U] = ((x + y) % 3U) == 0U ? 0U : 255U;
        }
    }
    return image;
}

void test_deterministic_bc7_and_codec() {
    const auto image = make_color_image();
    heartstead::assets::TextureCookSettings settings;
    settings.role = heartstead::assets::TextureRole::mask;
    settings.color_space = heartstead::assets::TextureAssetColorSpace::srgb;
    settings.preserve_alpha_coverage = true;
    settings.alpha_cutoff = 0.5F;

    auto first = heartstead::assets::cook_texture_asset(image, settings);
    auto second = heartstead::assets::cook_texture_asset(image, settings);
    assert(first);
    assert(second);
    assert(first.value() == second.value());
    assert(first.value().format == heartstead::assets::TextureAssetFormat::bc7_rgba);
    assert(first.value().mips.size() == 4);
    assert(first.value().mips.back().width == 1);
    assert(first.value().mips.back().height == 1);
    assert(first.value().gpu_memory_bytes() == 112);

    auto encoded = heartstead::assets::encode_texture_asset(first.value());
    assert(encoded);
    auto decoded = heartstead::assets::decode_texture_asset(encoded.value());
    assert(decoded);
    assert(decoded.value() == first.value());
    for (std::size_t size = 0; size < encoded.value().size(); ++size) {
        assert(!heartstead::assets::decode_texture_asset(
            std::span<const std::uint8_t>{encoded.value().data(), size}));
    }

    auto upload =
        heartstead::renderer::texture_upload_desc_from_asset("test:mask", decoded.value());
    assert(upload);
    assert(upload.value().cooked_mip_levels == 4);
    assert(upload.value().cooked_bytes.size() == 112);
    assert(upload.value().cooked_format ==
           heartstead::renderer::rhi::RenderImageFormat::bc7_rgba_srgb);
}

void test_normal_mips_and_rgba_fallback() {
    auto image = make_color_image();
    for (std::size_t offset = 0; offset < image.rgba8.size(); offset += 4U) {
        image.rgba8[offset] = 128;
        image.rgba8[offset + 1U] = 128;
        image.rgba8[offset + 2U] = 255;
        image.rgba8[offset + 3U] = 255;
    }
    heartstead::assets::TextureCookSettings normal;
    normal.role = heartstead::assets::TextureRole::normal;
    normal.color_space = heartstead::assets::TextureAssetColorSpace::linear;
    auto cooked = heartstead::assets::cook_texture_asset(image, normal);
    assert(cooked);
    assert(cooked.value().format == heartstead::assets::TextureAssetFormat::bc5_rg);
    assert(cooked.value().gpu_memory_bytes() == 112);

    heartstead::assets::TextureCookSettings fallback;
    fallback.compression = heartstead::assets::TextureCompressionMode::rgba8;
    auto rgba = heartstead::assets::cook_texture_asset(image, fallback);
    assert(rgba);
    assert(rgba.value().format == heartstead::assets::TextureAssetFormat::rgba8);
    assert(rgba.value().gpu_memory_bytes() == 340);
}

void test_role_inference_and_sidecar() {
    const auto inferred =
        heartstead::assets::infer_texture_cook_settings("textures/props/hammer_normal.png");
    assert(inferred.role == heartstead::assets::TextureRole::normal);
    assert(inferred.color_space == heartstead::assets::TextureAssetColorSpace::linear);
    assert(inferred.compression == heartstead::assets::TextureCompressionMode::bc5);
    const auto voxel = heartstead::assets::infer_texture_cook_settings("textures/voxels/grass.png");
    assert(voxel.compression == heartstead::assets::TextureCompressionMode::rgba8);

    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("heartstead_texture_asset_tests_" + std::to_string(nonce));
    std::filesystem::create_directories(root);
    const auto texture = root / "leaf.png";
    const auto sidecar = heartstead::assets::texture_cook_sidecar_path(texture);
    {
        std::ofstream output(texture, std::ios::binary);
        assert(output);
        output << "source";
    }
    {
        std::ofstream output(sidecar);
        assert(output);
        output << "role = mask\n"
                  "color_space = linear\n"
                  "compression = bc7\n"
                  "generate_mips = true\n"
                  "preserve_alpha_coverage = true\n"
                  "alpha_cutoff = 0.42\n";
    }
    auto loaded = heartstead::assets::load_texture_cook_settings(texture);
    assert(loaded);
    assert(loaded.value().role == heartstead::assets::TextureRole::mask);
    assert(loaded.value().color_space == heartstead::assets::TextureAssetColorSpace::linear);
    assert(loaded.value().compression == heartstead::assets::TextureCompressionMode::bc7);
    assert(loaded.value().preserve_alpha_coverage);
    assert(loaded.value().alpha_cutoff > 0.419F);

    heartstead::assets::AssetCatalog catalog;
    auto indexed = heartstead::assets::AssetCatalogBuilder::index_directory(
        catalog, root, "base", heartstead::assets::AssetSourceKind::mod, "base", 0);
    assert(!indexed.has_errors());
    assert(catalog.count_kind(heartstead::assets::AssetKind::texture) == 1);
    assert(catalog.count_kind(heartstead::assets::AssetKind::data) == 1);
    assert(heartstead::assets::discover_asset_dependencies(catalog));
    const auto* texture_record = catalog.find_active("base:leaf.png");
    assert(texture_record != nullptr);
    assert(texture_record->dependencies.size() == 1);
    assert(texture_record->dependencies[0].to_string() == "base:leaf.png.texture.toml");

    std::error_code error;
    std::filesystem::remove_all(root, error);
    assert(!error);
}

} // namespace

int main() {
    test_deterministic_bc7_and_codec();
    test_normal_mips_and_rgba_fallback();
    test_role_inference_and_sidecar();
    return 0;
}
