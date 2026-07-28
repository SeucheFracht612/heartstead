#pragma once

#include "engine/audio/audio_system.hpp"

#include <memory>

namespace heartstead::audio::miniaudio {

[[nodiscard]] core::Result<std::unique_ptr<IAudioSystem>>
create_system(const AudioSystemDesc& desc);

} // namespace heartstead::audio::miniaudio
