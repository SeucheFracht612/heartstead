#pragma once

#include "engine/core/result.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/world/coords/world_position.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::renderer {

enum class SurfaceMarkBlendMode : std::uint8_t {
    alpha,
    premultiplied_alpha,
    additive,
};

struct SurfaceMarkPrototype {
    core::PrototypeId id;
    std::string display_name;
    std::uint8_t material_group = 0;
    SurfaceMarkBlendMode blend_mode = SurfaceMarkBlendMode::premultiplied_alpha;
    float size_min = 0.2F;
    float size_max = 0.5F;
    float lifetime_seconds = 0.0F;
    float fade_seconds = 0.0F;
    float surface_offset = 0.006F;
    float maximum_distance = 96.0F;
    std::uint16_t atlas_columns = 1;
    std::uint16_t atlas_rows = 1;
    std::uint16_t atlas_frame_count = 1;
    bool receives_lighting = true;

    [[nodiscard]] core::Status validate() const noexcept;
};

class SurfaceMarkPrototypeRegistry {
  public:
    [[nodiscard]] core::Status add(SurfaceMarkPrototype prototype);
    [[nodiscard]] const SurfaceMarkPrototype*
    find(const core::PrototypeId& id) const noexcept;
    [[nodiscard]] std::span<const SurfaceMarkPrototype> prototypes() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::vector<SurfaceMarkPrototype> prototypes_;
};

[[nodiscard]] core::Result<SurfaceMarkPrototype>
surface_mark_prototype_from_generic(const modding::GenericPrototype& prototype);
[[nodiscard]] core::Result<SurfaceMarkPrototypeRegistry>
surface_mark_registry_from_prototypes(const modding::PrototypeRegistry& prototypes);

struct SurfaceMarkSpawn {
    core::PrototypeId prototype;
    world::WorldPosition position;
    math::Vec3f normal{0.0F, 1.0F, 0.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    float size_multiplier = 1.0F;
    float rotation_degrees = 0.0F;
    std::uint64_t seed = 1;
};

struct SurfaceMarkRendererConfig {
    std::array<MaterialRuntimeHandle, 4> material_groups{};
    std::uint32_t maximum_marks = 16'384;
    std::uint32_t maximum_spawns_per_update = 1'024;

    [[nodiscard]] core::Status validate() const noexcept;
};

struct SurfaceMarkRendererStats {
    std::uint32_t retained_marks = 0;
    std::uint32_t spawned_this_update = 0;
    std::uint32_t expired_this_update = 0;
    std::uint64_t dropped_marks = 0;
};

class SurfaceMarkRenderer {
  public:
    [[nodiscard]] core::Status
    initialize(Renderer& renderer, const SurfaceMarkPrototypeRegistry& prototypes,
               SurfaceMarkRendererConfig config = {});
    [[nodiscard]] core::Status spawn(const SurfaceMarkSpawn& mark);
    [[nodiscard]] core::Status update(float delta_seconds);
    [[nodiscard]] core::Status clear();
    [[nodiscard]] core::Status shutdown();

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const SurfaceMarkRendererStats& stats() const noexcept;

  private:
    struct RetainedMark {
        RenderObjectProxy proxy;
        std::array<float, 4> base_color{};
        float age_seconds = 0.0F;
        float lifetime_seconds = 0.0F;
        float fade_seconds = 0.0F;
    };

    Renderer* renderer_ = nullptr;
    const SurfaceMarkPrototypeRegistry* prototypes_ = nullptr;
    SurfaceMarkRendererConfig config_{};
    RenderMeshHandle quad_mesh_{};
    std::vector<RetainedMark> marks_;
    SurfaceMarkRendererStats stats_{};
    std::uint32_t spawned_in_window_ = 0;
};

} // namespace heartstead::renderer
