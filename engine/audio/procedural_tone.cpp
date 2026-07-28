#include "engine/audio/procedural_tone.hpp"

#include "engine/modding/flat_manifest.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <system_error>

namespace heartstead::audio {

namespace {

struct ToneDefinition {
    std::string wave;
    float frequency_hz = 220.0F;
    float secondary_frequency_hz = 0.0F;
    float amplitude = 0.1F;
    float duration_ms = 250.0F;
    float attack_ms = 0.0F;
    float release_ms = 0.0F;
    std::uint32_t seed = 1;
};

[[nodiscard]] const std::string* field(const std::map<std::string, std::string>& fields,
                                       std::string_view key) {
    const auto found = fields.find(std::string(key));
    return found == fields.end() ? nullptr : &found->second;
}

[[nodiscard]] core::Result<float> float_field(const std::map<std::string, std::string>& fields,
                                              std::string_view key, float fallback) {
    const auto* value = field(fields, key);
    if (value == nullptr) {
        return core::Result<float>::success(fallback);
    }
    float parsed = 0.0F;
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (error != std::errc{} || end != value->data() + value->size() || !std::isfinite(parsed)) {
        return core::Result<float>::failure("procedural_tone.invalid_number",
                                            std::string(key) + " must be finite");
    }
    return core::Result<float>::success(parsed);
}

[[nodiscard]] core::Result<std::uint32_t>
u32_field(const std::map<std::string, std::string>& fields, std::string_view key,
          std::uint32_t fallback) {
    const auto* value = field(fields, key);
    if (value == nullptr) {
        return core::Result<std::uint32_t>::success(fallback);
    }
    std::uint32_t parsed = 0;
    const auto [end, error] = std::from_chars(value->data(), value->data() + value->size(), parsed);
    if (error != std::errc{} || end != value->data() + value->size()) {
        return core::Result<std::uint32_t>::failure("procedural_tone.invalid_integer",
                                                    std::string(key) + " must be u32");
    }
    return core::Result<std::uint32_t>::success(parsed);
}

[[nodiscard]] core::Result<ToneDefinition> load_definition(const std::filesystem::path& path) {
    std::vector<modding::ModDiagnostic> diagnostics;
    const auto fields = modding::parse_flat_manifest(path, diagnostics,
                                                     {.diagnostic_prefix = "procedural_tone",
                                                      .maximum_bytes = 16U * 1024U,
                                                      .maximum_line_bytes = 1024U,
                                                      .maximum_fields = 16U});
    if (!diagnostics.empty()) {
        return core::Result<ToneDefinition>::failure(diagnostics.front().code,
                                                     diagnostics.front().message);
    }
    const auto* wave = field(fields, "wave");
    auto frequency = float_field(fields, "frequency_hz", 220.0F);
    auto secondary = float_field(fields, "secondary_frequency_hz", 0.0F);
    auto amplitude = float_field(fields, "amplitude", 0.1F);
    auto duration = float_field(fields, "duration_ms", 250.0F);
    auto attack = float_field(fields, "attack_ms", 0.0F);
    auto release = float_field(fields, "release_ms", 0.0F);
    auto seed = u32_field(fields, "seed", 1U);
    if (wave == nullptr || !frequency || !secondary || !amplitude || !duration || !attack ||
        !release || !seed) {
        if (wave == nullptr) {
            return core::Result<ToneDefinition>::failure("procedural_tone.missing_wave",
                                                         "procedural tone must declare a wave");
        }
        const auto& error = !frequency   ? frequency.error()
                            : !secondary ? secondary.error()
                            : !amplitude ? amplitude.error()
                            : !duration  ? duration.error()
                            : !attack    ? attack.error()
                            : !release   ? release.error()
                                         : seed.error();
        return core::Result<ToneDefinition>::failure(error.code, error.message);
    }

    ToneDefinition result;
    result.wave = *wave;
    result.frequency_hz = frequency.value();
    result.secondary_frequency_hz = secondary.value();
    result.amplitude = amplitude.value();
    result.duration_ms = duration.value();
    result.attack_ms = attack.value();
    result.release_ms = release.value();
    result.seed = seed.value();
    if (result.wave != "sine" && result.wave != "noise") {
        return core::Result<ToneDefinition>::failure("procedural_tone.invalid_wave",
                                                     "procedural tone wave must be sine or noise");
    }
    if (result.frequency_hz < 20.0F || result.frequency_hz > 20'000.0F ||
        result.secondary_frequency_hz < 0.0F || result.secondary_frequency_hz > 20'000.0F) {
        return core::Result<ToneDefinition>::failure(
            "procedural_tone.invalid_frequency",
            "procedural tone frequencies must be within the audible range");
    }
    if (result.amplitude < 0.0F || result.amplitude > 1.0F) {
        return core::Result<ToneDefinition>::failure(
            "procedural_tone.invalid_amplitude",
            "procedural tone amplitude must be between zero and one");
    }
    if (result.duration_ms <= 0.0F || result.duration_ms > 60'000.0F || result.attack_ms < 0.0F ||
        result.release_ms < 0.0F || result.attack_ms + result.release_ms > result.duration_ms) {
        return core::Result<ToneDefinition>::failure(
            "procedural_tone.invalid_duration",
            "procedural tone duration/envelope must fit within one minute");
    }
    return core::Result<ToneDefinition>::success(std::move(result));
}

[[nodiscard]] float envelope(const ToneDefinition& definition, float elapsed_ms) noexcept {
    auto gain = 1.0F;
    if (definition.attack_ms > 0.0F && elapsed_ms < definition.attack_ms) {
        gain = elapsed_ms / definition.attack_ms;
    }
    const auto release_start = definition.duration_ms - definition.release_ms;
    if (definition.release_ms > 0.0F && elapsed_ms > release_start) {
        gain = std::min(gain, (definition.duration_ms - elapsed_ms) / definition.release_ms);
    }
    return std::clamp(gain, 0.0F, 1.0F);
}

} // namespace

bool is_procedural_tone_asset(const std::filesystem::path& path) noexcept {
    return path.extension() == ".tone";
}

core::Result<ProceduralToneAsset> load_procedural_tone_asset(const std::filesystem::path& path,
                                                             std::uint32_t sample_rate) {
    if (sample_rate < 8'000 || sample_rate > 384'000) {
        return core::Result<ProceduralToneAsset>::failure(
            "procedural_tone.invalid_sample_rate",
            "procedural tone sample rate must be between 8000 and 384000");
    }
    auto definition = load_definition(path);
    if (!definition) {
        return core::Result<ProceduralToneAsset>::failure(definition.error().code,
                                                          definition.error().message);
    }
    const auto sample_count_double = static_cast<double>(sample_rate) *
                                     static_cast<double>(definition.value().duration_ms) / 1000.0;
    const auto sample_count = static_cast<std::size_t>(std::ceil(sample_count_double));
    if (sample_count == 0) {
        return core::Result<ProceduralToneAsset>::failure("procedural_tone.empty",
                                                          "procedural tone produced no samples");
    }

    ProceduralToneAsset result;
    result.sample_rate = sample_rate;
    result.mono_samples.resize(sample_count);
    auto noise_state = definition.value().seed == 0 ? 1U : definition.value().seed;
    constexpr auto tau = std::numbers::pi_v<float> * 2.0F;
    for (std::size_t index = 0; index < sample_count; ++index) {
        const auto seconds = static_cast<float>(index) / static_cast<float>(sample_rate);
        const auto elapsed_ms = seconds * 1000.0F;
        float sample = 0.0F;
        if (definition.value().wave == "noise") {
            noise_state ^= noise_state << 13U;
            noise_state ^= noise_state >> 17U;
            noise_state ^= noise_state << 5U;
            sample = (static_cast<float>(noise_state) /
                      static_cast<float>(std::numeric_limits<std::uint32_t>::max())) *
                         2.0F -
                     1.0F;
        } else {
            sample = std::sin(tau * definition.value().frequency_hz * seconds);
            if (definition.value().secondary_frequency_hz > 0.0F) {
                sample =
                    (sample + std::sin(tau * definition.value().secondary_frequency_hz * seconds)) *
                    0.5F;
            }
        }
        result.mono_samples[index] =
            sample * definition.value().amplitude * envelope(definition.value(), elapsed_ms);
    }
    return core::Result<ProceduralToneAsset>::success(std::move(result));
}

} // namespace heartstead::audio
