#include "engine/world/fluids/fluid_state.hpp"

#include "engine/world/voxels/voxel_palette.hpp"

namespace heartstead::world {

core::Status validate_fluid_state(const FluidState& state) {
    if (state.amount == 0 || state.amount > maximum_fluid_amount) {
        return core::Status::failure(
            "fluid_state.invalid_amount",
            "fluid amount must be an integer between one and eight");
    }
    if (state.source && state.amount != maximum_fluid_amount) {
        return core::Status::failure("fluid_state.partial_source",
                                     "a fluid source must contain the maximum amount");
    }
    if (static_cast<std::uint8_t>(state.flow) >
        static_cast<std::uint8_t>(FluidFlowDirection::positive_z)) {
        return core::Status::failure("fluid_state.invalid_flow",
                                     "fluid flow direction is outside the encoded range");
    }
    return core::Status::ok();
}

core::Result<std::uint16_t> encode_fluid_state(const FluidState& state) {
    auto status = validate_fluid_state(state);
    if (!status) {
        return core::Result<std::uint16_t>::failure(status.error().code, status.error().message);
    }
    auto bits = static_cast<std::uint16_t>(state.amount);
    if (state.falling) {
        bits |= fluid_falling_bit;
    }
    if (state.source) {
        bits |= fluid_source_bit;
    }
    bits |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(state.flow))
            << fluid_flow_shift;
    return core::Result<std::uint16_t>::success(bits);
}

core::Result<FluidState> decode_fluid_state(std::uint16_t state_bits) {
    if ((state_bits & static_cast<std::uint16_t>(~fluid_state_mask)) != 0) {
        return core::Result<FluidState>::failure(
            "fluid_state.reserved_bits",
            "fluid cell uses state bits reserved for future fluid formats");
    }
    FluidState state;
    state.amount = static_cast<std::uint8_t>(state_bits & fluid_amount_mask);
    state.falling = (state_bits & fluid_falling_bit) != 0;
    state.source = (state_bits & fluid_source_bit) != 0;
    state.flow = static_cast<FluidFlowDirection>(
        static_cast<std::uint8_t>((state_bits & fluid_flow_mask) >> fluid_flow_shift));
    auto status = validate_fluid_state(state);
    if (!status) {
        return core::Result<FluidState>::failure(status.error().code, status.error().message);
    }
    return core::Result<FluidState>::success(state);
}

core::Result<FluidState> decode_fluid_cell(const VoxelCell& cell,
                                           const VoxelPalette& palette) {
    const auto* definition = palette.find_by_type(cell.type);
    if (definition == nullptr) {
        return core::Result<FluidState>::failure(
            "fluid_state.unknown_voxel",
            "fluid cell references a voxel type absent from the palette");
    }
    if (definition->logical_occupancy != BlockLogicalOccupancy::fluid) {
        return core::Result<FluidState>::failure(
            "fluid_state.non_fluid_voxel",
            "fluid state can only be decoded for a fluid voxel prototype");
    }
    if (cell.metadata_handle != 0) {
        return core::Result<FluidState>::failure(
            "fluid_state.metadata_not_supported",
            "fluid cells cannot carry unrelated rich-block metadata");
    }
    return decode_fluid_state(cell.state_bits);
}

core::Result<VoxelCell> make_fluid_cell(std::uint16_t type, const FluidState& state,
                                        std::uint8_t light) {
    if (type == VoxelDefinition::air_type) {
        return core::Result<VoxelCell>::failure(
            "fluid_state.air_type",
            "a fluid cell must reference a non-air voxel type");
    }
    auto bits = encode_fluid_state(state);
    if (!bits) {
        return core::Result<VoxelCell>::failure(bits.error().code, bits.error().message);
    }
    return core::Result<VoxelCell>::success(VoxelCell{type, light, bits.value(), 0});
}

} // namespace heartstead::world
