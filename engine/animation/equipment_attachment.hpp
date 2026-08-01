#pragma once

#include "engine/assets/model_asset.hpp"
#include "engine/core/result.hpp"
#include "engine/entities/entity_visual.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace heartstead::animation {

struct ResolvedEquipmentAttachment {
    std::uint32_t primary_socket_node{assets::no_model_index};
    std::uint32_t secondary_socket_node{assets::no_model_index};
    std::vector<std::uint32_t> hidden_body_nodes;
    std::vector<std::uint32_t> equipment_to_character_nodes;
    bool skinned{false};
    bool two_handed{false};
};

[[nodiscard]] core::Result<ResolvedEquipmentAttachment> resolve_equipment_attachment(
    const entities::EntityVisualDefinition& character_visual,
    const assets::ModelAsset& character_model,
    const entities::VisualEquipmentVariantDefinition& equipment,
    const assets::ModelAsset& equipment_model,
    bool stowed);

[[nodiscard]] core::Result<math::Mat4f> equipment_socket_matrix(
    std::span<const math::Mat4f> character_node_matrices,
    const ResolvedEquipmentAttachment& attachment);

} // namespace heartstead::animation
