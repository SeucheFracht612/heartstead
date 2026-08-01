#include "engine/core/file_io.hpp"
#include "engine/renderer/ui/ui_font.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string>

int main() {
    using namespace heartstead;
    const auto source = core::read_binary_file(
        std::filesystem::path{HEARTSTEAD_TEST_SOURCE_DIR} /
        "mods/base/assets/fonts/heartstead-ui.ttf");
    assert(source);
    auto first = renderer::UiFont::build(source.value());
    auto second = renderer::UiFont::build(source.value());
    assert(first && second);
    assert(first.value().atlas_width() == second.value().atlas_width());
    assert(first.value().atlas_height() == second.value().atlas_height());
    assert(std::ranges::equal(first.value().atlas_sdf(), second.value().atlas_sdf()));

    auto layout = first.value().layout_utf8(
        "Heartstead - Gr\xc3\xbc\xc3\x9f \xce\x94\xd0\x96", {10.0F, 20.0F}, 24.0F);
    assert(layout);
    assert(layout.value().glyphs.size() >= 16);
    assert(layout.value().replacement_count == 0);
    assert(layout.value().extent_pixels.x > 100.0F);

    const std::string malformed{"A\xf0\x28\x8c\x28Z", 6};
    layout = first.value().layout_utf8(malformed, {}, 16.0F);
    assert(layout);
    assert(layout.value().replacement_count >= 1);
    assert(!renderer::UiFont::build({}));
    return 0;
}
