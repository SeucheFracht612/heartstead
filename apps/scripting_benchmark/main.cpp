#include "engine/core/process_entry.hpp"
#include "engine/scripting/script_runtime.hpp"

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
    std::uint32_t modules = 16;
    std::uint32_t warmup_calls = 1'000;
    std::uint32_t measured_calls = 10'000;
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
                return core::Result<std::string_view>::failure("scripting_benchmark.missing_value",
                                                               std::string(argument) +
                                                                   " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--modules" || argument == "--warmup" || argument == "--calls") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_unsigned<std::uint32_t>(value.value());
            if (!parsed || *parsed == 0) {
                return core::Result<Options>::failure("scripting_benchmark.invalid_count",
                                                      std::string(argument) +
                                                          " must be a positive integer");
            }
            if (argument == "--modules") {
                if (*parsed > 256) {
                    return core::Result<Options>::failure("scripting_benchmark.module_limit",
                                                          "--modules must be between one and 256");
                }
                options.modules = *parsed;
            } else if (argument == "--warmup") {
                options.warmup_calls = *parsed;
            } else {
                options.measured_calls = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure("scripting_benchmark.unknown_option",
                                                  "unknown scripting benchmark option: " +
                                                      std::string(argument));
        }
    }
    return core::Result<Options>::success(options);
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_scripting_benchmark [options]\n"
              "  --modules N  Isolated neutral modules (default 16)\n"
              "  --warmup N   Unmeasured calls (default 1000)\n"
              "  --calls N    Measured calls (default 10000)\n"
              "  --output P   Write JSON to a file as well as stdout\n";
}

[[nodiscard]] double percentile(std::vector<double> values, double quantile) {
    std::ranges::sort(values);
    const auto position = quantile * static_cast<double>(values.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto alpha = position - static_cast<double>(lower);
    return values[lower] * (1.0 - alpha) + values[upper] * alpha;
}

[[nodiscard]] core::Status write_output(const std::filesystem::path& path, std::string_view json) {
    if (path.empty()) {
        return core::Status::ok();
    }
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) {
        return core::Status::failure("scripting_benchmark.output_directory_failed",
                                     "failed to create the benchmark output directory");
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json;
    if (!output) {
        return core::Status::failure("scripting_benchmark.output_write_failed",
                                     "failed to write the benchmark output");
    }
    return core::Status::ok();
}

[[nodiscard]] int run(const Options& options) {
    using namespace scripting;

    const auto backend = script_backend_info(ScriptBackend::luau);
    if (!backend.available) {
        const std::string json = "{\n"
                                 "  \"schema\": \"heartstead.scripting_benchmark.v1\",\n"
                                 "  \"backend\": \"luau\",\n"
                                 "  \"available\": false\n"
                                 "}\n";
        std::cout << json;
        const auto written = write_output(options.output, json);
        if (!written) {
            std::cerr << written.error().code << ": " << written.error().message << '\n';
            return 1;
        }
        return 0;
    }

    ScriptRuntimeDesc runtime_desc{ScriptBackend::luau};
    runtime_desc.max_modules = options.modules;
    auto runtime = create_script_runtime(runtime_desc);
    if (!runtime) {
        std::cerr << runtime.error().code << ": " << runtime.error().message << '\n';
        return 1;
    }

    std::vector<std::string> module_ids;
    module_ids.reserve(options.modules);
    for (std::uint32_t index = 0; index < options.modules; ++index) {
        ScriptModuleDesc module;
        module.module_id = "benchmark:scripts/runtime_server/neutral_" + std::to_string(index);
        module.source_mod_id = "benchmark";
        module.source_path =
            "benchmark/scripts/runtime_server/neutral_" + std::to_string(index) + ".luau";
        module.stage = ScriptStage::runtime_server;
        module.source = "return { run = function(value)\n"
                        "  local result = value\n"
                        "  for index = 1, 64 do\n"
                        "    result = (result * 1.000001 + index) % 100000\n"
                        "  end\n"
                        "  return result\n"
                        "end }\n";
        auto loaded = runtime.value()->load_module(module);
        if (!loaded) {
            std::cerr << loaded.error().code << ": " << loaded.error().message << '\n';
            return 1;
        }
        module_ids.push_back(std::move(module.module_id));
    }

    const auto invoke = [&](std::uint32_t index) -> core::Status {
        ScriptCallDesc call;
        call.module_id = module_ids[index % options.modules];
        call.function_name = "run";
        call.stage = ScriptStage::runtime_server;
        call.arguments = {ScriptValue::number(static_cast<double>(index % 10'000U))};
        auto result = runtime.value()->call(std::move(call));
        if (!result) {
            return core::Status::failure(result.error().code, result.error().message);
        }
        if (result.value().return_value.kind != ScriptValueKind::number ||
            !std::isfinite(result.value().return_value.number_value)) {
            return core::Status::failure("scripting_benchmark.invalid_result",
                                         "neutral benchmark module returned an invalid value");
        }
        return core::Status::ok();
    };

    for (std::uint32_t index = 0; index < options.warmup_calls; ++index) {
        auto status = invoke(index);
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
    }

    std::vector<double> milliseconds;
    milliseconds.reserve(options.measured_calls);
    for (std::uint32_t index = 0; index < options.measured_calls; ++index) {
        const auto started = std::chrono::steady_clock::now();
        auto status = invoke(index);
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
        milliseconds.push_back(elapsed);
    }

    const auto p50 = percentile(milliseconds, 0.50);
    const auto p95 = percentile(milliseconds, 0.95);
    const auto maximum = *std::ranges::max_element(milliseconds);
    constexpr double target_p95_ms = 0.25;
    const auto stats = runtime.value()->stats();
    const auto json =
        std::string("{\n") + "  \"schema\": \"heartstead.scripting_benchmark.v1\",\n" +
        "  \"backend\": \"luau\",\n" + "  \"available\": true,\n" +
        "  \"modules\": " + std::to_string(options.modules) + ",\n" +
        "  \"warmup_calls\": " + std::to_string(options.warmup_calls) + ",\n" +
        "  \"measured_calls\": " + std::to_string(options.measured_calls) + ",\n" +
        "  \"p50_call_ms\": " + std::to_string(p50) + ",\n" +
        "  \"p95_call_ms\": " + std::to_string(p95) + ",\n" +
        "  \"maximum_call_ms\": " + std::to_string(maximum) + ",\n" +
        "  \"target_p95_ms\": " + std::to_string(target_p95_ms) + ",\n" +
        "  \"within_target\": " + (p95 <= target_p95_ms ? "true" : "false") + ",\n" +
        "  \"current_vm_memory_bytes\": " + std::to_string(stats.current_memory_bytes) + ",\n" +
        "  \"peak_vm_memory_bytes\": " + std::to_string(stats.peak_memory_bytes) + ",\n" +
        "  \"interrupt_count\": " + std::to_string(stats.interrupt_count) +
        "\n"
        "}\n";
    std::cout << json;
    const auto written = write_output(options.output, json);
    if (!written) {
        std::cerr << written.error().code << ": " << written.error().message << '\n';
        return 1;
    }
    return 0;
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
