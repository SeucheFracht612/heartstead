#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/world/coords/world_position.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace heartstead::renderer {

struct TrailId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        return index != 0U && generation != 0U;
    }
    friend auto operator<=>(const TrailId&, const TrailId&) = default;
};

struct TrailDesc {
    std::uint8_t material_group = 0;
    RenderLayer layer = RenderLayer::premultiplied;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    float width = 0.08F;
    float segment_lifetime_seconds = 0.45F;
    float minimum_point_distance = 0.04F;
    std::uint32_t maximum_segments = 64;
    bool emissive = false;
    float emissive_intensity = 1.0F;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct TrailRendererConfig {
    std::array<MaterialRuntimeHandle, 4> material_groups{};
    std::uint32_t maximum_trails = 1'024;
    std::uint32_t maximum_segments = 65'536;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct TrailRendererStats {
    std::uint32_t retained_trails = 0;
    std::uint32_t retained_segments = 0;
    std::uint32_t submitted_segments = 0;
    std::uint32_t expired_segments = 0;
    std::uint64_t dropped_segments = 0;
};

class TrailRenderer {
  public:
    [[nodiscard]] core::Status initialize(Renderer& renderer,
                                          TrailRendererConfig config = {});
    [[nodiscard]] core::Result<TrailId> create_trail(TrailDesc trail);
    [[nodiscard]] core::Status append_point(TrailId id,
                                            const world::WorldPosition& point);
    [[nodiscard]] core::Status update(const RenderCamera& camera, float delta_seconds);
    [[nodiscard]] core::Status destroy_trail(TrailId id);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const TrailRendererStats& stats() const noexcept;

  private:
    struct Segment {
        world::WorldPosition start;
        world::WorldPosition end;
        RenderObjectProxy proxy;
        float age_seconds = 0.0F;
    };

    struct TrailSlot {
        std::uint32_t generation = 1;
        bool occupied = false;
        TrailDesc desc;
        std::vector<Segment> segments;
        world::WorldPosition last_point;
        bool has_last_point = false;
    };

    [[nodiscard]] TrailSlot* find(TrailId id) noexcept;
    [[nodiscard]] core::Status remove_segment(TrailSlot& trail, std::size_t index);
    [[nodiscard]] core::Status update_segment(Segment& segment, const TrailDesc& trail,
                                              const RenderCamera& camera);
    void refresh_stats() noexcept;

    Renderer* renderer_ = nullptr;
    TrailRendererConfig config_{};
    RenderMeshHandle quad_mesh_{};
    std::vector<TrailSlot> trails_;
    std::vector<std::uint32_t> free_trails_;
    TrailRendererStats stats_{};
};

} // namespace heartstead::renderer
