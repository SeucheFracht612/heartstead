#include "engine/ui/ui_skin_prototype.hpp"

#include "engine/modding/prototype_registry.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>
#include <utility>

namespace heartstead::ui {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

template <std::size_t Count>
[[nodiscard]] core::Result<std::array<float, Count>>
parse_float_list(std::string_view value, std::string_view field_name) {
    std::array<float, Count> result{};
    std::size_t first = 0;
    for (std::size_t index = 0; index < Count; ++index) {
        const auto last = value.find(',', first);
        const auto part = value.substr(
            first, last == std::string_view::npos ? value.size() - first : last - first);
        const auto [end, error] =
            std::from_chars(part.data(), part.data() + part.size(), result[index]);
        if (error != std::errc{} || end != part.data() + part.size()) {
            return core::Result<std::array<float, Count>>::failure(
                "ui_skin_prototype.invalid_number",
                std::string(field_name) + " contains an invalid number");
        }
        if (index + 1 < Count) {
            if (last == std::string_view::npos) {
                return core::Result<std::array<float, Count>>::failure(
                    "ui_skin_prototype.invalid_list",
                    std::string(field_name) + " has too few values");
            }
            first = last + 1;
        } else if (last != std::string_view::npos) {
            return core::Result<std::array<float, Count>>::failure(
                "ui_skin_prototype.invalid_list",
                std::string(field_name) + " has too many values");
        }
    }
    return core::Result<std::array<float, Count>>::success(result);
}

[[nodiscard]] core::Result<std::uint16_t> parse_layer(std::string_view value) {
    std::uint16_t result = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return core::Result<std::uint16_t>::failure(
            "ui_skin_prototype.invalid_texture_layer",
            "texture_layer must be an unsigned 16-bit integer");
    }
    return core::Result<std::uint16_t>::success(result);
}

} // namespace

core::Result<UiSkin>
ui_skin_from_panel_prototypes(std::span<const modding::GenericPrototype* const> prototypes) {
    if (prototypes.empty()) {
        return core::Result<UiSkin>::success(UiSkin::storybook_default());
    }
    UiSkin skin;
    for (const auto* prototype : prototypes) {
        if (prototype == nullptr || prototype->kind != modding::PrototypeKinds::ui_panel) {
            return core::Result<UiSkin>::failure(
                "ui_skin_prototype.kind_mismatch",
                "UI skin loader received a missing or non-ui_panel prototype");
        }
        const auto* skin_name = field(*prototype, "skin_name");
        const auto* atlas_region = field(*prototype, "atlas_region");
        const auto* texture_layer = field(*prototype, "texture_layer");
        const auto* uv_minimum = field(*prototype, "uv_minimum");
        const auto* uv_maximum = field(*prototype, "uv_maximum");
        const auto* source_size = field(*prototype, "source_size_pixels");
        const auto* border = field(*prototype, "border_pixels");
        if (skin_name == nullptr || atlas_region == nullptr || texture_layer == nullptr ||
            uv_minimum == nullptr || uv_maximum == nullptr || source_size == nullptr ||
            border == nullptr || skin_name->empty() || atlas_region->empty()) {
            return core::Result<UiSkin>::failure(
                "ui_skin_prototype.missing_field",
                prototype->id.value() + " is missing a required UI skin field");
        }
        auto parsed_layer = parse_layer(*texture_layer);
        auto parsed_uv_minimum = parse_float_list<2>(*uv_minimum, "uv_minimum");
        auto parsed_uv_maximum = parse_float_list<2>(*uv_maximum, "uv_maximum");
        auto parsed_source_size = parse_float_list<2>(*source_size, "source_size_pixels");
        auto parsed_border = parse_float_list<4>(*border, "border_pixels");
        if (!parsed_layer || !parsed_uv_minimum || !parsed_uv_maximum || !parsed_source_size ||
            !parsed_border) {
            const auto& error = !parsed_layer        ? parsed_layer.error()
                                : !parsed_uv_minimum ? parsed_uv_minimum.error()
                                : !parsed_uv_maximum ? parsed_uv_maximum.error()
                                : !parsed_source_size
                                    ? parsed_source_size.error()
                                    : parsed_border.error();
            return core::Result<UiSkin>::failure(error.code, error.message);
        }
        if (skin.find_region(*atlas_region) == nullptr) {
            auto status = skin.add_region(
                {*atlas_region,
                 parsed_layer.value(),
                 {parsed_uv_minimum.value()[0], parsed_uv_minimum.value()[1]},
                 {parsed_uv_maximum.value()[0], parsed_uv_maximum.value()[1]},
                 {parsed_source_size.value()[0], parsed_source_size.value()[1]}});
            if (!status) {
                return core::Result<UiSkin>::failure(status.error().code,
                                                     status.error().message);
            }
        }
        auto status = skin.add_nine_slice(
            {*skin_name,
             *atlas_region,
             {parsed_border.value()[0], parsed_border.value()[1], parsed_border.value()[2],
              parsed_border.value()[3]}});
        if (!status) {
            return core::Result<UiSkin>::failure(status.error().code, status.error().message);
        }
    }
    return core::Result<UiSkin>::success(std::move(skin));
}

} // namespace heartstead::ui
