#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace heartstead::audio {

struct ProceduralToneAsset {
    std::uint32_t sample_rate = 48'000;
    std::vector<float> mono_samples;
};

[[nodiscard]] bool is_procedural_tone_asset(const std::filesystem::path& path) noexcept;
[[nodiscard]] core::Result<ProceduralToneAsset>
load_procedural_tone_asset(const std::filesystem::path& path, std::uint32_t sample_rate);
[[nodiscard]] core::Result<ProceduralToneAsset>
load_procedural_tone_asset(std::string_view manifest, std::uint32_t sample_rate,
                           const std::filesystem::path& source = "<cooked-tone>");

} // namespace heartstead::audio
