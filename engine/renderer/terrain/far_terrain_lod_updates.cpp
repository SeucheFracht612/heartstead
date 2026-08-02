#include "engine/renderer/terrain/far_terrain_lod_updates.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <set>
#include <tuple>

namespace heartstead::renderer {

namespace {

[[nodiscard]] bool intersects_horizontally(const math::Bounds3d& left,
                                           const math::Bounds3d& right) noexcept {
    return left.min.x < right.max.x && left.max.x > right.min.x && left.min.z < right.max.z &&
           left.max.z > right.min.z;
}

[[nodiscard]] bool mesh_input_changed(const FarTerrainPatch& left,
                                      const FarTerrainPatch& right) noexcept {
    return left.horizontal_bounds != right.horizontal_bounds || left.cell_size != right.cell_size ||
           left.resolution != right.resolution || left.finer_coverage != right.finer_coverage;
}

[[nodiscard]] core::Status invalid_result_ticket() {
    return core::Status::failure("renderer.invalid_far_terrain_lod_ticket",
                                 "far-terrain LOD result does not match an in-flight patch update");
}

} // namespace

core::Status FarTerrainLodUpdateConfig::validate(std::uint32_t clipmap_level_count,
                                                 std::uint32_t maximum_updates_per_frame) const {
    if (clipmap_level_count == 0 || maximum_updates_per_frame == 0 || mid_level_count == 0 ||
        mid_level_count > clipmap_level_count || maximum_mid_rebuilds_per_frame == 0 ||
        maximum_far_rebuilds_per_frame == 0) {
        return core::Status::failure(
            "renderer.invalid_far_terrain_lod_update_config",
            "far-terrain LOD updates require valid levels and positive update quotas");
    }
    if (maximum_mid_rebuilds_per_frame > maximum_updates_per_frame ||
        maximum_far_rebuilds_per_frame >
            maximum_updates_per_frame - maximum_mid_rebuilds_per_frame) {
        return core::Status::failure(
            "renderer.invalid_far_terrain_lod_update_budget",
            "mid/far replacement reservations must fit the total patch-update budget");
    }
    return core::Status::ok();
}

core::Result<FarTerrainLodUpdateGraph>
FarTerrainLodUpdateGraph::create(FarTerrainLodUpdateConfig config,
                                 std::uint32_t clipmap_level_count,
                                 std::uint32_t maximum_updates_per_frame) {
    auto status = config.validate(clipmap_level_count, maximum_updates_per_frame);
    if (!status) {
        return core::Result<FarTerrainLodUpdateGraph>::failure(status.error().code,
                                                               status.error().message);
    }
    return core::Result<FarTerrainLodUpdateGraph>::success(
        FarTerrainLodUpdateGraph(config, clipmap_level_count, maximum_updates_per_frame));
}

FarTerrainLodUpdateGraph::FarTerrainLodUpdateGraph(FarTerrainLodUpdateConfig config,
                                                   std::uint32_t clipmap_level_count,
                                                   std::uint32_t maximum_updates_per_frame) noexcept
    : config_(config), clipmap_level_count_(clipmap_level_count),
      maximum_updates_per_frame_(maximum_updates_per_frame) {}

core::Status
FarTerrainLodUpdateGraph::synchronize(const FarTerrainPlan& plan, std::uint64_t surface_revision,
                                      std::span<const math::Bounds3d> invalidated_regions) {
    std::set<FarTerrainPatchKey> desired;
    for (const auto& region : invalidated_regions) {
        if (!region.is_valid()) {
            return core::Status::failure(
                "renderer.invalid_far_terrain_lod_invalidation",
                "far-terrain LOD invalidation bounds must be finite and ordered");
        }
    }
    for (const auto& patch : plan.patches) {
        if (patch.key.level >= clipmap_level_count_ || patch.resolution < 2 ||
            !std::isfinite(patch.cell_size) || patch.cell_size <= 0.0 ||
            !std::isfinite(patch.streaming_priority) || !patch.horizontal_bounds.is_valid()) {
            return core::Status::failure(
                "renderer.invalid_far_terrain_lod_patch",
                "far-terrain LOD plans require valid, finite patch metadata");
        }
        if (!desired.insert(patch.key).second) {
            return core::Status::failure("renderer.duplicate_far_terrain_lod_patch",
                                         "far-terrain LOD plans cannot contain duplicate keys");
        }
        auto found = nodes_.find(patch.key);
        if (found == nodes_.end()) {
            Node node;
            node.patch = patch;
            node.band = band_for(patch.key.level);
            node.source_revision = surface_revision;
            node.request_sequence = next_sequence();
            nodes_.emplace(patch.key, std::move(node));
        } else {
            const auto geometry_changed = mesh_input_changed(found->second.patch, patch);
            found->second.patch = patch;
            found->second.band = band_for(patch.key.level);
            if (geometry_changed) {
                auto status = request_update(found->second, surface_revision, true);
                if (!status) {
                    return status;
                }
            }
        }
    }
    std::erase_if(nodes_, [&desired](const auto& item) { return !desired.contains(item.first); });

    const auto revision_changed =
        surface_revision_.has_value() && *surface_revision_ != surface_revision;
    if (revision_changed) {
        for (auto& [key, node] : nodes_) {
            static_cast<void>(key);
            const auto affected =
                invalidated_regions.empty() ||
                std::ranges::any_of(invalidated_regions, [&node](const auto& region) {
                    return intersects_horizontally(node.patch.horizontal_bounds, region);
                });
            if (!affected || node.source_revision == surface_revision) {
                continue;
            }
            auto status = request_update(node, surface_revision, true);
            if (!status) {
                return status;
            }
        }
    }
    surface_revision_ = surface_revision;

    for (auto& [key, node] : nodes_) {
        static_cast<void>(key);
        if (node.resident_request_revision == node.request_revision) {
            node.pending_frames = 0;
        } else if (node.pending_frames != std::numeric_limits<std::uint64_t>::max()) {
            ++node.pending_frames;
        }
    }
    refresh_stats();
    return core::Status::ok();
}

std::vector<FarTerrainLodUpdateRequest>
FarTerrainLodUpdateGraph::schedule_updates(std::size_t maximum_requests) {
    std::vector<Node*> missing;
    std::vector<Node*> dirty_mid;
    std::vector<Node*> dirty_far;
    std::uint32_t in_flight_mid_replacements = 0;
    std::uint32_t in_flight_far_replacements = 0;
    for (auto& [key, node] : nodes_) {
        static_cast<void>(key);
        if (node.in_flight_request_revision.has_value()) {
            if (node.resident_request_revision.has_value() &&
                node.resident_request_revision != node.request_revision) {
                if (node.band == FarTerrainLodBand::mid) {
                    ++in_flight_mid_replacements;
                } else {
                    ++in_flight_far_replacements;
                }
            }
            continue;
        }
        if (node.resident_request_revision == node.request_revision) {
            continue;
        }
        if (!node.resident_request_revision.has_value()) {
            missing.push_back(&node);
        } else if (node.band == FarTerrainLodBand::mid) {
            dirty_mid.push_back(&node);
        } else {
            dirty_far.push_back(&node);
        }
    }

    const auto older_first = [](const Node* left, const Node* right) {
        if (left->pending_frames != right->pending_frames) {
            return left->pending_frames > right->pending_frames;
        }
        if (left->patch.streaming_priority != right->patch.streaming_priority) {
            return left->patch.streaming_priority > right->patch.streaming_priority;
        }
        return std::tie(left->request_sequence, left->patch.key) <
               std::tie(right->request_sequence, right->patch.key);
    };
    const auto missing_first = [&older_first](const Node* left, const Node* right) {
        if (left->band != right->band) {
            return left->band == FarTerrainLodBand::mid;
        }
        return older_first(left, right);
    };
    std::ranges::sort(dirty_mid, older_first);
    std::ranges::sort(dirty_far, older_first);
    std::ranges::sort(missing, missing_first);

    const auto effective_maximum = std::min<std::size_t>(maximum_updates_per_frame_,
                                                         maximum_requests);
    std::vector<FarTerrainLodUpdateRequest> result;
    result.reserve(effective_maximum);
    const auto admit = [&result, effective_maximum](Node& node) {
        if (result.size() >= effective_maximum) {
            return false;
        }
        node.in_flight_request_revision = node.request_revision;
        result.push_back({node.patch, node.band, node.source_revision, node.request_revision,
                          node.request_sequence, node.pending_frames,
                          node.resident_request_revision.has_value()});
        return true;
    };
    const auto admit_up_to = [&admit](const std::vector<Node*>& candidates, std::uint32_t maximum) {
        std::uint32_t admitted = 0;
        for (auto* node : candidates) {
            if (admitted >= maximum || !admit(*node)) {
                break;
            }
            ++admitted;
        }
    };

    // Reserve edit propagation before filling holes. This keeps coarse derived state converging
    // during continuous traversal while the total cap still bounds render-owner work.
    const auto available_mid_replacements =
        config_.maximum_mid_rebuilds_per_frame > in_flight_mid_replacements
            ? config_.maximum_mid_rebuilds_per_frame - in_flight_mid_replacements
            : 0U;
    const auto available_far_replacements =
        config_.maximum_far_rebuilds_per_frame > in_flight_far_replacements
            ? config_.maximum_far_rebuilds_per_frame - in_flight_far_replacements
            : 0U;
    admit_up_to(dirty_mid, available_mid_replacements);
    admit_up_to(dirty_far, available_far_replacements);
    for (auto* node : missing) {
        if (!admit(*node)) {
            break;
        }
    }
    refresh_stats();
    return result;
}

bool FarTerrainLodUpdateGraph::accepts_result(const FarTerrainPatchKey& key,
                                              std::uint64_t request_revision) const noexcept {
    const auto found = nodes_.find(key);
    return found != nodes_.end() && found->second.in_flight_request_revision == request_revision &&
           found->second.request_revision == request_revision;
}

core::Status FarTerrainLodUpdateGraph::publish(const FarTerrainPatchKey& key,
                                               std::uint64_t request_revision) {
    const auto found = nodes_.find(key);
    if (found == nodes_.end() || found->second.in_flight_request_revision != request_revision ||
        found->second.request_revision != request_revision) {
        return invalid_result_ticket();
    }
    found->second.resident_request_revision = request_revision;
    found->second.in_flight_request_revision.reset();
    found->second.pending_frames = 0;
    ++stats_.total_published_updates;
    refresh_stats();
    return core::Status::ok();
}

core::Status FarTerrainLodUpdateGraph::reject_stale(const FarTerrainPatchKey& key,
                                                    std::uint64_t request_revision) {
    const auto found = nodes_.find(key);
    if (found == nodes_.end() || found->second.in_flight_request_revision != request_revision ||
        found->second.request_revision == request_revision) {
        return invalid_result_ticket();
    }
    found->second.in_flight_request_revision.reset();
    ++stats_.total_stale_results;
    refresh_stats();
    return core::Status::ok();
}

core::Status FarTerrainLodUpdateGraph::retry(const FarTerrainPatchKey& key,
                                             std::uint64_t request_revision) {
    const auto found = nodes_.find(key);
    if (found == nodes_.end() || found->second.in_flight_request_revision != request_revision ||
        found->second.request_revision != request_revision) {
        return invalid_result_ticket();
    }
    found->second.in_flight_request_revision.reset();
    ++stats_.total_retried_updates;
    refresh_stats();
    return core::Status::ok();
}

core::Status FarTerrainLodUpdateGraph::evict_resident(const FarTerrainPatchKey& key) {
    const auto found = nodes_.find(key);
    if (found == nodes_.end() || !found->second.resident_request_revision.has_value()) {
        return core::Status::failure("renderer.invalid_far_terrain_lod_eviction",
                                     "far-terrain LOD eviction must name a desired resident patch");
    }
    found->second.resident_request_revision.reset();
    found->second.pending_frames = 0;
    found->second.request_sequence = next_sequence();
    refresh_stats();
    return core::Status::ok();
}

void FarTerrainLodUpdateGraph::clear() noexcept {
    surface_revision_.reset();
    next_request_sequence_ = 1;
    nodes_.clear();
    stats_ = {};
}

bool FarTerrainLodUpdateGraph::contains(const FarTerrainPatchKey& key) const noexcept {
    return nodes_.contains(key);
}

bool FarTerrainLodUpdateGraph::is_current(const FarTerrainPatchKey& key) const noexcept {
    const auto found = nodes_.find(key);
    return found != nodes_.end() && found->second.resident_request_revision.has_value() &&
           found->second.resident_request_revision == found->second.request_revision;
}

std::optional<std::uint64_t>
FarTerrainLodUpdateGraph::requested_revision(const FarTerrainPatchKey& key) const noexcept {
    const auto found = nodes_.find(key);
    return found == nodes_.end() ? std::nullopt
                                 : std::optional<std::uint64_t>{found->second.request_revision};
}

const FarTerrainLodUpdateStats& FarTerrainLodUpdateGraph::stats() const noexcept {
    return stats_;
}

FarTerrainLodBand FarTerrainLodUpdateGraph::band_for(std::uint32_t level) const noexcept {
    return level < config_.mid_level_count ? FarTerrainLodBand::mid : FarTerrainLodBand::far;
}

std::uint64_t FarTerrainLodUpdateGraph::next_sequence() noexcept {
    const auto result = next_request_sequence_;
    if (next_request_sequence_ != std::numeric_limits<std::uint64_t>::max()) {
        ++next_request_sequence_;
    }
    return result;
}

core::Status FarTerrainLodUpdateGraph::request_update(Node& node, std::uint64_t source_revision,
                                                      bool count_invalidation) {
    if (node.request_revision == std::numeric_limits<std::uint64_t>::max()) {
        return core::Status::failure("renderer.far_terrain_lod_revision_exhausted",
                                     "far-terrain LOD patch request revision range is exhausted");
    }
    if (!node.resident_request_revision.has_value() ||
        node.resident_request_revision != node.request_revision) {
        ++stats_.total_coalesced_invalidations;
    }
    ++node.request_revision;
    node.source_revision = source_revision;
    node.request_sequence = next_sequence();
    node.pending_frames = 0;
    stats_.total_invalidated_patches += count_invalidation ? 1U : 0U;
    return core::Status::ok();
}

void FarTerrainLodUpdateGraph::refresh_stats() noexcept {
    stats_.desired_patches = nodes_.size();
    stats_.current_patches = 0;
    stats_.missing_patches = 0;
    stats_.stale_resident_patches = 0;
    stats_.pending_mid_updates = 0;
    stats_.pending_far_updates = 0;
    stats_.in_flight_updates = 0;
    stats_.maximum_pending_frames = 0;
    for (const auto& [key, node] : nodes_) {
        static_cast<void>(key);
        const auto current = node.resident_request_revision.has_value() &&
                             node.resident_request_revision == node.request_revision;
        stats_.current_patches += current ? 1U : 0U;
        stats_.missing_patches += node.resident_request_revision.has_value() ? 0U : 1U;
        stats_.stale_resident_patches +=
            node.resident_request_revision.has_value() && !current ? 1U : 0U;
        if (!current) {
            if (node.band == FarTerrainLodBand::mid) {
                ++stats_.pending_mid_updates;
            } else {
                ++stats_.pending_far_updates;
            }
            stats_.maximum_pending_frames =
                std::max(stats_.maximum_pending_frames, node.pending_frames);
        }
        stats_.in_flight_updates += node.in_flight_request_revision.has_value() ? 1U : 0U;
    }
}

const char* far_terrain_lod_band_name(FarTerrainLodBand band) noexcept {
    switch (band) {
    case FarTerrainLodBand::mid:
        return "mid";
    case FarTerrainLodBand::far:
        return "far";
    }
    return "unknown";
}

} // namespace heartstead::renderer
