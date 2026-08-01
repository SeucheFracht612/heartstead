#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/rhi/render_device.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace heartstead::renderer::testing {

struct VisualCapture {
    rhi::RenderExtent extent{};
    std::vector<std::uint8_t> rgba8;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct VisualRegressionThresholds {
    double maximum_changed_fraction = 0.0025;
    double maximum_rmse = 1.5;
    std::uint8_t per_channel_tolerance = 3;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct VisualRegressionResult {
    bool passed = false;
    double changed_fraction = 0.0;
    double rmse = 0.0;
    std::uint8_t maximum_channel_delta = 0;
    std::uint64_t changed_pixels = 0;
    std::uint64_t total_pixels = 0;
    std::string actual_hash;
    std::string baseline_hash;
};

[[nodiscard]] core::Result<VisualCapture> capture_output(rhi::IRenderDevice& device);
[[nodiscard]] core::Status write_visual_capture(const std::filesystem::path& png_path,
                                                const VisualCapture& capture);
[[nodiscard]] core::Result<VisualCapture>
read_visual_capture(const std::filesystem::path& png_path);
[[nodiscard]] core::Result<VisualRegressionResult>
compare_visual_captures(const VisualCapture& actual, const VisualCapture& baseline,
                        VisualRegressionThresholds thresholds = {});
[[nodiscard]] std::string visual_capture_hash(std::span<const std::uint8_t> rgba8);

} // namespace heartstead::renderer::testing
