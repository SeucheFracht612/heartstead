#include "engine/animation/equipment_attachment.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace heartstead::animation {
namespace {

[[nodiscard]] std::string_view resolve_socket_name(
    const entities::EntityVisualDefinition& visual,
    const std::string_view name) {
    const auto alias = visual.socket_aliases.find(std::string(name));
    return alias == visual.socket_aliases.end() ? name : std::string_view(alias->second);
}

[[nodiscard]] std::uint32_t find_socket_node(const assets::ModelAsset& model,
                                             const std::string_view name) {
    const auto socket = std::ranges::find(model.sockets, name, &assets::ModelSocket::name);
    if (socket != model.sockets.end()) {
        return socket->node;
    }
    const auto node = std::ranges::find(model.nodes, name, &assets::ModelNode::name);
    return node == model.nodes.end()
               ? assets::no_model_index
               : static_cast<std::uint32_t>(std::distance(model.nodes.begin(), node));
}

} // namespace

core::Result<ResolvedEquipmentAttachment> resolve_equipment_attachment(
    const entities::EntityVisualDefinition& character_visual,
    const assets::ModelAsset& character_model,
    const entities::VisualEquipmentVariantDefinition& equipment,
    const assets::ModelAsset& equipment_model,
    const bool stowed) {
    const auto requested_socket =
        stowed && !equipment.stowed_socket.empty() ? equipment.stowed_socket : equipment.socket;
    ResolvedEquipmentAttachment result;
    result.primary_socket_node =
        find_socket_node(character_model, resolve_socket_name(character_visual, requested_socket));
    if (result.primary_socket_node == assets::no_model_index) {
        return core::Result<ResolvedEquipmentAttachment>::failure(
            "equipment_attachment.missing_primary_socket",
            "equipment attachment primary socket is not present in the character model");
    }

    result.skinned = equipment.skinned;
    result.two_handed = equipment.two_handed;
    if (!equipment.secondary_socket.empty()) {
        result.secondary_socket_node = find_socket_node(
            character_model, resolve_socket_name(character_visual, equipment.secondary_socket));
        if (result.secondary_socket_node == assets::no_model_index) {
            return core::Result<ResolvedEquipmentAttachment>::failure(
                "equipment_attachment.missing_secondary_socket",
                "equipment attachment secondary socket is not present in the character model");
        }
    }

    for (const auto& hidden_group : equipment.hidden_body_groups) {
        const auto group = std::ranges::find(character_visual.visibility_groups, hidden_group,
                                             &entities::VisualVisibilityGroup::name);
        if (group == character_visual.visibility_groups.end()) {
            return core::Result<ResolvedEquipmentAttachment>::failure(
                "equipment_attachment.missing_body_group",
                "equipment attachment references an unknown character visibility group");
        }
        for (const auto& node_name : group->nodes) {
            const auto node = find_socket_node(character_model, node_name);
            if (node == assets::no_model_index) {
                return core::Result<ResolvedEquipmentAttachment>::failure(
                    "equipment_attachment.missing_body_node",
                    "equipment attachment body group references a missing character node");
            }
            result.hidden_body_nodes.push_back(node);
        }
    }
    std::ranges::sort(result.hidden_body_nodes);
    result.hidden_body_nodes.erase(
        std::unique(result.hidden_body_nodes.begin(), result.hidden_body_nodes.end()),
        result.hidden_body_nodes.end());

    if (equipment.skinned) {
        result.equipment_to_character_nodes.reserve(equipment_model.nodes.size());
        for (const auto& equipment_node : equipment_model.nodes) {
            const auto character_node = std::ranges::find(
                character_model.nodes, equipment_node.name, &assets::ModelNode::name);
            if (character_node == character_model.nodes.end()) {
                return core::Result<ResolvedEquipmentAttachment>::failure(
                    "equipment_attachment.incompatible_skeleton",
                    "skinned equipment node is not present in the character skeleton");
            }
            result.equipment_to_character_nodes.push_back(static_cast<std::uint32_t>(
                std::distance(character_model.nodes.begin(), character_node)));
        }
    }
    return core::Result<ResolvedEquipmentAttachment>::success(std::move(result));
}

core::Result<math::Mat4f> equipment_socket_matrix(
    const std::span<const math::Mat4f> character_node_matrices,
    const ResolvedEquipmentAttachment& attachment) {
    if (attachment.primary_socket_node >= character_node_matrices.size()) {
        return core::Result<math::Mat4f>::failure(
            "equipment_attachment.invalid_palette",
            "character pose does not contain the equipment socket node");
    }
    return core::Result<math::Mat4f>::success(
        character_node_matrices[attachment.primary_socket_node]);
}

} // namespace heartstead::animation
