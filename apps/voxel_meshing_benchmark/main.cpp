#include "engine/core/process_entry.hpp"
#include "engine/world/meshing/voxel_meshing_benchmark.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

namespace benchmark = heartstead::world::benchmark;
namespace core = heartstead::core;

struct Options {
    benchmark::VoxelMeshingBenchmarkConfig benchmark;
    std::filesystem::path output;
    bool help = false;
    bool list_corpora = false;
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

[[nodiscard]] std::optional<benchmark::VoxelCorpusKind>
parse_corpus(std::string_view name) noexcept {
    constexpr std::array corpora{
        benchmark::VoxelCorpusKind::empty,           benchmark::VoxelCorpusKind::uniform_solid,
        benchmark::VoxelCorpusKind::layered_terrain, benchmark::VoxelCorpusKind::sparse_caves,
        benchmark::VoxelCorpusKind::lit_settlement,  benchmark::VoxelCorpusKind::checkerboard,
        benchmark::VoxelCorpusKind::high_entropy,
    };
    for (const auto corpus : corpora) {
        if (benchmark::voxel_corpus_name(corpus) == name) {
            return corpus;
        }
    }
    return std::nullopt;
}

[[nodiscard]] core::Result<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        const auto next = [&]() -> core::Result<std::string_view> {
            if (index + 1 >= argc) {
                return core::Result<std::string_view>::failure(
                    "voxel_meshing.missing_value", std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--list-corpora") {
            options.list_corpora = true;
        } else if (argument == "--corpus") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            if (value.value() != "all") {
                const auto parsed = parse_corpus(value.value());
                if (!parsed) {
                    return core::Result<Options>::failure(
                        "voxel_meshing.invalid_corpus",
                        "--corpus must name a listed corpus or all");
                }
                options.benchmark.corpora = {*parsed};
            }
        } else if (argument == "--seed") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_unsigned<std::uint64_t>(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("voxel_meshing.invalid_seed",
                                                      "--seed must be an unsigned integer");
            }
            options.benchmark.seed = *parsed;
        } else if (argument == "--warmup" || argument == "--repetitions") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_unsigned<std::uint32_t>(value.value());
            if (!parsed || (*parsed == 0 && argument != "--warmup")) {
                return core::Result<Options>::failure("voxel_meshing.invalid_count",
                                                      std::string(argument) +
                                                          " must be a positive integer");
            }
            if (argument == "--warmup") {
                options.benchmark.warmup_repetitions = *parsed;
            } else {
                options.benchmark.repetitions = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure("voxel_meshing.unknown_option",
                                                  "unknown voxel meshing benchmark option: " +
                                                      std::string(argument));
        }
    }
    auto status = options.benchmark.validate();
    if (!status) {
        return core::Result<Options>::failure(status.error().code, status.error().message);
    }
    return core::Result<Options>::success(std::move(options));
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_voxel_meshing_benchmark [options]\n"
              "  --corpus NAME       Corpus name or all (default all)\n"
              "  --seed N            Deterministic corpus seed\n"
              "  --warmup N          Unmeasured repetitions (default 3; zero allowed)\n"
              "  --repetitions N     Retained raw samples per case (default 15)\n"
              "  --output PATH       Write JSON; otherwise JSON is written to stdout\n"
              "  --list-corpora      Print deterministic corpus names\n"
              "\nThe production 32-cubed mesher is measured. Performance measurements require an "
              "optimized build.\n";
}

void print_corpora() {
    constexpr std::array corpora{
        benchmark::VoxelCorpusKind::empty,           benchmark::VoxelCorpusKind::uniform_solid,
        benchmark::VoxelCorpusKind::layered_terrain, benchmark::VoxelCorpusKind::sparse_caves,
        benchmark::VoxelCorpusKind::lit_settlement,  benchmark::VoxelCorpusKind::checkerboard,
        benchmark::VoxelCorpusKind::high_entropy,
    };
    for (const auto corpus : corpora) {
        std::cout << benchmark::voxel_corpus_name(corpus) << '\n';
    }
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    static_cast<void>(options);
    std::cerr << "voxel_meshing.unoptimized_build: rebuild with default-release or an optimized "
                 "profiling configuration\n";
    return 2;
#else
    auto report = benchmark::run_voxel_meshing_benchmark(options.benchmark);
    if (!report) {
        std::cerr << report.error().code << ": " << report.error().message << '\n';
        return 1;
    }
    if (options.output.empty()) {
        std::cout << report.value().to_json();
    } else {
        auto status = report.value().write_json(options.output);
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
        std::cout << "wrote " << report.value().raw_samples.size() << " raw samples to "
                  << options.output << '\n';
    }
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    return core::run_process_entry(argv[0], [argc, argv] {
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
        if (options.value().list_corpora) {
            print_corpora();
            return 0;
        }
        return run(options.value());
    });
}
