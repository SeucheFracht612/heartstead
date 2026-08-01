#include "engine/renderer/testing/visual_regression.hpp"

#include "engine/assets/image_asset.hpp"
#include "engine/core/file_io.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace heartstead::renderer::testing {

core::Status VisualCapture::validate() const noexcept {
    if (!extent.is_valid()) {
        return core::Status::failure("visual_capture.invalid_extent",
                                     "visual capture extent must be nonzero");
    }
    const auto expected = static_cast<std::uint64_t>(extent.width) * extent.height * 4U;
    if (expected > std::numeric_limits<std::size_t>::max() || rgba8.size() != expected) {
        return core::Status::failure("visual_capture.invalid_payload",
                                     "visual capture must contain tightly packed RGBA8 pixels");
    }
    return core::Status::ok();
}

core::Status VisualRegressionThresholds::validate() const noexcept {
    if (!std::isfinite(maximum_changed_fraction) || maximum_changed_fraction < 0.0 ||
        maximum_changed_fraction > 1.0 || !std::isfinite(maximum_rmse) || maximum_rmse < 0.0 ||
        maximum_rmse > 255.0) {
        return core::Status::failure("visual_regression.invalid_thresholds",
                                     "visual regression thresholds are outside valid ranges");
    }
    return core::Status::ok();
}

std::string visual_capture_hash(std::span<const std::uint8_t> rgba8) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto byte : rgba8) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

core::Result<VisualCapture> capture_output(rhi::IRenderDevice& device) {
    const auto extent = device.current_extent();
    auto bytes = device.read_back_output_image();
    if (!bytes) {
        return core::Result<VisualCapture>::failure(bytes.error().code, bytes.error().message);
    }
    VisualCapture capture{extent, std::move(bytes).value()};
    auto status = capture.validate();
    if (!status) {
        return core::Result<VisualCapture>::failure(status.error().code, status.error().message);
    }
    return core::Result<VisualCapture>::success(std::move(capture));
}

core::Status write_visual_capture(const std::filesystem::path& png_path,
                                  const VisualCapture& capture) {
    auto status = capture.validate();
    if (!status) {
        return status;
    }
    std::error_code error;
    if (png_path.has_parent_path()) {
        std::filesystem::create_directories(png_path.parent_path(), error);
        if (error) {
            return core::Status::failure("visual_capture.create_directory_failed",
                                         "failed to create capture directory: " + error.message());
        }
    }
    if (capture.extent.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        capture.extent.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return core::Status::failure("visual_capture.extent_too_large",
                                     "capture extent exceeds PNG encoder limits");
    }
    const auto written = stbi_write_png(
        png_path.string().c_str(), static_cast<int>(capture.extent.width),
        static_cast<int>(capture.extent.height), 4, capture.rgba8.data(),
        static_cast<int>(capture.extent.width * 4U));
    if (written == 0) {
        return core::Status::failure("visual_capture.png_write_failed",
                                     "failed to encode visual capture: " + png_path.string());
    }
    auto sidecar = png_path;
    sidecar += ".capture.json";
    std::ofstream metadata(sidecar, std::ios::binary | std::ios::trunc);
    if (!metadata) {
        return core::Status::failure("visual_capture.metadata_open_failed",
                                     "failed to open visual capture metadata");
    }
    metadata << "{\n  \"schema\": \"heartstead.visual_capture.v1\",\n"
             << "  \"width\": " << capture.extent.width << ",\n"
             << "  \"height\": " << capture.extent.height << ",\n"
             << "  \"rgba8_hash\": \"" << visual_capture_hash(capture.rgba8) << "\"\n}\n";
    if (!metadata) {
        return core::Status::failure("visual_capture.metadata_write_failed",
                                     "failed to write visual capture metadata");
    }
    return core::Status::ok();
}

core::Result<VisualCapture> read_visual_capture(const std::filesystem::path& png_path) {
    auto encoded = core::read_binary_file(png_path);
    if (!encoded) {
        return core::Result<VisualCapture>::failure(encoded.error().code,
                                                    encoded.error().message);
    }
    auto image = assets::decode_png_or_jpeg(encoded.value());
    if (!image) {
        return core::Result<VisualCapture>::failure(image.error().code, image.error().message);
    }
    VisualCapture capture{{image.value().width, image.value().height},
                          std::move(image).value().rgba8};
    return core::Result<VisualCapture>::success(std::move(capture));
}

core::Result<VisualRegressionResult>
compare_visual_captures(const VisualCapture& actual, const VisualCapture& baseline,
                        VisualRegressionThresholds thresholds) {
    auto status = actual.validate();
    if (!status) {
        return core::Result<VisualRegressionResult>::failure(status.error().code,
                                                              status.error().message);
    }
    status = baseline.validate();
    if (!status) {
        return core::Result<VisualRegressionResult>::failure(status.error().code,
                                                              status.error().message);
    }
    status = thresholds.validate();
    if (!status) {
        return core::Result<VisualRegressionResult>::failure(status.error().code,
                                                              status.error().message);
    }
    if (actual.extent.width != baseline.extent.width ||
        actual.extent.height != baseline.extent.height) {
        return core::Result<VisualRegressionResult>::failure(
            "visual_regression.extent_mismatch", "actual and baseline capture extents differ");
    }
    VisualRegressionResult result;
    result.total_pixels = static_cast<std::uint64_t>(actual.extent.width) * actual.extent.height;
    result.actual_hash = visual_capture_hash(actual.rgba8);
    result.baseline_hash = visual_capture_hash(baseline.rgba8);
    long double squared_error = 0.0L;
    for (std::uint64_t pixel = 0; pixel < result.total_pixels; ++pixel) {
        bool changed = false;
        for (std::uint64_t channel = 0; channel < 4U; ++channel) {
            const auto index = static_cast<std::size_t>(pixel * 4U + channel);
            const auto delta = static_cast<std::uint8_t>(
                std::abs(static_cast<int>(actual.rgba8[index]) -
                         static_cast<int>(baseline.rgba8[index])));
            result.maximum_channel_delta = std::max(result.maximum_channel_delta, delta);
            changed = changed || delta > thresholds.per_channel_tolerance;
            squared_error += static_cast<long double>(delta) * delta;
        }
        result.changed_pixels += changed ? 1U : 0U;
    }
    result.changed_fraction = result.total_pixels == 0U
                                  ? 0.0
                                  : static_cast<double>(result.changed_pixels) /
                                        static_cast<double>(result.total_pixels);
    result.rmse = std::sqrt(static_cast<double>(
        squared_error / static_cast<long double>(result.total_pixels * 4U)));
    result.passed = result.changed_fraction <= thresholds.maximum_changed_fraction &&
                    result.rmse <= thresholds.maximum_rmse;
    return core::Result<VisualRegressionResult>::success(std::move(result));
}

} // namespace heartstead::renderer::testing
