#pragma once

#include "engine/math/vector.hpp"
#include "engine/renderer/camera/frustum.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace heartstead::renderer {

using VisibilityKey = std::uint64_t;
using VisibilityViewId = std::uint32_t;

enum class VisibilityViewKind : std::uint8_t {
    main,
    directional_shadow,
    local_shadow,
    reflection,
    map,
};

struct VisibilityLodLevel {
    std::uint32_t level = 0;
    // Maximum object-space deviation from LOD 0. LOD 0 should use zero.
    float geometric_error = 0.0F;
    bool resident = true;
};

struct VisibilityObject {
    VisibilityKey key = 0;
    math::Bounds3d world_bounds{};
    std::vector<VisibilityLodLevel> lods;
    double maximum_distance = 0.0;
    float importance = 1.0F;
    bool casts_shadow = true;
    bool allow_occlusion = true;
    math::Vec3d distance_reference{};
    bool use_distance_reference = false;
};

struct VisibilityView {
    VisibilityViewId id = 0;
    VisibilityViewKind kind = VisibilityViewKind::main;
    math::Vec3d camera_world{};
    RenderFrustum camera_relative_frustum{};
    std::uint32_t viewport_height = 1;
    float vertical_field_of_view_radians = 1.0471976F;
    double maximum_distance = 0.0;
};

struct VisibilitySelection {
    VisibilityKey key = 0;
    VisibilityViewId view_id = 0;
    std::uint32_t lod_level = 0;
    double distance = 0.0;
    float projected_error_pixels = 0.0F;
    float streaming_priority = 0.0F;
};

struct VisibilityQueryStats {
    std::size_t hierarchy_nodes_tested = 0;
    std::size_t hierarchy_nodes_culled = 0;
    std::size_t objects_tested = 0;
    std::size_t distance_culled = 0;
    std::size_t frustum_culled = 0;
    std::size_t shadow_policy_culled = 0;
    std::size_t occlusion_culled = 0;
    std::size_t selections_emitted = 0;
};

struct VisibilityQueryResult {
    std::vector<VisibilitySelection> selections;
    VisibilityQueryStats stats;
};

// Stores delayed HZB/query results without making the CPU hierarchy depend on a particular
// occlusion implementation. An object is rejected only after consecutive results from adjacent
// frames, preventing a stale single-frame result from making newly revealed geometry disappear.
class TemporalOcclusionHistory {
  public:
    void begin_frame(std::uint64_t frame_index, bool camera_cut = false);
    void submit(VisibilityViewId view_id, VisibilityKey key, bool occluded);
    [[nodiscard]] bool is_conservatively_occluded(VisibilityViewId view_id,
                                                   VisibilityKey key) const noexcept;
    void clear() noexcept;

  private:
    struct Key {
        VisibilityViewId view_id = 0;
        VisibilityKey object_id = 0;

        friend bool operator==(const Key&, const Key&) = default;
    };

    struct KeyHash {
        [[nodiscard]] std::size_t operator()(const Key& key) const noexcept;
    };

    struct Entry {
        std::uint64_t frame_index = 0;
        std::uint8_t consecutive_occluded_frames = 0;
        bool occluded = false;
    };

    std::unordered_map<Key, Entry, KeyHash> entries_;
    std::uint64_t frame_index_ = 0;
};

class VisibilityHierarchy {
  public:
    struct QueryOptions {
        float maximum_lod_error_pixels = 2.0F;
        float minimum_occlusion_distance = 8.0F;
        const TemporalOcclusionHistory* occlusion_history = nullptr;
    };

    void rebuild(std::span<const VisibilityObject> objects);
    void upsert(VisibilityObject object);
    [[nodiscard]] bool erase(VisibilityKey key);
    void clear() noexcept;
    [[nodiscard]] VisibilityQueryResult query(std::span<const VisibilityView> views) const;
    [[nodiscard]] VisibilityQueryResult query(std::span<const VisibilityView> views,
                                               QueryOptions options) const;
    [[nodiscard]] std::size_t object_count() const noexcept;
    [[nodiscard]] std::size_t node_count() const;

  private:
    struct Node {
        math::Bounds3d bounds{};
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::uint32_t left = 0;
        std::uint32_t right = 0;
        std::uint32_t parent = 0;
        bool leaf = false;
    };

    [[nodiscard]] std::uint32_t build_node(std::uint32_t first, std::uint32_t count,
                                           std::uint32_t parent);
    void rebuild_owned();
    void ensure_built() const;
    void refit_from(std::uint32_t node_index);

    std::vector<VisibilityObject> objects_;
    std::vector<std::uint32_t> object_order_;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> object_leaf_;
    std::unordered_map<VisibilityKey, std::uint32_t> object_by_key_;
    bool topology_dirty_ = false;
};

} // namespace heartstead::renderer
