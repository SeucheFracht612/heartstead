#pragma once

#include "engine/renderer/lighting/clustered_lighting.hpp"
#include "engine/renderer/rhi/render_frame_plan.hpp"

#include <array>
#include <vector>

namespace heartstead::renderer {

// Renderer-facing command lists. World/chunk concepts stop here and never enter IRenderDevice.
struct RenderCommandLists {
    std::array<std::vector<rhi::RenderDrawCommand>, 4> directional_shadow_draws;
    std::array<std::vector<rhi::RenderDrawCommand>, local_shadow_map_count> local_shadow_draws;
    std::vector<rhi::RenderDrawCommand> sky_draws;
    std::vector<rhi::RenderDrawCommand> opaque_terrain_draws;
    std::vector<rhi::RenderDrawCommand> alpha_tested_terrain_draws;
    std::vector<rhi::RenderDrawCommand> rich_instance_draws;
    std::vector<rhi::RenderDrawCommand> transparent_terrain_draws;
    std::vector<rhi::RenderDrawCommand> debug_draws;
    std::vector<rhi::RenderDrawCommand> ui_draws;
};

} // namespace heartstead::renderer
