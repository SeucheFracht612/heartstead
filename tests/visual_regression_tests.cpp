#include "engine/renderer/testing/visual_regression.hpp"

#include <cassert>
#include <filesystem>

int main() {
    using namespace heartstead::renderer::testing;
    VisualCapture baseline{{4, 4}, std::vector<std::uint8_t>(4U * 4U * 4U, 64U)};
    auto actual = baseline;
    assert(baseline.validate());
    auto comparison = compare_visual_captures(actual, baseline);
    assert(comparison && comparison.value().passed);
    assert(comparison.value().changed_pixels == 0);
    assert(comparison.value().actual_hash == comparison.value().baseline_hash);

    actual.rgba8[0] = 72;
    VisualRegressionThresholds tolerant;
    tolerant.maximum_changed_fraction = 0.1;
    tolerant.maximum_rmse = 2.0;
    tolerant.per_channel_tolerance = 3;
    comparison = compare_visual_captures(actual, baseline, tolerant);
    assert(comparison && comparison.value().passed);
    tolerant.maximum_changed_fraction = 0.0;
    comparison = compare_visual_captures(actual, baseline, tolerant);
    assert(comparison && !comparison.value().passed);

    const auto path = std::filesystem::temp_directory_path() /
                      "heartstead_visual_regression_test.png";
    assert(write_visual_capture(path, baseline));
    auto decoded = read_visual_capture(path);
    assert(decoded);
    comparison = compare_visual_captures(decoded.value(), baseline);
    assert(comparison && comparison.value().passed);
    std::error_code error;
    std::filesystem::remove(path, error);
    auto sidecar = path;
    sidecar += ".capture.json";
    std::filesystem::remove(sidecar, error);

    auto different_extent = baseline;
    different_extent.extent = {2, 8};
    assert(!compare_visual_captures(baseline, different_extent));
    return 0;
}
