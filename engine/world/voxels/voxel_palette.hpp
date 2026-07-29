#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/world/blocks/block_model.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::world {

enum class BlockLogicalOccupancy {
    full,
    partial,
    decorative,
    fluid,
};

enum class BlockOcclusionBehavior {
    none,
    model,
    full_cube,
};

struct VoxelInteractionDefinition {
    std::optional<core::PrototypeId> break_particle;
    std::optional<core::PrototypeId> break_sound;
    std::optional<core::PrototypeId> place_sound;

    [[nodiscard]] core::Status validate() const;
};

struct VoxelDefinition {
    static constexpr std::uint16_t air_type = 0;

    std::uint16_t type = air_type;
    core::PrototypeId prototype_id;
    std::string display_name;
    std::string terrain_material;
    std::string mining_tool;
    std::vector<std::string> tags;
    std::optional<core::PrototypeId> block_model_id;
    BlockLogicalOccupancy logical_occupancy = BlockLogicalOccupancy::full;
    std::vector<math::Bounds3f> collision_bounds{{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}};
    std::vector<math::Bounds3f> selection_bounds{{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}};
    std::vector<math::Bounds3f> occlusion_bounds{{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}};
    BlockOcclusionBehavior occlusion = BlockOcclusionBehavior::full_cube;
    std::uint8_t light_emission = 0;
    std::uint8_t light_absorption = 255;
    VoxelInteractionDefinition interaction;
    bool metadata_required = false;
    bool missing_prototype = false;

    [[nodiscard]] core::Status validate() const;
};

struct VoxelPaletteManifestEntry {
    std::uint16_t type = VoxelDefinition::air_type;
    core::PrototypeId prototype_id;

    friend auto operator<=>(const VoxelPaletteManifestEntry&,
                            const VoxelPaletteManifestEntry&) = default;
};

struct VoxelPaletteManifest {
    std::vector<VoxelPaletteManifestEntry> entries;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] const VoxelPaletteManifestEntry*
    find_by_type(std::uint16_t type) const noexcept;
    [[nodiscard]] const VoxelPaletteManifestEntry*
    find_by_prototype(const core::PrototypeId& prototype_id) const noexcept;
};

class VoxelPalette {
  public:
    static constexpr std::uint16_t air_type = VoxelDefinition::air_type;
    static constexpr std::uint16_t first_content_type = 1;

    [[nodiscard]] core::Status add(VoxelDefinition definition);
    [[nodiscard]] core::Status add_block_model(BlockModelDefinition definition);
    [[nodiscard]] const VoxelDefinition* find_by_type(std::uint16_t type) const noexcept;
    [[nodiscard]] const VoxelDefinition*
    find_by_prototype(const core::PrototypeId& prototype_id) const noexcept;
    [[nodiscard]] std::optional<std::uint16_t>
    type_for(const core::PrototypeId& prototype_id) const noexcept;
    [[nodiscard]] core::Result<VoxelCell> cell_for(const core::PrototypeId& prototype_id,
                                                   std::uint8_t light = 0) const;
    [[nodiscard]] std::vector<const VoxelDefinition*> definitions() const;
    [[nodiscard]] const BlockModelDatabase& block_models() const noexcept;
    [[nodiscard]] const BlockModelDefinition& model_for(const VoxelDefinition& definition) const;
    [[nodiscard]] const BlockModelDefinition* model_for_type(std::uint16_t type) const noexcept;
    [[nodiscard]] std::uint16_t mesh_invalidation_radius(VoxelCell cell) const noexcept;
    [[nodiscard]] std::uint16_t neighbor_dependency_radius(VoxelCell cell) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::uint64_t render_revision() const noexcept;
    [[nodiscard]] VoxelPaletteManifest manifest() const;

  private:
    std::vector<VoxelDefinition> definitions_;
    BlockModelDatabase block_models_;
    std::unordered_map<std::uint16_t, std::size_t> by_type_;
    std::unordered_map<std::string, std::size_t> by_prototype_;
    std::uint64_t render_revision_ = 1;
};

[[nodiscard]] core::Result<VoxelDefinition>
voxel_definition_from_prototype(const modding::GenericPrototype& prototype, std::uint16_t type);
[[nodiscard]] core::Result<VoxelPalette>
voxel_palette_from_prototypes(const modding::PrototypeRegistry& prototypes);
[[nodiscard]] core::Result<VoxelPalette>
voxel_palette_from_prototypes(const modding::PrototypeRegistry& prototypes,
                              const VoxelPaletteManifest& persisted_manifest,
                              bool append_new_prototypes = true);

} // namespace heartstead::world
