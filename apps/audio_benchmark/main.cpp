#include "engine/assets/asset_catalog.hpp"
#include "engine/audio/audio_system.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/core/process_entry.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace heartstead;

struct Options {
    std::uint32_t voices = 128;
    std::uint32_t warmup_blocks = 120;
    std::uint32_t measured_blocks = 1'000;
    std::filesystem::path output;
    bool help = false;
};

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_unsigned(std::string_view value) {
    Integer result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] core::Result<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        const auto next = [&]() -> core::Result<std::string_view> {
            if (index + 1 >= argc) {
                return core::Result<std::string_view>::failure(
                    "audio_benchmark.missing_value", std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--voices" || argument == "--warmup" || argument == "--blocks") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_unsigned<std::uint32_t>(value.value());
            if (!parsed || *parsed == 0) {
                return core::Result<Options>::failure("audio_benchmark.invalid_count",
                                                      std::string(argument) +
                                                          " must be a positive integer");
            }
            if (argument == "--voices") {
                if (*parsed > 1'024) {
                    return core::Result<Options>::failure("audio_benchmark.invalid_voice_count",
                                                          "--voices must be between one and 1024");
                }
                options.voices = *parsed;
            } else if (argument == "--warmup") {
                options.warmup_blocks = *parsed;
            } else {
                options.measured_blocks = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure("audio_benchmark.unknown_option",
                                                  "unknown audio benchmark option: " +
                                                      std::string(argument));
        }
    }
    return core::Result<Options>::success(options);
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_audio_benchmark [options]\n"
              "  --voices N  Active mono looping voices (default 128)\n"
              "  --warmup N  Unmeasured 256-frame blocks (default 120)\n"
              "  --blocks N  Measured 256-frame blocks (default 1000)\n"
              "  --output P  Write JSON to a file as well as stdout\n";
}

[[nodiscard]] double percentile(std::vector<double> values, double quantile) {
    std::ranges::sort(values);
    const auto rank =
        static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(values.size())));
    return values[std::min(values.size() - 1U, std::max<std::size_t>(1U, rank) - 1U)];
}

[[nodiscard]] int run(const Options& options) {
    constexpr std::uint32_t sample_rate = 48'000;
    constexpr std::uint32_t channels = 2;
    constexpr std::uint32_t frames = 256;
    const auto source_path = std::filesystem::path(HEARTSTEAD_SOURCE_ROOT) /
                             "mods/base/assets/sounds/footsteps/earth_step.tone";

    assets::AssetCatalog assets;
    auto status = assets.add(assets::AssetRecord{
        "benchmark:sounds/voice.tone",
        assets::AssetKind::sound,
        assets::VirtualPath{"benchmark", "sounds/voice.tone"},
        assets::AssetSourceKind::engine,
        "audio_benchmark",
        0,
        source_path,
        "benchmark",
        false,
        {},
    });
    if (!status) {
        std::cerr << status.error().code << ": " << status.error().message << '\n';
        return 1;
    }
    audio::SoundEventDefinition definition;
    definition.prototype_id = core::PrototypeId::parse("benchmark:audio/voice").value();
    definition.asset_id = "benchmark:sounds/voice.tone";
    definition.gain = 0.005F;
    definition.maximum_instances = options.voices;
    definition.spatialized = false;
    definition.looping = true;
    audio::SoundEventRegistry events;
    status = events.add(definition);
    if (!status) {
        std::cerr << status.error().code << ": " << status.error().message << '\n';
        return 1;
    }

    audio::AudioSystemDesc desc;
    desc.backend = audio::AudioBackend::miniaudio;
    desc.events = &events;
    desc.assets = &assets;
    desc.mixer.maximum_voices = options.voices;
    desc.sample_rate = sample_rate;
    desc.output_channels = channels;
    desc.period_frames = frames;
    desc.open_output_device = false;
    auto system = audio::create_audio_system(desc);
    if (!system) {
        std::cerr << system.error().code << ": " << system.error().message << '\n';
        return 1;
    }
    for (std::uint32_t index = 0; index < options.voices; ++index) {
        auto voice = system.value()->play({definition.prototype_id, std::nullopt, 1.0F, 1.0F});
        if (!voice) {
            std::cerr << voice.error().code << ": " << voice.error().message << '\n';
            return 1;
        }
    }

    std::vector<float> output(static_cast<std::size_t>(frames) * channels);
    const auto delta_seconds = static_cast<float>(frames) / static_cast<float>(sample_rate);
    const auto mix = [&]() -> core::Status {
        auto render_status = system.value()->render_offline(output, frames);
        if (!render_status) {
            return render_status;
        }
        return system.value()->update(delta_seconds);
    };
    for (std::uint32_t block = 0; block < options.warmup_blocks; ++block) {
        status = mix();
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
    }

    std::vector<double> milliseconds;
    milliseconds.reserve(options.measured_blocks);
    double sum = 0.0;
    double sample_energy = 0.0;
    for (std::uint32_t block = 0; block < options.measured_blocks; ++block) {
        const auto start = std::chrono::steady_clock::now();
        status = mix();
        const auto end = std::chrono::steady_clock::now();
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(end - start).count();
        milliseconds.push_back(elapsed);
        sum += elapsed;
        for (const auto sample : output) {
            sample_energy += static_cast<double>(sample) * static_cast<double>(sample);
        }
    }
    const auto p50 = percentile(milliseconds, 0.50);
    const auto p95 = percentile(milliseconds, 0.95);
    const auto maximum = *std::ranges::max_element(milliseconds);
    const auto average = sum / static_cast<double>(milliseconds.size());
    const auto rms =
        std::sqrt(sample_energy / static_cast<double>(milliseconds.size() * output.size()));

    const auto json = std::string("{\n") + "  \"voices\": " + std::to_string(options.voices) +
                      ",\n" + "  \"sample_rate\": " + std::to_string(sample_rate) + ",\n" +
                      "  \"channels\": " + std::to_string(channels) + ",\n" +
                      "  \"frames_per_block\": " + std::to_string(frames) + ",\n" +
                      "  \"measured_blocks\": " + std::to_string(options.measured_blocks) + ",\n" +
                      "  \"average_ms\": " + std::to_string(average) + ",\n" +
                      "  \"p50_ms\": " + std::to_string(p50) + ",\n" +
                      "  \"p95_ms\": " + std::to_string(p95) + ",\n" +
                      "  \"maximum_ms\": " + std::to_string(maximum) + ",\n" +
                      "  \"output_rms\": " + std::to_string(rms) + "\n}\n";
    std::cout << json;
    if (!options.output.empty()) {
        std::ofstream file(options.output, std::ios::binary | std::ios::trunc);
        if (!file) {
            std::cerr << "audio_benchmark.output_open_failed: " << options.output << '\n';
            return 1;
        }
        file << json;
        if (!file.good()) {
            std::cerr << "audio_benchmark.output_write_failed: " << options.output << '\n';
            return 1;
        }
    }
    return rms > 0.0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        auto options = parse_options(argc, argv);
        if (!options) {
            print_usage(std::cerr);
            std::cerr << options.error().code << ": " << options.error().message << '\n';
            return 2;
        }
        if (options.value().help) {
            print_usage(std::cout);
            return 0;
        }
        return run(options.value());
    });
}
