#pragma once

#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/vegetation/vegetation_species.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace heartstead::renderer {

class Renderer;

struct VegetationPatchDesc {
    std::uint64_t id = 0;
    core::PrototypeId species;
    world::WorldPosition origin;
    math::Vec2f extent{1.0F, 1.0F};
    std::uint32_t instance_count = 1;
    std::uint64_t seed = 1;
    std::string growth_state;

    [[nodiscard]] core::Status validate() const;
};

using VegetationHeightSampler = std::function<float(float local_x, float local_z)>;

struct VegetationRendererConfig {
    std::uint32_t maximum_patches = 4'096;
    std::uint32_t maximum_logical_instances = 250'000;
    std::uint32_t maximum_render_objects = 750'000;

    [[nodiscard]] core::Status validate() const;
};

struct VegetationRendererStats {
    std::uint32_t loaded_species = 0;
    std::uint32_t loaded_models = 0;
    std::uint32_t retained_patches = 0;
    std::uint32_t logical_instances = 0;
    std::uint32_t render_objects = 0;
    std::uint32_t density_rejected_lods = 0;
    std::uint32_t occluded_patches = 0;
    std::uint32_t visibility_updates = 0;
};

// Dedicated high-volume vegetation presentation. Geometry and materials are loaded from the
// production cooked store once; generated plants are submitted through RenderScene, where equal
// mesh/material/LOD records become one hardware-instanced draw.
class VegetationRenderer {
  public:
    VegetationRenderer();
    ~VegetationRenderer();

    VegetationRenderer(const VegetationRenderer&) = delete;
    VegetationRenderer& operator=(const VegetationRenderer&) = delete;
    VegetationRenderer(VegetationRenderer&&) noexcept;
    VegetationRenderer& operator=(VegetationRenderer&&) noexcept;

    [[nodiscard]] core::Status
    initialize(Renderer& renderer, const VegetationSpeciesRegistry& registry,
               const std::filesystem::path& cooked_asset_root,
               VegetationRendererConfig config = {});
    [[nodiscard]] core::Status upsert_patch(VegetationPatchDesc patch,
                                            VegetationHeightSampler height_sampler = {});
    [[nodiscard]] core::Status remove_patch(std::uint64_t patch_id);
    [[nodiscard]] core::Status
    update_occlusion(const RenderCamera& camera,
                     std::span<const math::Bounds3f> camera_relative_occluders);
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const VegetationRendererStats& stats() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heartstead::renderer
