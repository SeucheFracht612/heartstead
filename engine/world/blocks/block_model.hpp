#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/modding/prototype_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::world {

enum class BlockModelKind {
    cube,
    parametric,
    cross_plane,
    custom_voxel,
    mesh,
    connected_texture,
    ore_protrusion,
    random_variant,
    state_dependent,
};

struct BlockModelBox {
    math::Bounds3f bounds{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};

    [[nodiscard]] core::Status validate() const;
};

struct BlockModelTriangle {
    std::array<math::Vec3f, 3> positions{};

    [[nodiscard]] core::Status validate() const;
};

struct BlockModelDefinition {
    static constexpr std::uint16_t max_dependency_radius = 8;

    core::PrototypeId prototype_id;
    BlockModelKind kind = BlockModelKind::cube;
    std::vector<BlockModelBox> boxes;
    std::vector<BlockModelTriangle> triangles;
    math::Bounds3f render_bounds{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}};
    std::string material_asset;
    std::string mesh_asset;
    std::uint16_t neighbor_dependency_radius = 1;
    std::uint16_t mesh_invalidation_radius = 1;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] bool uses_chunk_geometry() const noexcept;
    [[nodiscard]] bool is_unit_cube() const noexcept;
};

class BlockModelDatabase {
  public:
    [[nodiscard]] core::Status add(BlockModelDefinition definition);
    [[nodiscard]] const BlockModelDefinition*
    find(const core::PrototypeId& prototype_id) const noexcept;
    [[nodiscard]] std::vector<const BlockModelDefinition*> definitions() const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

  private:
    std::vector<BlockModelDefinition> definitions_;
    std::unordered_map<std::string, std::size_t> by_prototype_;
};

[[nodiscard]] const BlockModelDefinition& legacy_cube_block_model() noexcept;
[[nodiscard]] std::string_view block_model_kind_name(BlockModelKind kind) noexcept;
[[nodiscard]] std::optional<BlockModelKind> parse_block_model_kind(std::string_view value) noexcept;
[[nodiscard]] core::Result<BlockModelDefinition>
block_model_definition_from_prototype(const modding::GenericPrototype& prototype);
[[nodiscard]] core::Result<BlockModelDatabase>
block_model_database_from_prototypes(const modding::PrototypeRegistry& prototypes);

} // namespace heartstead::world
