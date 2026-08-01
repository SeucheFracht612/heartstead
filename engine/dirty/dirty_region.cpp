#include "engine/dirty/dirty_region.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace heartstead::dirty {

namespace {

[[nodiscard]] std::int64_t saturated_increment(std::int64_t value) noexcept {
    if (value == std::numeric_limits<std::int64_t>::max()) {
        return value;
    }
    return value + 1;
}

[[nodiscard]] bool axis_overlaps_or_touches(std::int64_t min_a, std::int64_t max_a,
                                            std::int64_t min_b, std::int64_t max_b) noexcept {
    return min_a <= saturated_increment(max_b) && min_b <= saturated_increment(max_a);
}

[[nodiscard]] std::int64_t saturated_subtract(std::int64_t value, std::uint32_t amount) noexcept {
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    const auto delta = static_cast<std::int64_t>(amount);
    if (value < min + delta) {
        return min;
    }
    return value - delta;
}

[[nodiscard]] std::int64_t saturated_add(std::int64_t value, std::uint32_t amount) noexcept {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    const auto delta = static_cast<std::int64_t>(amount);
    if (value > max - delta) {
        return max;
    }
    return value + delta;
}

} // namespace

DirtyRegionBounds DirtyRegionBounds::single(DirtyRegionCoord coord) noexcept {
    return DirtyRegionBounds{coord, coord};
}

bool DirtyRegionBounds::is_valid() const noexcept {
    return min.x <= max.x && min.y <= max.y && min.z <= max.z;
}

bool DirtyRegionBounds::overlaps_or_touches(const DirtyRegionBounds& other) const noexcept {
    return is_valid() && other.is_valid() &&
           axis_overlaps_or_touches(min.x, max.x, other.min.x, other.max.x) &&
           axis_overlaps_or_touches(min.y, max.y, other.min.y, other.max.y) &&
           axis_overlaps_or_touches(min.z, max.z, other.min.z, other.max.z);
}

DirtyRegionBounds DirtyRegionBounds::merged_with(const DirtyRegionBounds& other) const noexcept {
    return DirtyRegionBounds{
        {std::min(min.x, other.min.x), std::min(min.y, other.min.y), std::min(min.z, other.min.z)},
        {std::max(max.x, other.max.x), std::max(max.y, other.max.y), std::max(max.z, other.max.z)}};
}

core::Result<DirtyRegionBounds> DirtyRegionBounds::expanded(std::uint32_t padding) const {
    if (!is_valid()) {
        return core::Result<DirtyRegionBounds>::failure("dirty_region.invalid_bounds",
                                                        "dirty region bounds are invalid");
    }

    return core::Result<DirtyRegionBounds>::success(
        DirtyRegionBounds{{saturated_subtract(min.x, padding), saturated_subtract(min.y, padding),
                           saturated_subtract(min.z, padding)},
                          {saturated_add(max.x, padding), saturated_add(max.y, padding),
                           saturated_add(max.z, padding)}});
}

core::Status DirtyRegionTracker::mark(DirtyRegionKind kind, DirtyRegionBounds bounds,
                                      std::string reason) {
    if (!bounds.is_valid()) {
        return core::Status::failure("dirty_region.invalid_bounds",
                                     "dirty region bounds are invalid");
    }

    const auto marked_at = DirtyRegionClock::now();
    auto earliest_mark = marked_at;
    std::vector<bool> merge_regions(regions_.size(), false);
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (std::size_t index = 0; index < regions_.size(); ++index) {
            if (merge_regions[index] || regions_[index].kind != kind ||
                !regions_[index].bounds.overlaps_or_touches(bounds)) {
                continue;
            }
            merge_regions[index] = true;
            bounds = bounds.merged_with(regions_[index].bounds);
            earliest_mark = std::min(earliest_mark, regions_[index].marked_at);
            expanded = true;
        }
    }

    const auto survivor = std::ranges::find(merge_regions, true);
    if (survivor != merge_regions.end()) {
        const auto survivor_index = static_cast<std::size_t>(survivor - merge_regions.begin());
        auto merged_reason = regions_[survivor_index].reason;
        if (merged_reason.empty()) {
            for (std::size_t index = survivor_index + 1; index < regions_.size(); ++index) {
                if (merge_regions[index] && !regions_[index].reason.empty()) {
                    merged_reason = regions_[index].reason;
                    break;
                }
            }
        }
        if (merged_reason.empty()) {
            merged_reason = std::move(reason);
        }

        regions_[survivor_index].bounds = bounds;
        regions_[survivor_index].reason = std::move(merged_reason);
        regions_[survivor_index].marked_at = earliest_mark;
        for (std::size_t index = regions_.size(); index-- > survivor_index + 1;) {
            if (merge_regions[index]) {
                regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        return core::Status::ok();
    }

    regions_.push_back(DirtyRegion{kind, bounds, std::move(reason), next_sequence_, marked_at});
    ++next_sequence_;
    return core::Status::ok();
}

core::Status DirtyRegionTracker::mark_single(DirtyRegionKind kind, DirtyRegionCoord coord,
                                             std::string reason) {
    return mark(kind, DirtyRegionBounds::single(coord), std::move(reason));
}

bool DirtyRegionTracker::empty() const noexcept {
    return regions_.empty();
}

std::size_t DirtyRegionTracker::size() const noexcept {
    return regions_.size();
}

std::size_t DirtyRegionTracker::count(DirtyRegionKind kind) const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        regions_, [kind](const DirtyRegion& region) { return region.kind == kind; }));
}

const std::vector<DirtyRegion>& DirtyRegionTracker::regions() const noexcept {
    return regions_;
}

std::vector<DirtyRegion> DirtyRegionTracker::consume_kind(DirtyRegionKind kind) {
    std::vector<DirtyRegion> consumed;
    auto output = regions_.begin();
    for (auto input = regions_.begin(); input != regions_.end(); ++input) {
        if (input->kind == kind) {
            consumed.push_back(std::move(*input));
        } else {
            if (output != input) {
                *output = std::move(*input);
            }
            ++output;
        }
    }
    regions_.erase(output, regions_.end());
    return consumed;
}

std::vector<DirtyRegion> DirtyRegionTracker::consume_all() {
    auto consumed = std::move(regions_);
    regions_.clear();
    return consumed;
}

void DirtyRegionTracker::clear() noexcept {
    regions_.clear();
}

std::string_view dirty_region_kind_name(DirtyRegionKind kind) noexcept {
    switch (kind) {
    case DirtyRegionKind::chunk_mesh:
        return "chunk_mesh";
    case DirtyRegionKind::chunk_collision:
        return "chunk_collision";
    case DirtyRegionKind::chunk_lighting:
        return "chunk_lighting";
    case DirtyRegionKind::room_graph:
        return "room_graph";
    case DirtyRegionKind::road_network:
        return "road_network";
    case DirtyRegionKind::cart_access_network:
        return "cart_access_network";
    case DirtyRegionKind::storage_access_network:
        return "storage_access_network";
    case DirtyRegionKind::power_network:
        return "power_network";
    case DirtyRegionKind::ward_network:
        return "ward_network";
    case DirtyRegionKind::smoke_ventilation_network:
        return "smoke_ventilation_network";
    case DirtyRegionKind::water_network:
        return "water_network";
    case DirtyRegionKind::logistics_network:
        return "logistics_network";
    }
    return "unknown";
}

} // namespace heartstead::dirty
