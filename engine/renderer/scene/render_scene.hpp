#pragma once

#include "engine/core/result.hpp"
#include "engine/math/matrix.hpp"
#include "engine/renderer/assets/render_asset_handles.hpp"
#include "engine/renderer/render_camera.hpp"
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
};

enum class RenderObjectFlags : std::uint32_t {
    none = 0,
    hidden = 1U << 0U,
    teleport = 1U << 1U,
    two_sided = 1U << 2U,
    cast_shadow = 1U << 3U,
};

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
    RenderMeshHandle mesh;
    MaterialRuntimeHandle material;
    math::Bounds3f local_bounds{};
    RenderObjectId parent;
    RenderSkinPaletteId skin_palette;
    RenderLayer layer = RenderLayer::opaque;
    RenderObjectFlags flags = RenderObjectFlags::none;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint32_t sprite_frame = 0;
};

enum class RenderLightKind : std::uint8_t {
    directional,
    point,
};

struct RenderLightProxy {
    RenderLightId id;
    RenderLightKind kind = RenderLightKind::point;
    world::WorldPosition anchor;
    math::Vec3f direction{0.0F, -1.0F, 0.0F};
    math::Vec3f color{1.0F, 1.0F, 1.0F};
    float intensity = 1.0F;
    float radius = 8.0F;
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
    std::uint32_t sprite_frame = 0;
};

struct RenderInstanceBatch {
    RenderMeshHandle mesh;
    MaterialRuntimeHandle material;
    RenderLayer layer = RenderLayer::opaque;
    bool two_sided = false;
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
};

struct RenderSceneStats {
    std::uint32_t retained_objects = 0;
    std::uint32_t retained_lights = 0;
    std::uint32_t retained_skin_palettes = 0;
    std::uint32_t visible_objects = 0;
    std::uint32_t culled_objects = 0;
    std::uint32_t hidden_objects = 0;
    std::uint32_t instance_batches = 0;
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

    [[nodiscard]] core::Result<RenderSceneFrame> extract(const RenderCamera& camera,
                                                         float simulation_alpha) const;
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
};

[[nodiscard]] core::Status validate_render_object_proxy(const RenderObjectProxy& object);
[[nodiscard]] core::Status validate_render_light_proxy(const RenderLightProxy& light);
[[nodiscard]] core::Status
validate_render_skin_palette_proxy(const RenderSkinPaletteProxy& palette);
[[nodiscard]] math::Transform3f interpolate_render_transform(const RenderObjectProxy& object,
                                                             float simulation_alpha) noexcept;

} // namespace heartstead::renderer
