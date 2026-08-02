#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace heartstead::profiling {

struct LatencyWindowStats {
    std::size_t sample_count = 0;
    double latest_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double maximum_ms = 0.0;
    double session_maximum_ms = 0.0;
};

// A fixed-capacity rolling distribution for owner-thread lifecycle measurements. Recording a
// sample never allocates; percentile values use the same linearly interpolated definition as the
// retained renderer and streaming benchmark reports.
template <std::size_t Capacity> class LatencyWindow {
  public:
    static_assert(Capacity > 0);

    void record(double milliseconds) noexcept {
        if (!std::isfinite(milliseconds)) {
            return;
        }
        const auto sample = std::max(0.0, milliseconds);
        samples_[next_sample_] = sample;
        next_sample_ = (next_sample_ + 1U) % Capacity;
        sample_count_ = std::min(sample_count_ + 1U, Capacity);
        stats_.latest_ms = sample;
        stats_.session_maximum_ms = std::max(stats_.session_maximum_ms, sample);
        refresh_distribution();
    }

    void reset() noexcept {
        samples_.fill(0.0);
        sample_count_ = 0;
        next_sample_ = 0;
        stats_ = {};
    }

    [[nodiscard]] const LatencyWindowStats& stats() const noexcept {
        return stats_;
    }

  private:
    [[nodiscard]] static double percentile(std::span<const double> sorted,
                                           double fraction) noexcept {
        if (sorted.empty()) {
            return 0.0;
        }
        const auto position = fraction * static_cast<double>(sorted.size() - 1U);
        const auto lower = static_cast<std::size_t>(std::floor(position));
        const auto upper = static_cast<std::size_t>(std::ceil(position));
        const auto weight = position - static_cast<double>(lower);
        return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
    }

    void refresh_distribution() noexcept {
        std::array<double, Capacity> sorted{};
        std::copy_n(samples_.begin(), sample_count_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sample_count_));
        const std::span<const double> values{sorted.data(), sample_count_};
        stats_.sample_count = sample_count_;
        stats_.median_ms = percentile(values, 0.50);
        stats_.p95_ms = percentile(values, 0.95);
        stats_.p99_ms = percentile(values, 0.99);
        stats_.maximum_ms = values.back();
    }

    std::array<double, Capacity> samples_{};
    std::size_t sample_count_ = 0;
    std::size_t next_sample_ = 0;
    LatencyWindowStats stats_{};
};

} // namespace heartstead::profiling
