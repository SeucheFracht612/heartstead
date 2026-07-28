#include "engine/core/process_entry.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/renderer/ui/ui_renderer.hpp"
#include "engine/ui/widget_tree.hpp"

#include <algorithm>
#include <array>
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
    std::uint32_t widgets = 2'000;
    std::uint32_t warmup_frames = 60;
    std::uint32_t measured_frames = 600;
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
                    "ui_benchmark.missing_value", std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--widgets" || argument == "--warmup" ||
                   argument == "--frames") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = parse_unsigned<std::uint32_t>(value.value());
            if (!parsed || *parsed == 0) {
                return core::Result<Options>::failure(
                    "ui_benchmark.invalid_count",
                    std::string(argument) + " must be a positive integer");
            }
            if (argument == "--widgets") {
                if (*parsed > 10'000) {
                    return core::Result<Options>::failure(
                        "ui_benchmark.widget_limit",
                        "--widgets must be between one and 10000");
                }
                options.widgets = *parsed;
            } else if (argument == "--warmup") {
                options.warmup_frames = *parsed;
            } else {
                options.measured_frames = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else {
            return core::Result<Options>::failure(
                "ui_benchmark.unknown_option",
                "unknown UI benchmark option: " + std::string(argument));
        }
    }
    return core::Result<Options>::success(options);
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_ui_benchmark [options]\n"
              "  --widgets N  Retained widgets per frame (default 2000)\n"
              "  --warmup N   Unmeasured frames (default 60)\n"
              "  --frames N   Measured frames (default 600)\n"
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

struct Fixture {
    std::unique_ptr<renderer::rhi::IRenderDevice> device;
    renderer::rhi::RenderResourceHandle pipeline;
};

[[nodiscard]] core::Result<Fixture> make_fixture() {
    using namespace renderer::rhi;
    RenderDeviceDesc desc;
    desc.initial_extent = {1280, 720};
    auto device = create_render_device(desc);
    if (!device) {
        return core::Result<Fixture>::failure(device.error().code, device.error().message);
    }
    const auto material = core::PrototypeId::parse("benchmark:materials/ui");
    RenderPipelineLayoutDesc layout;
    layout.material_id = *material;
    layout.shader_template = {"benchmark", "ui"};
    layout.push_constant_ranges.push_back(
        {RenderShaderStageFlags::vertex | RenderShaderStageFlags::fragment, 0,
         sizeof(ChunkPushConstants)});
    auto status = device.value()->bind_pipeline_layout(layout);
    if (!status) {
        return core::Result<Fixture>::failure(status.error().code, status.error().message);
    }
    constexpr std::array<std::uint32_t, 5> spirv{0x07230203, 0x00010000, 0, 1, 0};
    auto vertex = device.value()->create_shader_module({RenderShaderStage::vertex, "ui"}, spirv);
    auto fragment =
        device.value()->create_shader_module({RenderShaderStage::fragment, "ui"}, spirv);
    if (!vertex || !fragment) {
        const auto& error = !vertex ? vertex.error() : fragment.error();
        return core::Result<Fixture>::failure(error.code, error.message);
    }
    RenderGraphicsPipelineDesc pipeline;
    pipeline.vertex_shader = vertex.value().handle;
    pipeline.fragment_shader = fragment.value().handle;
    pipeline.material_id = *material;
    pipeline.vertex_stride = sizeof(renderer::GpuUiVertex);
    pipeline.vertex_attributes.assign(renderer::gpu_ui_vertex_attributes.begin(),
                                      renderer::gpu_ui_vertex_attributes.end());
    pipeline.cull_mode = RenderCullMode::none;
    pipeline.depth_test_enable = true;
    pipeline.depth_write_enable = false;
    pipeline.depth_compare = RenderCompareOperation::always;
    pipeline.blend_mode = RenderBlendMode::alpha;
    auto created = device.value()->create_graphics_pipeline(pipeline);
    if (!created) {
        return core::Result<Fixture>::failure(created.error().code, created.error().message);
    }
    return core::Result<Fixture>::success(
        {std::move(device).value(), created.value().handle});
}

[[nodiscard]] int run(const Options& options) {
    auto fixture = make_fixture();
    if (!fixture) {
        std::cerr << fixture.error().code << ": " << fixture.error().message << '\n';
        return 1;
    }
    renderer::UiRenderer renderer(*fixture.value().device, fixture.value().pipeline);
    renderer::UiRendererConfig renderer_config;
    renderer_config.maximum_vertices = (options.widgets + 1U) * 4U;
    renderer_config.maximum_indices = (options.widgets + 1U) * 6U;
    auto status = renderer.initialize({1280, 720}, renderer_config);
    if (!status) {
        std::cerr << status.error().code << ": " << status.error().message << '\n';
        return 1;
    }
    ui::WidgetTree tree;
    ui::WidgetDesc root;
    root.id = ui::widget_id("benchmark.root");
    root.layout.width = ui::UiSize::fill();
    root.layout.height = ui::UiSize::fill();
    root.layout.mode = ui::UiLayoutMode::grid;
    root.layout.grid_columns = 50;
    root.layout.grid_cell_height = 16.0F;
    root.layout.gap = 1.0F;
    root.color = {0.0F, 0.0F, 0.0F, 0.0F};
    status = tree.add(root);
    if (!status) {
        std::cerr << status.error().code << ": " << status.error().message << '\n';
        return 1;
    }
    for (std::uint32_t index = 0; index < options.widgets; ++index) {
        ui::WidgetDesc widget;
        widget.id = ui::widget_id("benchmark.widget." + std::to_string(index));
        widget.parent = root.id;
        widget.kind = ui::WidgetKind::image;
        widget.layout.width = ui::UiSize::fill();
        widget.layout.height = ui::UiSize::fill();
        widget.color = {0.28F, 0.14F, 0.05F, 0.9F};
        status = tree.add(std::move(widget));
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
    }
    std::uint32_t maximum_draw_calls = 0;
    const auto frame = [&]() -> core::Status {
        auto layout_status = tree.layout({1280.0F, 720.0F});
        if (!layout_status) {
            return layout_status;
        }
        auto paint = tree.paint(renderer);
        if (!paint) {
            return core::Status::failure(paint.error().code, paint.error().message);
        }
        auto built = renderer.build_frame();
        if (!built) {
            return core::Status::failure(built.error().code, built.error().message);
        }
        maximum_draw_calls = std::max(maximum_draw_calls, built.value().stats.draw_calls);
        return core::Status::ok();
    };
    for (std::uint32_t index = 0; index < options.warmup_frames; ++index) {
        status = frame();
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
    }
    std::vector<double> milliseconds;
    milliseconds.reserve(options.measured_frames);
    for (std::uint32_t index = 0; index < options.measured_frames; ++index) {
        const auto start = std::chrono::steady_clock::now();
        status = frame();
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (!status) {
            std::cerr << status.error().code << ": " << status.error().message << '\n';
            return 1;
        }
        milliseconds.push_back(elapsed);
    }
    const auto median = percentile(milliseconds, 0.50);
    const auto p95 = percentile(milliseconds, 0.95);
    const auto maximum = *std::ranges::max_element(milliseconds);
    constexpr double target_p95_ms = 1.0;
    const auto json =
        std::string("{\n") + "  \"widgets\": " + std::to_string(options.widgets) + ",\n" +
        "  \"measured_frames\": " + std::to_string(options.measured_frames) + ",\n" +
        "  \"median_layout_paint_build_ms\": " + std::to_string(median) + ",\n" +
        "  \"p95_layout_paint_build_ms\": " + std::to_string(p95) + ",\n" +
        "  \"maximum_layout_paint_build_ms\": " + std::to_string(maximum) + ",\n" +
        "  \"maximum_draw_calls\": " + std::to_string(maximum_draw_calls) + ",\n" +
        "  \"target_p95_ms\": " + std::to_string(target_p95_ms) + ",\n" +
        "  \"within_target\": " + (p95 <= target_p95_ms ? "true" : "false") + "\n}\n";
    std::cout << json;
    if (!options.output.empty()) {
        std::error_code error;
        if (options.output.has_parent_path()) {
            std::filesystem::create_directories(options.output.parent_path(), error);
        }
        std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
        output << json;
        if (error || !output) {
            std::cerr << "ui_benchmark.write_failed: failed to write output\n";
            return 1;
        }
    }
    status = renderer.shutdown();
    if (!status) {
        std::cerr << status.error().code << ": " << status.error().message << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        auto options = parse_options(argc, argv);
        if (!options) {
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
