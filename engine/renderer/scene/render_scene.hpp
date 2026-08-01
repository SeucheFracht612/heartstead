#pragma once

#include "engine/core/result.hpp"
#include "engine/math/matrix.hpp"
#include "engine/renderer/assets/render_asset_handles.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/visibility/visibility_hierarchy.hpp"
#include "engine/world/coords/world_position.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::renderer {

struct RenderObjectId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != 0 && generation != 0;
    }
    friend constexpr auto operator<=>(const RenderObjectId&, const RenderObjectId&) = default;
};

struct RenderLightId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != 0 && generation != 0;
    }
    friend constexpr auto operator<=>(const RenderLightId&, const RenderLightId&) = default;
};

struct RenderSkinPaletteId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return index != 0 && generation != 0;
    }
    friend constexpr auto operator<=>(const RenderSkinPaletteId&,
                                      const RenderSkinPaletteId&) = default;
};

inline constexpr std::uint32_t maximum_render_skin_joints = 256;

enum class RenderLayer : std::uint8_t {
    opaque,
    alpha_tested,
    transparent,
    additive,
    premultiplied,
};

enum class RenderObjectFlags : std::uint32_t {
    none = 0,
    hidden = 1U << 0U,
    teleport = 1U << 1U,
    two_sided = 1U << 2U,
    cast_shadow = 1U << 3U,
};

enum class RenderEffectFlags : std::uint32_t {
    none = 0,
    vegetation = 1U << 0U,
    foliage_transmission = 1U << 1U,
    billboard = 1U << 2U,
    particle = 1U << 3U,
    velocity_aligned = 1U << 4U,
    soft_particle = 1U << 5U,
    water_surface = 1U << 6U,
    unlit_particle = 1U << 7U,
    emissive_particle = 1U << 8U,
    premultiplied_particle = 1U << 9U,
    disable_weather_response = 1U << 10U,
};

[[nodiscard]] constexpr RenderEffectFlags operator|(RenderEffectFlags left,
                                                    RenderEffectFlags right) noexcept {
    return static_cast<RenderEffectFlags>(static_cast<std::uint32_t>(left) |
                                          static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr RenderEffectFlags operator&(RenderEffectFlags left,
                                                    RenderEffectFlags right) noexcept {
    return static_cast<RenderEffectFlags>(static_cast<std::uint32_t>(left) &
                                          static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool any(RenderEffectFlags flags) noexcept {
    return flags != RenderEffectFlags::none;
}

[[nodiscard]] constexpr RenderObjectFlags operator|(RenderObjectFlags left,
                                                    RenderObjectFlags right) noexcept {
    return static_cast<RenderObjectFlags>(static_cast<std::uint32_t>(left) |
                                          static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr RenderObjectFlags operator&(RenderObjectFlags left,
                                                    RenderObjectFlags right) noexcept {
    return static_cast<RenderObjectFlags>(static_cast<std::uint32_t>(left) &
                                          static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr bool any(RenderObjectFlags flags) noexcept {
    return flags != RenderObjectFlags::none;
}

struct RenderObjectProxy {
    RenderObjectId id;
    world::WorldPosition anchor;
    math::Transform3f previous_transform;
    math::Transform3f current_transform;
    // Model-local transform applied after the interpolated entity transform. Animated rigid
    // primitives use this for their evaluated glTF node matrix; static callers keep identity.
    math::Mat4f model_transform = math::Mat4f::identity();
    RenderMeshHandle mesh;
    MaterialRuntimeHandle material;
    math::Bounds3f local_bounds{};
    RenderObjectId parent;
    RenderSkinPaletteId skin_palette;
    RenderLayer layer = RenderLayer::opaque;
    RenderObjectFlags flags = RenderObjectFlags::none;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::vector<float> morph_weights;
    std::uint32_t sprite_frame = 0;
    std::uint16_t atlas_columns = 1;
    std::uint16_t atlas_rows = 1;
    RenderEffectFlags effect_flags = RenderEffectFlags::none;
    float wind_phase = 0.0F;
    // Zero bends freely; one is rigid.
    float wind_stiffness = 1.0F;
    float foliage_transmission = 0.0F;
    float water_wave_height = 0.0F;
    float water_wave_speed = 1.0F;
    float water_optical_depth = 8.0F;
    float water_foam_strength = 0.0F;
    math::Vec2f water_world_phase{};
    float particle_emissive_intensity = 1.0F;
    float particle_soft_fade_distance = 0.0F;
    float particle_velocity_stretch = 0.0F;
    // Smooth visibility width at minimum/maximum view-distance boundaries. Zero is a hard cut.
    float distance_fade_width = 0.0F;
    // Inclusive lower and exclusive upper camera-distance bounds. A zero maximum is unbounded.
    float minimum_view_distance = 0.0F;
    float maximum_view_distance = 0.0F;
    // Multi-primitive prefab LODs use their shared entity transform origin, rather than each
    // primitive's authored bounds center, to make the whole chain switch atomically.
    bool use_object_origin_for_view_distance = false;
};

enum class RenderLightKind : std::uint8_t {
    directional,
    point,
    spot,
};

struct RenderLightProxy {
    RenderLightId id;
    RenderLightKind kind = RenderLightKind::point;
    world::WorldPosition anchor;
    math::Vec3f direction{0.0F, -1.0F, 0.0F};
    math::Vec3f color{1.0F, 1.0F, 1.0F};
    float intensity = 1.0F;
    float radius = 8.0F;
    // Spot cones are expressed as cosines so the shader avoids trigonometry. Point and
    // directional lights ignore them.
    float inner_cone_cosine = 0.9F;
    float outer_cone_cosine = 0.8F;
    float gameplay_importance = 1.0F;
    bool casts_shadow = false;
    std::uint64_t light_revision = 0;
    std::uint64_t nearby_geometry_revision = 0;
};

struct RenderSkinPaletteProxy {
    RenderSkinPaletteId id;
    std::vector<math::Mat4f> joint_matrices;
};

enum class RenderSceneUpdateKind : std::uint8_t {
    upsert_object,
    remove_object,
    upsert_light,
    remove_light,
    upsert_skin_palette,
    remove_skin_palette,
};

struct RenderSceneUpdate {
    RenderSceneUpdateKind kind = RenderSceneUpdateKind::upsert_object;
    RenderObjectProxy object;
    RenderObjectId object_id;
    RenderLightProxy light;
    RenderLightId light_id;
    RenderSkinPaletteProxy skin_palette;
    RenderSkinPaletteId skin_palette_id;
};

struct RenderObjectInstance {
    RenderObjectId id;
    RenderMeshHandle mesh;
    MaterialRuntimeHandle material;
    RenderSkinPaletteId skin_palette;
    RenderLayer layer = RenderLayer::opaque;
    math::Mat4f camera_relative_transform = math::Mat4f::identity();
    math::Bounds3f camera_relative_bounds{};
    std::array<float, 4> color{};
    std::vector<float> morph_weights;
    std::uint32_t sprite_frame = 0;
    std::uint16_t atlas_columns = 1;
    std::uint16_t atlas_rows = 1;
    RenderEffectFlags effect_flags = RenderEffectFlags::none;
    float wind_phase = 0.0F;
    float wind_stiffness = 1.0F;
    float foliage_transmission = 0.0F;
    math::Vec2f water_world_phase{};
    std::array<float, 4> effect_parameters2{};
    float visibility = 1.0F;
    bool camera_visible = true;
    std::uint64_t shadow_visibility_mask = 0;
    bool reset_motion_history = false;
};

struct RenderInstanceBatch {
    RenderMeshHandle mesh;
    MaterialRuntimeHandle material;
    RenderLayer layer = RenderLayer::opaque;
    bool two_sided = false;
    bool casts_shadow = false;
    bool camera_visible = true;
    std::uint64_t shadow_visibility_mask = 0;
    std::vector<RenderObjectInstance> instances;
};

struct RenderLightInstance {
    RenderLightId id;
    RenderLightKind kind = RenderLightKind::point;
    math::Vec3f camera_relative_position{};
    math::Vec3f direction{};
    math::Vec3f color{};
    float intensity = 0.0F;
    float radius = 0.0F;
    float inner_cone_cosine = 0.9F;
    float outer_cone_cosine = 0.8F;
    float gameplay_importance = 1.0F;
    bool casts_shadow = false;
    std::uint64_t light_revision = 0;
    std::uint64_t nearby_geometry_revision = 0;
};

struct RenderSceneStats {
    std::uint32_t retained_objects = 0;
    std::uint32_t retained_lights = 0;
    std::uint32_t retained_skin_palettes = 0;
    std::uint32_t visible_objects = 0;
    std::uint32_t culled_objects = 0;
    std::uint32_t hidden_objects = 0;
    std::uint32_t instance_batches = 0;
    std::uint32_t visibility_hierarchy_nodes = 0;
    std::uint32_t visibility_nodes_tested = 0;
    std::uint32_t visibility_nodes_culled = 0;
};

struct RenderSceneFrame {
    std::vector<RenderInstanceBatch> batches;
    std::vector<RenderLightInstance> lights;
    RenderSceneStats stats;
};

class RenderScene {
  public:
    [[nodiscard]] RenderObjectId reserve_object_id();
    [[nodiscard]] RenderLightId reserve_light_id();
    [[nodiscard]] RenderSkinPaletteId reserve_skin_palette_id();
    [[nodiscard]] core::Result<RenderObjectId> create_object(RenderObjectProxy object);
    [[nodiscard]] core::Result<RenderLightId> create_light(RenderLightProxy light);
    [[nodiscard]] core::Result<RenderSkinPaletteId>
    create_skin_palette(RenderSkinPaletteProxy palette);
    [[nodiscard]] core::Status upsert_object(const RenderObjectProxy& object);
    [[nodiscard]] core::Status remove_object(RenderObjectId id);
    [[nodiscard]] core::Status upsert_light(const RenderLightProxy& light);
    [[nodiscard]] core::Status remove_light(RenderLightId id);
    [[nodiscard]] core::Status upsert_skin_palette(const RenderSkinPaletteProxy& palette);
    [[nodiscard]] core::Status remove_skin_palette(RenderSkinPaletteId id);
    [[nodiscard]] core::Status apply(std::span<const RenderSceneUpdate> updates);

    [[nodiscard]] core::Result<RenderSceneFrame>
    extract(const RenderCamera& camera, float simulation_alpha,
            std::span<const math::Mat4f> shadow_view_projections = {}) const;
    [[nodiscard]] std::vector<RenderLightInstance> extract_lights(const RenderCamera& camera) const;
    void clear() noexcept;

    [[nodiscard]] const RenderObjectProxy* find_object(RenderObjectId id) const noexcept;
    [[nodiscard]] const RenderLightProxy* find_light(RenderLightId id) const noexcept;
    [[nodiscard]] const RenderSkinPaletteProxy*
    find_skin_palette(RenderSkinPaletteId id) const noexcept;
    [[nodiscard]] RenderSceneStats stats() const noexcept;

  private:
    struct ObjectSlot {
        std::uint32_t generation = 1;
        bool reserved = false;
        bool occupied = false;
        std::uint32_t child_count = 0;
        RenderObjectProxy proxy;
    };
    struct LightSlot {
        std::uint32_t generation = 1;
        bool reserved = false;
        bool occupied = false;
        RenderLightProxy proxy;
    };
    struct SkinPaletteSlot {
        std::uint32_t generation = 1;
        bool reserved = false;
        bool occupied = false;
        RenderSkinPaletteProxy proxy;
    };

    std::vector<ObjectSlot> objects_;
    std::vector<LightSlot> lights_;
    std::vector<SkinPaletteSlot> skin_palettes_;
    std::vector<std::uint32_t> free_objects_;
    std::vector<std::uint32_t> free_lights_;
    std::vector<std::uint32_t> free_skin_palettes_;
    mutable VisibilityHierarchy visibility_hierarchy_;
};

[[nodiscard]] core::Status validate_render_object_proxy(const RenderObjectProxy& object);
[[nodiscard]] core::Status validate_render_light_proxy(const RenderLightProxy& light);
[[nodiscard]] core::Status
validate_render_skin_palette_proxy(const RenderSkinPaletteProxy& palette);
[[nodiscard]] math::Transform3f interpolate_render_transform(const RenderObjectProxy& object,
                                                             float simulation_alpha) noexcept;

} // namespace heartstead::renderer
