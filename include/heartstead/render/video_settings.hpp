#pragma once

#include <cstdint>

namespace heartstead {

struct VideoSettings {
    std::int32_t render_distance_chunks{64};
    std::int32_t render_distance_scale_max{128};
    float distance_smoothing_start{160.0F};
    float fog_start_fraction{0.82F};
    std::int32_t shadow_distance_blocks{224};
    bool vsync{true};
    bool fullscreen{};
};

} // namespace heartstead
