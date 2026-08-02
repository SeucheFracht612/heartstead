#pragma once

#include "engine/core/result.hpp"
#include "engine/renderer/terrain/far_terrain_clipmap.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace heartstead::renderer {

enum class FarTerrainLodBand : std::uint8_t {
    mid,
    far,
};

struct FarTerrainLodUpdateConfig {
    // Clipmap levels below this boundary form the mid field. Remaining levels are deliberately
    // lower-priority far-field derivatives of the authoritative block world.
    std::uint32_t mid_level_count = 1;
    std::uint32_t maximum_mid_rebuilds_per_frame = 2;
    std::uint32_t maximum_far_rebuilds_per_frame = 1;

    [[nodiscard]] core::Status validate(std::uint32_t clipmap_level_count,
                                        std::uint32_t maximum_updates_per_frame) const;
};

struct FarTerrainLodUpdateRequest {
    FarTerrainPatch patch;
    FarTerrainLodBand band = FarTerrainLodBand::far;
    std::uint64_t source_revision = 0;
    std::uint64_t request_revision = 0;
    std::uint64_t request_sequence = 0;
    std::uint64_t pending_frames = 0;
    bool replaces_resident_patch = false;
};

struct FarTerrainLodUpdateStats {
    std::size_t desired_patches = 0;
    std::size_t current_patches = 0;
    std::size_t missing_patches = 0;
    std::size_t stale_resident_patches = 0;
    std::size_t pending_mid_updates = 0;
    std::size_t pending_far_updates = 0;
    std::size_t in_flight_updates = 0;
    std::uint64_t maximum_pending_frames = 0;

    std::uint64_t total_invalidated_patches = 0;
    std::uint64_t total_coalesced_invalidations = 0;
    std::uint64_t total_published_updates = 0;
    std::uint64_t total_stale_results = 0;
    std::uint64_t total_retried_updates = 0;
};

// Owner-thread dependency state for derived clipmap patches. The graph never owns GPU resources
// and workers never mutate it: update requests carry immutable tickets, and only the owner may
// publish, reject, or retry their results.
class FarTerrainLodUpdateGraph {
  public:
    [[nodiscard]] static core::Result<FarTerrainLodUpdateGraph>
    create(FarTerrainLodUpdateConfig config, std::uint32_t clipmap_level_count,
           std::uint32_t maximum_updates_per_frame);

    [[nodiscard]] core::Status
    synchronize(const FarTerrainPlan& plan, std::uint64_t surface_revision,
                std::span<const math::Bounds3d> invalidated_regions = {});

    // Reserves deterministic update tickets. Dirty resident replacements consume their explicit
    // mid/far quotas first; unused total capacity then admits missing patches.
    [[nodiscard]] std::vector<FarTerrainLodUpdateRequest> schedule_updates();

    [[nodiscard]] bool accepts_result(const FarTerrainPatchKey& key,
                                      std::uint64_t request_revision) const noexcept;
    [[nodiscard]] core::Status publish(const FarTerrainPatchKey& key,
                                       std::uint64_t request_revision);
    [[nodiscard]] core::Status reject_stale(const FarTerrainPatchKey& key,
                                            std::uint64_t request_revision);
    [[nodiscard]] core::Status retry(const FarTerrainPatchKey& key, std::uint64_t request_revision);
    [[nodiscard]] core::Status evict_resident(const FarTerrainPatchKey& key);
    void clear() noexcept;

    [[nodiscard]] bool contains(const FarTerrainPatchKey& key) const noexcept;
    [[nodiscard]] bool is_current(const FarTerrainPatchKey& key) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t>
    requested_revision(const FarTerrainPatchKey& key) const noexcept;
    [[nodiscard]] const FarTerrainLodUpdateStats& stats() const noexcept;

  private:
    struct Node {
        FarTerrainPatch patch;
        FarTerrainLodBand band = FarTerrainLodBand::far;
        std::optional<std::uint64_t> resident_request_revision;
        std::uint64_t source_revision = 0;
        std::uint64_t request_revision = 1;
        std::uint64_t request_sequence = 0;
        std::uint64_t pending_frames = 0;
        std::optional<std::uint64_t> in_flight_request_revision;
    };

    FarTerrainLodUpdateGraph(FarTerrainLodUpdateConfig config, std::uint32_t clipmap_level_count,
                             std::uint32_t maximum_updates_per_frame) noexcept;

    [[nodiscard]] FarTerrainLodBand band_for(std::uint32_t level) const noexcept;
    [[nodiscard]] std::uint64_t next_sequence() noexcept;
    [[nodiscard]] core::Status request_update(Node& node, std::uint64_t source_revision,
                                              bool count_invalidation);
    void refresh_stats() noexcept;

    FarTerrainLodUpdateConfig config_{};
    std::uint32_t clipmap_level_count_ = 0;
    std::uint32_t maximum_updates_per_frame_ = 0;
    std::optional<std::uint64_t> surface_revision_;
    std::uint64_t next_request_sequence_ = 1;
    std::map<FarTerrainPatchKey, Node> nodes_;
    FarTerrainLodUpdateStats stats_{};
};

[[nodiscard]] const char* far_terrain_lod_band_name(FarTerrainLodBand band) noexcept;

} // namespace heartstead::renderer
