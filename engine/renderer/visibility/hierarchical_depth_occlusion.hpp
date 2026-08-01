#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/renderer/render_camera.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace heartstead::renderer {

struct HierarchicalDepthOcclusionConfig {
    std::uint32_t width = 160;
    std::uint32_t height = 90;
    float depth_bias = 0.002F;
    std::uint32_t confirmation_frames = 2;
    float camera_cut_distance = 48.0F;
    float camera_cut_angle_radians = 0.45F;

    [[nodiscard]] core::Status validate() const;
};

struct HierarchicalDepthOcclusionStats {
    std::uint32_t mip_levels = 0;
    std::uint32_t submitted_occluders = 0;
    std::uint32_t rasterized_occluders = 0;
    std::uint32_t tested_bounds = 0;
    std::uint32_t raw_occluded_bounds = 0;
    std::uint32_t confirmed_occluded_bounds = 0;
    std::uint64_t camera_cut_resets = 0;
};

// Conservative software HZB used for coarse patch culling. Occluder boxes are rasterized using
// their farthest depth, then max-reduced. A query is hidden only when every covered hierarchy
// sample is nearer than the query's nearest point. Fine object culling remains in the GPU path.
class HierarchicalDepthOcclusion {
  public:
    [[nodiscard]] core::Status initialize(HierarchicalDepthOcclusionConfig config = {});
    [[nodiscard]] core::Status rebuild(const RenderCamera& camera,
                                       std::span<const math::Bounds3f> occluders);
    [[nodiscard]] bool query(std::uint64_t object_id, const math::Bounds3f& bounds);
    void erase(std::uint64_t object_id) noexcept;
    void reset_history() noexcept;

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const HierarchicalDepthOcclusionStats& stats() const noexcept;

  private:
    struct MipLevel {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<float> depth;
    };

    struct HistoryEntry {
        std::uint32_t consecutive_occluded_frames = 0;
        bool confirmed = false;
    };

    HierarchicalDepthOcclusionConfig config_{};
    HierarchicalDepthOcclusionStats stats_{};
    std::vector<MipLevel> mips_;
    std::unordered_map<std::uint64_t, HistoryEntry> history_;
    RenderCamera previous_camera_{};
    bool has_previous_camera_ = false;
};

} // namespace heartstead::renderer
