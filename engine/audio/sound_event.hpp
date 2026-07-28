#pragma once

#include "engine/assets/asset_catalog.hpp"
#include "engine/audio/audio_types.hpp"
#include "engine/core/result.hpp"
#include "engine/modding/generic_prototype.hpp"
#include "engine/modding/prototype_registry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::audio {

struct SoundEventDefinition {
    core::PrototypeId prototype_id;
    std::string asset_id;
    AudioBus bus = AudioBus::sfx;
    float gain = 1.0F;
    float minimum_distance = 1.0F;
    float maximum_distance = 32.0F;
    float cone_inner_angle_degrees = 360.0F;
    float cone_outer_angle_degrees = 360.0F;
    float cone_outer_gain = 1.0F;
    std::uint32_t maximum_instances = 8;
    std::uint8_t priority = 128;
    bool spatialized = true;
    bool looping = false;
    bool streaming = false;

    [[nodiscard]] core::Status validate() const;
};

class SoundEventRegistry {
  public:
    [[nodiscard]] core::Status add(SoundEventDefinition definition);
    [[nodiscard]] const SoundEventDefinition* find(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::vector<SoundEventDefinition>& definitions() const noexcept;

  private:
    std::vector<SoundEventDefinition> definitions_;
    std::unordered_map<std::string, std::size_t> by_id_;
};

[[nodiscard]] core::Result<SoundEventDefinition>
sound_event_definition_from_prototype(const modding::GenericPrototype& prototype,
                                      const assets::AssetCatalog& assets);

[[nodiscard]] core::Result<SoundEventRegistry>
sound_event_registry_from_prototypes(const modding::PrototypeRegistry& prototypes,
                                     const assets::AssetCatalog& assets);

[[nodiscard]] core::Result<AudioBus> audio_bus_from_name(std::string_view name);

} // namespace heartstead::audio
