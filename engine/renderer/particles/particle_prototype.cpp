#include "engine/renderer/particles/particle_prototype.hpp"

#include "engine/modding/prototype_registry.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace heartstead::renderer {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

template <typename Value>
[[nodiscard]] core::Result<Value> number_field(const modding::GenericPrototype& prototype,
                                               std::string_view key, Value fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<Value>::success(fallback);
    }
    Value value{};
    const auto [end, error] =
        std::from_chars(text->data(), text->data() + text->size(), value);
    if (error != std::errc{} || end != text->data() + text->size() ||
        (std::is_floating_point_v<Value> && !std::isfinite(value))) {
        return core::Result<Value>::failure(
            "particle_prototype.invalid_number",
            std::string(key) + " must be a finite number");
    }
    return core::Result<Value>::success(value);
}

[[nodiscard]] core::Result<std::array<float, 4>>
color_field(const modding::GenericPrototype& prototype, std::string_view key,
            std::array<float, 4> fallback) {
    const auto* text = field(prototype, key);
    if (text == nullptr) {
        return core::Result<std::array<float, 4>>::success(fallback);
    }
    std::array<float, 4> color{};
    std::string_view remaining = *text;
    for (std::size_t index = 0; index < color.size(); ++index) {
        const auto separator = remaining.find(',');
        const auto token = remaining.substr(0, separator);
        const auto [end, error] =
            std::from_chars(token.data(), token.data() + token.size(), color[index]);
        if (error != std::errc{} || end != token.data() + token.size() ||
            !std::isfinite(color[index]) ||
            (separator == std::string_view::npos) != (index + 1U == color.size())) {
            return core::Result<std::array<float, 4>>::failure(
                "particle_prototype.invalid_color",
                std::string(key) + " must contain four comma-separated finite values");
        }
        if (separator != std::string_view::npos) {
            remaining.remove_prefix(separator + 1U);
        }
    }
    return core::Result<std::array<float, 4>>::success(color);
}

} // namespace

core::Result<ParticlePrototype>
particle_prototype_from_generic(const modding::GenericPrototype& prototype) {
    if (prototype.kind != modding::PrototypeKinds::particle) {
        return core::Result<ParticlePrototype>::failure(
            "particle_prototype.invalid_kind", "prototype kind must be particle");
    }
    auto material_group = number_field<std::uint16_t>(prototype, "material_group", 0);
    auto lifetime_min = number_field<float>(prototype, "lifetime_min_seconds", 0.5F);
    auto lifetime_max = number_field<float>(prototype, "lifetime_max_seconds", 1.0F);
    auto speed_min = number_field<float>(prototype, "speed_min", 0.0F);
    auto speed_max = number_field<float>(prototype, "speed_max", 1.0F);
    auto spread = number_field<float>(prototype, "direction_spread", 1.0F);
    auto gravity = number_field<float>(prototype, "gravity", -9.81F);
    auto drag = number_field<float>(prototype, "drag", 0.0F);
    auto size_min = number_field<float>(prototype, "size_min", 0.1F);
    auto size_max = number_field<float>(prototype, "size_max", 0.2F);
    auto end_size = number_field<float>(prototype, "end_size_multiplier", 1.0F);
    auto start_color =
        color_field(prototype, "start_color", {1.0F, 1.0F, 1.0F, 1.0F});
    auto end_color = color_field(prototype, "end_color", {1.0F, 1.0F, 1.0F, 0.0F});
    auto atlas_columns = number_field<std::uint16_t>(prototype, "atlas_columns", 1);
    auto atlas_rows = number_field<std::uint16_t>(prototype, "atlas_rows", 1);
    auto atlas_frames = number_field<std::uint16_t>(prototype, "atlas_frame_count", 1);
    auto atlas_fps = number_field<float>(prototype, "atlas_frames_per_second", 0.0F);
    if (!material_group || !lifetime_min || !lifetime_max || !speed_min || !speed_max ||
        !spread || !gravity || !drag || !size_min || !size_max || !end_size || !start_color ||
        !end_color || !atlas_columns || !atlas_rows || !atlas_frames || !atlas_fps ||
        material_group.value() > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<ParticlePrototype>::failure(
            "particle_prototype.invalid_fields", "particle prototype fields are invalid");
    }

    ParticlePrototype result;
    result.id = prototype.id;
    result.material_group = static_cast<std::uint8_t>(material_group.value());
    result.lifetime_min_seconds = lifetime_min.value();
    result.lifetime_max_seconds = lifetime_max.value();
    result.speed_min = speed_min.value();
    result.speed_max = speed_max.value();
    result.direction_spread = spread.value();
    result.gravity = gravity.value();
    result.drag = drag.value();
    result.size_min = size_min.value();
    result.size_max = size_max.value();
    result.end_size_multiplier = end_size.value();
    result.start_color = start_color.value();
    result.end_color = end_color.value();
    result.atlas_columns = atlas_columns.value();
    result.atlas_rows = atlas_rows.value();
    result.atlas_frame_count = atlas_frames.value();
    result.atlas_frames_per_second = atlas_fps.value();
    auto status = result.validate();
    if (!status) {
        return core::Result<ParticlePrototype>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<ParticlePrototype>::success(result);
}

} // namespace heartstead::renderer
