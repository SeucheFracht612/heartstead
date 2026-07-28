#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace heartstead::audio {

struct ProceduralToneAsset {
    std::uint32_t sample_rate = 48'000;
    std::vector<float> mono_samples;
};

[[nodiscard]] bool is_procedural_tone_asset(const std::filesystem::path& path) noexcept;
[[nodiscard]] core::Result<ProceduralToneAsset>
load_procedural_tone_asset(const std::filesystem::path& path, std::uint32_t sample_rate);

} // namespace heartstead::audio
