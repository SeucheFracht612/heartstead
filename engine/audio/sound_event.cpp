#include "engine/audio/sound_event.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace heartstead::audio {

namespace {

[[nodiscard]] const std::string* field(const modding::GenericPrototype& prototype,
                                       std::string_view key) {
    const auto found = prototype.fields.find(std::string(key));
    return found == prototype.fields.end() ? nullptr : &found->second;
}

template <typename T>
[[nodiscard]] core::Result<T> integer_field(const modding::GenericPrototype& prototype,
                                            std::string_view key, T fallback) {
    const auto* value = field(prototype, key);
    if (value == nullptr) {
        return core::Result<T>::success(fallback);
    }
    T result{};
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), result);
    if (error != std::errc{} || end != value->data() + value->size()) {
        return core::Result<T>::failure("sound_event.invalid_integer",
                                        std::string(key) + " must be an integer");
    }
    return core::Result<T>::success(result);
}

[[nodiscard]] core::Result<float> float_field(const modding::GenericPrototype& prototype,
                                              std::string_view key, float fallback) {
    const auto* value = field(prototype, key);
    if (value == nullptr) {
        return core::Result<float>::success(fallback);
    }
    float result = 0.0F;
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), result);
    if (error != std::errc{} || end != value->data() + value->size() || !std::isfinite(result)) {
        return core::Result<float>::failure("sound_event.invalid_number",
                                            std::string(key) + " must be finite");
    }
    return core::Result<float>::success(result);
}

[[nodiscard]] core::Result<bool> bool_field(const modding::GenericPrototype& prototype,
                                            std::string_view key, bool fallback) {
    const auto* value = field(prototype, key);
    if (value == nullptr) {
        return core::Result<bool>::success(fallback);
    }
    if (*value == "true") {
        return core::Result<bool>::success(true);
    }
    if (*value == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("sound_event.invalid_bool",
                                       std::string(key) + " must be true or false");
}

} // namespace

core::Status SoundEventDefinition::validate() const {
    if (!prototype_id.is_valid()) {
        return core::Status::failure("sound_event.invalid_id",
                                     "sound event prototype id must be valid");
    }
    if (asset_id.empty() || !core::PrototypeId::parse(asset_id).has_value()) {
        return core::Status::failure("sound_event.invalid_asset",
                                     "sound event asset must be a logical namespace:path id");
    }
    if (!std::isfinite(gain) || gain < 0.0F) {
        return core::Status::failure("sound_event.invalid_gain",
                                     "sound event gain must be finite and non-negative");
    }
    if (!std::isfinite(minimum_distance) || !std::isfinite(maximum_distance) ||
        minimum_distance < 0.0F || maximum_distance <= minimum_distance) {
        return core::Status::failure(
            "sound_event.invalid_distance",
            "sound event maximum distance must be greater than its non-negative minimum");
    }
    if (!std::isfinite(cone_inner_angle_degrees) || !std::isfinite(cone_outer_angle_degrees) ||
        cone_inner_angle_degrees < 0.0F || cone_outer_angle_degrees < cone_inner_angle_degrees ||
        cone_outer_angle_degrees > 360.0F) {
        return core::Status::failure(
            "sound_event.invalid_cone",
            "sound event cone angles must be ordered within zero to 360 degrees");
    }
    if (!std::isfinite(cone_outer_gain) || cone_outer_gain < 0.0F || cone_outer_gain > 1.0F) {
        return core::Status::failure("sound_event.invalid_cone_gain",
                                     "sound event outer cone gain must be within zero to one");
    }
    if (maximum_instances == 0) {
        return core::Status::failure("sound_event.invalid_instance_limit",
                                     "sound event maximum instances must be non-zero");
    }
    if (!spatialized &&
        (cone_inner_angle_degrees != 360.0F || cone_outer_angle_degrees != 360.0F)) {
        return core::Status::failure("sound_event.non_spatial_cone",
                                     "non-spatial sound events cannot declare a direction cone");
    }
    return core::Status::ok();
}

core::Status SoundEventRegistry::add(SoundEventDefinition definition) {
    auto status = definition.validate();
    if (!status) {
        return status;
    }
    const auto key = definition.prototype_id.value();
    if (by_id_.contains(key)) {
        return core::Status::failure("sound_event.duplicate_id",
                                     "duplicate sound event prototype id: " + key);
    }
    by_id_.emplace(key, definitions_.size());
    definitions_.push_back(std::move(definition));
    return core::Status::ok();
}

const SoundEventDefinition* SoundEventRegistry::find(const core::PrototypeId& id) const noexcept {
    const auto found = by_id_.find(id.value());
    return found == by_id_.end() ? nullptr : &definitions_[found->second];
}

std::size_t SoundEventRegistry::size() const noexcept {
    return definitions_.size();
}

const std::vector<SoundEventDefinition>& SoundEventRegistry::definitions() const noexcept {
    return definitions_;
}

core::Result<AudioBus> audio_bus_from_name(std::string_view name) {
    if (name == "master") {
        return core::Result<AudioBus>::success(AudioBus::master);
    }
    if (name == "music") {
        return core::Result<AudioBus>::success(AudioBus::music);
    }
    if (name == "sfx") {
        return core::Result<AudioBus>::success(AudioBus::sfx);
    }
    if (name == "ambient") {
        return core::Result<AudioBus>::success(AudioBus::ambient);
    }
    return core::Result<AudioBus>::failure("sound_event.invalid_bus",
                                           "sound event bus is not recognized");
}

core::Result<SoundEventDefinition>
sound_event_definition_from_prototype(const modding::GenericPrototype& prototype,
                                      const assets::AssetCatalog& assets) {
    if (prototype.kind != modding::PrototypeKinds::sound_event) {
        return core::Result<SoundEventDefinition>::failure("sound_event.invalid_kind",
                                                           "prototype kind must be sound_event");
    }
    const auto* asset_id = field(prototype, "asset");
    if (asset_id == nullptr || asset_id->empty()) {
        return core::Result<SoundEventDefinition>::failure(
            "sound_event.missing_asset", "sound event prototype must declare an asset");
    }
    const auto* asset = assets.find_active(*asset_id);
    if (asset == nullptr) {
        return core::Result<SoundEventDefinition>::failure(
            "sound_event.asset_missing", "sound event asset is not present in the active catalog");
    }
    if (asset->kind != assets::AssetKind::sound && asset->kind != assets::AssetKind::music) {
        return core::Result<SoundEventDefinition>::failure(
            "sound_event.asset_kind", "sound event asset must resolve to a sound or music asset");
    }

    auto bus =
        audio_bus_from_name(field(prototype, "bus") == nullptr ? "sfx" : *field(prototype, "bus"));
    auto gain = float_field(prototype, "gain", 1.0F);
    auto minimum_distance = float_field(prototype, "minimum_distance", 1.0F);
    auto maximum_distance = float_field(prototype, "maximum_distance", 32.0F);
    auto cone_inner = float_field(prototype, "cone_inner_angle_degrees", 360.0F);
    auto cone_outer = float_field(prototype, "cone_outer_angle_degrees", 360.0F);
    auto cone_outer_gain = float_field(prototype, "cone_outer_gain", 1.0F);
    auto maximum_instances = integer_field<std::uint32_t>(prototype, "maximum_instances", 8U);
    auto priority = integer_field<std::uint16_t>(prototype, "priority", 128U);
    auto spatialized = bool_field(prototype, "spatialized", true);
    auto looping = bool_field(prototype, "looping", false);
    auto streaming = bool_field(prototype, "streaming", asset->kind == assets::AssetKind::music);
    if (!bus || !gain || !minimum_distance || !maximum_distance || !cone_inner || !cone_outer ||
        !cone_outer_gain || !maximum_instances || !priority || !spatialized || !looping ||
        !streaming) {
        const auto& error = !bus                 ? bus.error()
                            : !gain              ? gain.error()
                            : !minimum_distance  ? minimum_distance.error()
                            : !maximum_distance  ? maximum_distance.error()
                            : !cone_inner        ? cone_inner.error()
                            : !cone_outer        ? cone_outer.error()
                            : !cone_outer_gain   ? cone_outer_gain.error()
                            : !maximum_instances ? maximum_instances.error()
                            : !priority          ? priority.error()
                            : !spatialized       ? spatialized.error()
                            : !looping           ? looping.error()
                                                 : streaming.error();
        return core::Result<SoundEventDefinition>::failure(error.code, error.message);
    }
    if (priority.value() > std::numeric_limits<std::uint8_t>::max()) {
        return core::Result<SoundEventDefinition>::failure(
            "sound_event.invalid_priority", "sound event priority must be between zero and 255");
    }

    SoundEventDefinition result;
    result.prototype_id = prototype.id;
    result.asset_id = *asset_id;
    result.bus = bus.value();
    result.gain = gain.value();
    result.minimum_distance = minimum_distance.value();
    result.maximum_distance = maximum_distance.value();
    result.cone_inner_angle_degrees = cone_inner.value();
    result.cone_outer_angle_degrees = cone_outer.value();
    result.cone_outer_gain = cone_outer_gain.value();
    result.maximum_instances = maximum_instances.value();
    result.priority = static_cast<std::uint8_t>(priority.value());
    result.spatialized = spatialized.value();
    result.looping = looping.value();
    result.streaming = streaming.value();
    auto status = result.validate();
    if (!status) {
        return core::Result<SoundEventDefinition>::failure(status.error().code,
                                                           status.error().message);
    }
    return core::Result<SoundEventDefinition>::success(std::move(result));
}

core::Result<SoundEventRegistry>
sound_event_registry_from_prototypes(const modding::PrototypeRegistry& prototypes,
                                     const assets::AssetCatalog& assets) {
    SoundEventRegistry result;
    for (const auto* prototype :
         prototypes.prototypes_of_kind(modding::PrototypeKinds::sound_event)) {
        auto definition = sound_event_definition_from_prototype(*prototype, assets);
        if (!definition) {
            return core::Result<SoundEventRegistry>::failure(definition.error().code,
                                                             definition.error().message);
        }
        auto status = result.add(std::move(definition).value());
        if (!status) {
            return core::Result<SoundEventRegistry>::failure(status.error().code,
                                                             status.error().message);
        }
    }
    return core::Result<SoundEventRegistry>::success(std::move(result));
}

} // namespace heartstead::audio
