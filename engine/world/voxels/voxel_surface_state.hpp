#pragma once

#include "engine/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace heartstead::world {

// Solid voxels use the low nine state bits as composable surface-layer flags. Fluid voxels retain
// the independent encoding from fluid_state.hpp and must never be decoded through this contract.
enum class VoxelSurfaceState : std::uint8_t {
    wetness = 0,
    snow,
    frost,
    mud,
    moss,
    soot,
    heat,
    corruption,
    magical_residue,
};

inline constexpr std::size_t voxel_surface_state_count = 9;
inline constexpr std::uint16_t voxel_surface_state_mask = (1U << voxel_surface_state_count) - 1U;
inline constexpr std::uint16_t voxel_surface_coverage_shift = 9U;
inline constexpr std::uint16_t voxel_surface_coverage_mask =
    static_cast<std::uint16_t>(0x7U << voxel_surface_coverage_shift);
inline constexpr std::uint16_t voxel_surface_reserved_mask = 0xF000U;

struct VoxelSurfaceStateSet {
    std::uint16_t layers = 0;
    // One shared coverage value keeps the persisted cell ABI compact. Individual material
    // susceptibilities and stable per-layer noise produce distinct visual coverage.
    std::uint8_t coverage = 7;

    [[nodiscard]] constexpr bool contains(VoxelSurfaceState state) const noexcept {
        return (layers & (1U << static_cast<std::uint8_t>(state))) != 0;
    }

    friend bool operator==(const VoxelSurfaceStateSet&, const VoxelSurfaceStateSet&) = default;
};

[[nodiscard]] constexpr std::string_view
voxel_surface_state_name(VoxelSurfaceState state) noexcept {
    switch (state) {
    case VoxelSurfaceState::wetness:
        return "wetness";
    case VoxelSurfaceState::snow:
        return "snow";
    case VoxelSurfaceState::frost:
        return "frost";
    case VoxelSurfaceState::mud:
        return "mud";
    case VoxelSurfaceState::moss:
        return "moss";
    case VoxelSurfaceState::soot:
        return "soot";
    case VoxelSurfaceState::heat:
        return "heat";
    case VoxelSurfaceState::corruption:
        return "corruption";
    case VoxelSurfaceState::magical_residue:
        return "magical_residue";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::array<VoxelSurfaceState, voxel_surface_state_count>
voxel_surface_states() noexcept {
    return {
        VoxelSurfaceState::wetness, VoxelSurfaceState::snow,
        VoxelSurfaceState::frost, VoxelSurfaceState::mud,
        VoxelSurfaceState::moss, VoxelSurfaceState::soot,
        VoxelSurfaceState::heat, VoxelSurfaceState::corruption,
        VoxelSurfaceState::magical_residue,
    };
}

[[nodiscard]] constexpr std::uint16_t
encode_voxel_surface_states(VoxelSurfaceStateSet state) noexcept {
    const auto layers = static_cast<std::uint16_t>(state.layers & voxel_surface_state_mask);
    const auto coverage = static_cast<std::uint16_t>(state.coverage > 7U ? 7U : state.coverage);
    return static_cast<std::uint16_t>(
        layers | static_cast<std::uint16_t>(coverage << voxel_surface_coverage_shift));
}

[[nodiscard]] inline core::Result<VoxelSurfaceStateSet>
decode_voxel_surface_states(std::uint16_t state_bits) {
    if ((state_bits & voxel_surface_reserved_mask) != 0U) {
        return core::Result<VoxelSurfaceStateSet>::failure(
            "voxel_surface_state.reserved_bits",
            "solid voxel surface state uses reserved state bits");
    }
    return core::Result<VoxelSurfaceStateSet>::success(
        {static_cast<std::uint16_t>(state_bits & voxel_surface_state_mask),
         static_cast<std::uint8_t>((state_bits & voxel_surface_coverage_mask) >>
                                   voxel_surface_coverage_shift)});
}

} // namespace heartstead::world
