#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/scene/render_scene.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>

namespace heartstead::renderer {

class Renderer;

struct LargeWaterRendererConfig {
    std::uint32_t grid_resolution = 65;
    float camera_snap_distance = 24.0F;
    std::uint32_t maximum_bodies = 64;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct LargeWaterBodyDesc {
    std::uint64_t id = 0;
    world::WorldPosition center;
    float half_extent = 1'800.0F;
    float wave_height = 0.14F;
    float wave_speed = 1.0F;
    float optical_depth = 24.0F;
    float foam_strength = 0.35F;
    bool follows_camera = true;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct LargeWaterRendererStats {
    std::uint32_t retained_bodies = 0;
    std::uint32_t ocean_bodies = 0;
    std::uint32_t mesh_vertices = 0;
    std::uint32_t mesh_triangles = 0;
    std::uint64_t camera_recenters = 0;
};

// Draws geometric, camera-relative large-water coverage beyond editable voxel-water chunks.
// A quadratic grid distribution keeps tessellation dense near the camera and sparse at the
// horizon, so each ocean remains a single instanced draw with stable large-coordinate placement.
class LargeWaterRenderer {
  public:
    [[nodiscard]] core::Status initialize(Renderer& renderer,
                                          LargeWaterRendererConfig config = {});
    [[nodiscard]] core::Status add_body(LargeWaterBodyDesc body);
    [[nodiscard]] core::Status update_body(const LargeWaterBodyDesc& body);
    [[nodiscard]] core::Status remove_body(std::uint64_t id);
    [[nodiscard]] core::Status synchronize(const RenderCamera& camera);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const LargeWaterRendererStats& stats() const noexcept;

  private:
    struct RetainedBody {
        LargeWaterBodyDesc desc;
        RenderObjectProxy proxy;
    };

    [[nodiscard]] core::Result<RenderObjectProxy>
    make_proxy(const LargeWaterBodyDesc& body) const;
    [[nodiscard]] core::Result<world::WorldPosition>
    snapped_ocean_center(const LargeWaterBodyDesc& body, const RenderCamera& camera) const;
    void refresh_stats() noexcept;

    Renderer* renderer_ = nullptr;
    LargeWaterRendererConfig config_{};
    RenderMeshHandle mesh_{};
    std::unordered_map<std::uint64_t, RetainedBody> bodies_;
    LargeWaterRendererStats stats_{};
};

} // namespace heartstead::renderer
