#pragma once

#include "engine/core/result.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstdint>

namespace heartstead::world {

class VoxelPalette;

enum class FluidFlowDirection : std::uint8_t {
    none = 0,
    negative_x = 1,
    positive_x = 2,
    negative_z = 3,
    positive_z = 4,
};

inline constexpr std::uint8_t maximum_fluid_amount = 8;
inline constexpr std::uint16_t fluid_amount_mask = 0x000FU;
inline constexpr std::uint16_t fluid_falling_bit = 0x0010U;
inline constexpr std::uint16_t fluid_source_bit = 0x0020U;
inline constexpr std::uint16_t fluid_flow_mask = 0x01C0U;
inline constexpr std::uint8_t fluid_flow_shift = 6;
inline constexpr std::uint16_t fluid_state_mask =
    fluid_amount_mask | fluid_falling_bit | fluid_source_bit | fluid_flow_mask;

struct FluidState {
    std::uint8_t amount = maximum_fluid_amount;
    bool falling = false;
    bool source = false;
    FluidFlowDirection flow = FluidFlowDirection::none;

    friend auto operator<=>(const FluidState&, const FluidState&) = default;
};

[[nodiscard]] core::Status validate_fluid_state(const FluidState& state);
[[nodiscard]] core::Result<std::uint16_t> encode_fluid_state(const FluidState& state);
[[nodiscard]] core::Result<FluidState> decode_fluid_state(std::uint16_t state_bits);
[[nodiscard]] core::Result<FluidState> decode_fluid_cell(const VoxelCell& cell,
                                                         const VoxelPalette& palette);
[[nodiscard]] core::Result<VoxelCell> make_fluid_cell(std::uint16_t type, const FluidState& state,
                                                      std::uint8_t light = 0);
[[nodiscard]] constexpr std::uint16_t full_fluid_state_bits() noexcept {
    return maximum_fluid_amount;
}
[[nodiscard]] constexpr std::uint16_t full_fluid_source_state_bits() noexcept {
    return maximum_fluid_amount | fluid_source_bit;
}
[[nodiscard]] constexpr float fluid_surface_height(std::uint8_t amount) noexcept {
    return static_cast<float>(amount) / static_cast<float>(maximum_fluid_amount);
}

} // namespace heartstead::world
