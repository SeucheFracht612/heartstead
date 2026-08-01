#include "engine/animation/equipment_attachment.hpp"

#include <array>
#include <cassert>

int main() {
    using namespace heartstead;

    assets::ModelAsset character;
    character.nodes.resize(5);
    character.nodes[0].name = "Root";
    character.nodes[1].name = "HandR";
    character.nodes[2].name = "HandL";
    character.nodes[3].name = "Back";
    character.nodes[4].name = "TorsoMesh";
    assets::ModelSocket hand_socket;
    hand_socket.name = "socket_hand_r";
    hand_socket.node = 1;
    character.sockets.push_back(hand_socket);

    entities::EntityVisualDefinition visual;
    visual.socket_aliases.emplace("main_hand", "socket_hand_r");
    visual.socket_aliases.emplace("off_hand", "HandL");
    visual.socket_aliases.emplace("back", "Back");
    entities::VisualVisibilityGroup torso;
    torso.name = "torso";
    torso.nodes.push_back("TorsoMesh");
    visual.visibility_groups.push_back(torso);

    assets::ModelAsset equipment_model;
    equipment_model.nodes.resize(2);
    equipment_model.nodes[0].name = "Root";
    equipment_model.nodes[1].name = "HandR";

    entities::VisualEquipmentVariantDefinition hammer;
    hammer.slot = "main_hand";
    hammer.variant = "hammer";
    hammer.model_asset = "base:hammer";
    hammer.socket = "main_hand";
    hammer.stowed_socket = "back";
    hammer.secondary_socket = "off_hand";
    hammer.hidden_body_groups.push_back("torso");
    hammer.skinned = true;
    hammer.two_handed = true;

    auto held = animation::resolve_equipment_attachment(
        visual, character, hammer, equipment_model, false);
    assert(held);
    assert(held.value().primary_socket_node == 1);
    assert(held.value().secondary_socket_node == 2);
    assert(held.value().hidden_body_nodes == std::vector<std::uint32_t>{4});
    assert(held.value().equipment_to_character_nodes ==
           std::vector<std::uint32_t>({0, 1}));
    assert(held.value().two_handed && held.value().skinned);

    auto stowed = animation::resolve_equipment_attachment(
        visual, character, hammer, equipment_model, true);
    assert(stowed && stowed.value().primary_socket_node == 3);

    std::array<math::Mat4f, 5> matrices{};
    matrices[3] = math::Mat4f::identity();
    auto socket_matrix = animation::equipment_socket_matrix(matrices, stowed.value());
    assert(socket_matrix && socket_matrix.value() == matrices[3]);

    equipment_model.nodes[1].name = "MissingJoint";
    assert(!animation::resolve_equipment_attachment(
        visual, character, hammer, equipment_model, false));
}
