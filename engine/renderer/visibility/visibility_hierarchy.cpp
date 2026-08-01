#include "engine/renderer/visibility/visibility_hierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>

namespace heartstead::renderer {

namespace {

constexpr std::uint32_t leaf_capacity = 8;

[[nodiscard]] math::Bounds3d union_bounds(const math::Bounds3d& left,
                                          const math::Bounds3d& right) noexcept {
    if (!left.is_valid()) {
        return right;
    }
    if (!right.is_valid()) {
        return left;
    }
    return {math::component_min(left.min, right.min), math::component_max(left.max, right.max)};
}

[[nodiscard]] math::Vec3d center(const math::Bounds3d& bounds) noexcept {
    return (bounds.min + bounds.max) * 0.5;
}

[[nodiscard]] double distance_to_bounds(math::Vec3d point,
                                        const math::Bounds3d& bounds) noexcept {
    const auto distance_axis = [](double value, double minimum, double maximum) {
        return value < minimum ? minimum - value : (value > maximum ? value - maximum : 0.0);
    };
    const math::Vec3d delta{distance_axis(point.x, bounds.min.x, bounds.max.x),
                            distance_axis(point.y, bounds.min.y, bounds.max.y),
                            distance_axis(point.z, bounds.min.z, bounds.max.z)};
    return std::sqrt(math::length_squared(delta));
}

[[nodiscard]] math::Bounds3f camera_relative_bounds(const math::Bounds3d& bounds,
                                                    math::Vec3d camera) noexcept {
    const auto relative = [camera](math::Vec3d point) {
        return math::Vec3f{static_cast<float>(point.x - camera.x),
                           static_cast<float>(point.y - camera.y),
                           static_cast<float>(point.z - camera.z)};
    };
    return {relative(bounds.min), relative(bounds.max)};
}

[[nodiscard]] double effective_maximum_distance(const VisibilityObject& object,
                                                const VisibilityView& view) noexcept {
    if (object.maximum_distance <= 0.0) {
        return view.maximum_distance;
    }
    if (view.maximum_distance <= 0.0) {
        return object.maximum_distance;
    }
    return std::min(object.maximum_distance, view.maximum_distance);
}

struct LodChoice {
    std::uint32_t level = 0;
    float projected_error_pixels = 0.0F;
};

[[nodiscard]] LodChoice select_lod(const VisibilityObject& object, const VisibilityView& view,
                                   double distance, float maximum_error_pixels) noexcept {
    if (object.lods.empty()) {
        return {};
    }

    const auto half_fov = std::clamp(view.vertical_field_of_view_radians * 0.5F, 0.01F, 1.55F);
    const auto projection_scale =
        static_cast<double>(std::max(view.viewport_height, 1U)) / (2.0 * std::tan(half_fov));
    const auto safe_distance = std::max(distance, 0.01);

    LodChoice fallback{object.lods.front().level, 0.0F};
    for (auto iterator = object.lods.rbegin(); iterator != object.lods.rend(); ++iterator) {
        if (!iterator->resident) {
            continue;
        }
        const auto error = static_cast<float>(static_cast<double>(iterator->geometric_error) *
                                              projection_scale / safe_distance);
        fallback = {iterator->level, error};
        if (error <= maximum_error_pixels) {
            return fallback;
        }
    }
    return fallback;
}

[[nodiscard]] bool is_shadow_view(VisibilityViewKind kind) noexcept {
    return kind == VisibilityViewKind::directional_shadow ||
           kind == VisibilityViewKind::local_shadow;
}

} // namespace

std::size_t TemporalOcclusionHistory::KeyHash::operator()(const Key& key) const noexcept {
    // Hashing may collide, but unordered_map still compares the complete pair for identity.
    auto value = key.object_id ^ (static_cast<std::uint64_t>(key.view_id) << 32U);
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

void TemporalOcclusionHistory::begin_frame(std::uint64_t frame_index, bool camera_cut) {
    frame_index_ = frame_index;
    if (camera_cut) {
        clear();
    }
}

void TemporalOcclusionHistory::submit(VisibilityViewId view_id, VisibilityKey key,
                                      bool occluded) {
    auto& entry = entries_[Key{view_id, key}];
    const auto adjacent = entry.frame_index + 1U == frame_index_;
    entry.consecutive_occluded_frames =
        occluded ? static_cast<std::uint8_t>(adjacent ? std::min<int>(entry.consecutive_occluded_frames + 1, 255)
                                                     : 1)
                 : 0;
    entry.occluded = occluded;
    entry.frame_index = frame_index_;
}

bool TemporalOcclusionHistory::is_conservatively_occluded(VisibilityViewId view_id,
                                                           VisibilityKey key) const noexcept {
    const auto found = entries_.find(Key{view_id, key});
    return found != entries_.end() && found->second.frame_index == frame_index_ &&
           found->second.occluded && found->second.consecutive_occluded_frames >= 2;
}

void TemporalOcclusionHistory::clear() noexcept {
    entries_.clear();
}

void VisibilityHierarchy::rebuild(std::span<const VisibilityObject> objects) {
    std::vector<VisibilityObject> owned(objects.begin(), objects.end());
    objects_ = std::move(owned);
    rebuild_owned();
}

void VisibilityHierarchy::rebuild_owned() {
    std::ranges::stable_sort(objects_, {}, &VisibilityObject::key);
    const auto duplicate = std::ranges::unique(objects_, {}, &VisibilityObject::key);
    objects_.erase(duplicate.begin(), duplicate.end());
    object_order_.resize(objects_.size());
    std::iota(object_order_.begin(), object_order_.end(), 0U);
    object_leaf_.assign(objects_.size(), 0U);
    object_by_key_.clear();
    for (std::uint32_t index = 0; index < objects_.size(); ++index) {
        object_by_key_.insert_or_assign(objects_[index].key, index);
    }
    nodes_.clear();
    nodes_.reserve(objects_.empty() ? 0 : objects_.size() * 2U);
    if (!objects_.empty()) {
        static_cast<void>(build_node(0, static_cast<std::uint32_t>(objects_.size()), 0U));
    }
    topology_dirty_ = false;
}

void VisibilityHierarchy::ensure_built() const {
    if (topology_dirty_) {
        // The hierarchy is a derived acceleration structure. Delaying topology rebuilds until
        // query time batches large presentation create/remove transactions into one deterministic
        // sort and build instead of doing quadratic work.
        const_cast<VisibilityHierarchy*>(this)->rebuild_owned();
    }
}

void VisibilityHierarchy::upsert(VisibilityObject object) {
    const auto found = object_by_key_.find(object.key);
    if (found == object_by_key_.end()) {
        object_by_key_.insert_or_assign(object.key,
                                        static_cast<std::uint32_t>(objects_.size()));
        objects_.push_back(std::move(object));
        topology_dirty_ = true;
        return;
    }
    const auto object_index = found->second;
    objects_[object_index] = std::move(object);
    if (!topology_dirty_ && !object_leaf_.empty()) {
        refit_from(object_leaf_[object_index]);
    }
}

bool VisibilityHierarchy::erase(VisibilityKey key) {
    const auto found = object_by_key_.find(key);
    if (found == object_by_key_.end()) {
        return false;
    }
    const auto removed_index = found->second;
    const auto last_index = static_cast<std::uint32_t>(objects_.size() - 1U);
    object_by_key_.erase(found);
    if (removed_index != last_index) {
        objects_[removed_index] = std::move(objects_.back());
        object_by_key_.insert_or_assign(objects_[removed_index].key, removed_index);
    }
    objects_.pop_back();
    topology_dirty_ = true;
    return true;
}

void VisibilityHierarchy::clear() noexcept {
    objects_.clear();
    object_order_.clear();
    nodes_.clear();
    object_leaf_.clear();
    object_by_key_.clear();
    topology_dirty_ = false;
}

std::uint32_t VisibilityHierarchy::build_node(std::uint32_t first, std::uint32_t count,
                                              std::uint32_t parent) {
    const auto node_index = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({});

    math::Bounds3d bounds{};
    bool has_bounds = false;
    for (std::uint32_t offset = 0; offset < count; ++offset) {
        const auto& candidate = objects_[object_order_[first + offset]].world_bounds;
        bounds = has_bounds ? union_bounds(bounds, candidate) : candidate;
        has_bounds = true;
    }
    nodes_[node_index].bounds = bounds;
    nodes_[node_index].first = first;
    nodes_[node_index].count = count;
    nodes_[node_index].parent = parent;
    if (count <= leaf_capacity) {
        nodes_[node_index].leaf = true;
        for (std::uint32_t offset = 0; offset < count; ++offset) {
            object_leaf_[object_order_[first + offset]] = node_index;
        }
        return node_index;
    }

    const auto extent = bounds.max - bounds.min;
    const auto axis = extent.x >= extent.y && extent.x >= extent.z ? 0U :
                      (extent.y >= extent.z ? 1U : 2U);
    const auto coordinate = [this, axis](std::uint32_t object_index) {
        const auto object_center = center(objects_[object_index].world_bounds);
        return axis == 0U ? object_center.x : (axis == 1U ? object_center.y : object_center.z);
    };
    std::stable_sort(object_order_.begin() + first, object_order_.begin() + first + count,
                     [&coordinate, this](std::uint32_t left, std::uint32_t right) {
                         return std::tuple{coordinate(left), objects_[left].key} <
                                std::tuple{coordinate(right), objects_[right].key};
                     });
    const auto left_count = count / 2U;
    const auto left = build_node(first, left_count, node_index);
    const auto right = build_node(first + left_count, count - left_count, node_index);
    nodes_[node_index].left = left;
    nodes_[node_index].right = right;
    return node_index;
}

void VisibilityHierarchy::refit_from(std::uint32_t node_index) {
    while (node_index < nodes_.size()) {
        auto& node = nodes_[node_index];
        if (node.leaf) {
            bool has_bounds = false;
            for (std::uint32_t offset = 0; offset < node.count; ++offset) {
                const auto& bounds = objects_[object_order_[node.first + offset]].world_bounds;
                node.bounds = has_bounds ? union_bounds(node.bounds, bounds) : bounds;
                has_bounds = true;
            }
        } else {
            node.bounds = union_bounds(nodes_[node.left].bounds, nodes_[node.right].bounds);
        }
        if (node_index == 0U) {
            break;
        }
        node_index = node.parent;
    }
}

VisibilityQueryResult VisibilityHierarchy::query(std::span<const VisibilityView> views) const {
    return query(views, QueryOptions{});
}

VisibilityQueryResult VisibilityHierarchy::query(std::span<const VisibilityView> views,
                                                 QueryOptions options) const {
    ensure_built();
    VisibilityQueryResult result;
    if (nodes_.empty()) {
        return result;
    }
    options.maximum_lod_error_pixels = std::max(options.maximum_lod_error_pixels, 0.0F);

    for (const auto& view : views) {
        std::vector<std::uint32_t> stack{0U};
        while (!stack.empty()) {
            const auto node_index = stack.back();
            stack.pop_back();
            const auto& node = nodes_[node_index];
            ++result.stats.hierarchy_nodes_tested;
            if (!view.camera_relative_frustum.intersects(
                    camera_relative_bounds(node.bounds, view.camera_world))) {
                ++result.stats.hierarchy_nodes_culled;
                continue;
            }
            if (!node.leaf) {
                stack.push_back(node.right);
                stack.push_back(node.left);
                continue;
            }

            for (std::uint32_t offset = 0; offset < node.count; ++offset) {
                const auto& object = objects_[object_order_[node.first + offset]];
                ++result.stats.objects_tested;
                if (is_shadow_view(view.kind) && !object.casts_shadow) {
                    ++result.stats.shadow_policy_culled;
                    continue;
                }
                const auto distance = object.use_distance_reference
                                          ? math::length(object.distance_reference -
                                                         view.camera_world)
                                          : distance_to_bounds(view.camera_world,
                                                               object.world_bounds);
                const auto maximum_distance = effective_maximum_distance(object, view);
                if (maximum_distance > 0.0 && distance > maximum_distance) {
                    ++result.stats.distance_culled;
                    continue;
                }
                if (!view.camera_relative_frustum.intersects(
                        camera_relative_bounds(object.world_bounds, view.camera_world))) {
                    ++result.stats.frustum_culled;
                    continue;
                }
                if (object.allow_occlusion && distance >= options.minimum_occlusion_distance &&
                    options.occlusion_history != nullptr &&
                    options.occlusion_history->is_conservatively_occluded(view.id, object.key)) {
                    ++result.stats.occlusion_culled;
                    continue;
                }

                const auto lod = select_lod(object, view, distance,
                                            options.maximum_lod_error_pixels);
                const auto coverage_priority = 1.0F / static_cast<float>(1.0 + distance);
                result.selections.push_back({object.key, view.id, lod.level, distance,
                                             lod.projected_error_pixels,
                                             std::max(object.importance, 0.0F) *
                                                 coverage_priority});
                ++result.stats.selections_emitted;
            }
        }
    }

    std::ranges::sort(result.selections, [](const auto& left, const auto& right) {
        return std::tuple{left.view_id, -left.streaming_priority, left.key} <
               std::tuple{right.view_id, -right.streaming_priority, right.key};
    });
    return result;
}

std::size_t VisibilityHierarchy::object_count() const noexcept {
    return objects_.size();
}

std::size_t VisibilityHierarchy::node_count() const {
    ensure_built();
    return nodes_.size();
}

} // namespace heartstead::renderer
