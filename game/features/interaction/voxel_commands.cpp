#include "game/features/interaction/voxel_commands.hpp"

#include "engine/items/item_prototype.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/world_state.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace heartstead::game::interaction {

namespace {

[[nodiscard]] std::string encode_position(world::BlockCoord position) {
    return std::to_string(position.x) + '|' + std::to_string(position.y) + '|' +
           std::to_string(position.z);
}

[[nodiscard]] core::Result<world::BlockCoord> decode_position(std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('|', start);
        fields.push_back(
            text.substr(start, end == std::string_view::npos ? text.size() - start : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (fields.size() != 3) {
        return core::Result<world::BlockCoord>::failure(
            "voxel_command.invalid_position", "voxel command position must contain x, y, z");
    }
    world::BlockCoord result;
    const auto parse = [](std::string_view field, std::int64_t& output) -> core::Status {
        const auto [end, error] =
            std::from_chars(field.data(), field.data() + field.size(), output);
        return error == std::errc{} && end == field.data() + field.size()
                   ? core::Status::ok()
                   : core::Status::failure("voxel_command.invalid_position",
                                           "voxel command position contains an invalid number");
    };
    auto status = parse(fields[0], result.x);
    if (status) {
        status = parse(fields[1], result.y);
    }
    if (status) {
        status = parse(fields[2], result.z);
    }
    if (!status) {
        return core::Result<world::BlockCoord>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<world::BlockCoord>::success(result);
}

[[nodiscard]] core::Status
validate_voxel_interaction_reach_impl(world::BlockCoord position,
                                      const movement::PlayerControllerState& player) {
    auto center = world::WorldPosition::from_anchor(position, {0.5, 0.5, 0.5});
    if (!center) {
        return core::Status::failure(center.error().code, center.error().message);
    }
    const auto delta =
        center.value().relative_to(player.position.anchor) - player.position.local_offset;
    if (math::length(delta) > maximum_voxel_interaction_reach) {
        return core::Status::failure("voxel_command.out_of_reach",
                                     "voxel target is outside the player's interaction reach");
    }
    return core::Status::ok();
}

struct PreparedResourceGrant {
    world::InventoryRecord* inventory = nullptr;
    items::ItemStack stack;
    std::optional<std::size_t> merge_index;
};

struct PreparedPlacementItem {
    world::InventoryRecord* inventory = nullptr;
    std::size_t stack_index = 0;
    core::PrototypeId item;
};

[[nodiscard]] core::Result<PreparedPlacementItem>
prepare_placement_item(const PlaceVoxelCommand& command, core::SaveId inventory_owner,
                       const net::CommandExecutionContext& context) {
    if (context.voxel_palette == nullptr || context.world_state == nullptr) {
        return core::Result<PreparedPlacementItem>::failure(
            "voxel_command.missing_inventory_context",
            "voxel placement requires authoritative palette and inventory state");
    }
    const auto* voxel = context.voxel_palette->find_by_prototype(command.voxel);
    if (voxel == nullptr || !voxel->interaction.break_resource_item.has_value()) {
        return core::Result<PreparedPlacementItem>::failure(
            "voxel_command.voxel_not_placeable",
            "selected voxel has no corresponding inventory resource item");
    }
    auto* inventory = context.world_state->inventories().find(inventory_owner);
    if (inventory == nullptr) {
        return core::Result<PreparedPlacementItem>::failure(
            "voxel_command.missing_inventory",
            "voxel placement requires the connected player's inventory");
    }
    const auto stack = std::ranges::find(inventory->stacks, *voxel->interaction.break_resource_item,
                                         &items::ItemStack::prototype_id);
    if (stack == inventory->stacks.end() || stack->count == 0) {
        return core::Result<PreparedPlacementItem>::failure(
            "voxel_command.resource_item_missing",
            "player inventory does not contain the item required to place this voxel");
    }
    return core::Result<PreparedPlacementItem>::success(
        {inventory, static_cast<std::size_t>(std::distance(inventory->stacks.begin(), stack)),
         *voxel->interaction.break_resource_item});
}

void commit_placement_item(PreparedPlacementItem item, core::SaveId inventory_owner,
                           world::BlockCoord position, world::WorldOperation& operation) {
    auto& stack = item.inventory->stacks[item.stack_index];
    --stack.count;
    if (stack.count == 0) {
        item.inventory->stacks.erase(
            item.inventory->stacks.begin() +
            static_cast<std::vector<items::ItemStack>::difference_type>(item.stack_index));
    }
    (void)operation.record_mutation("consume voxel placement resource item");
    operation.record_derived_update("Inventory");
    operation.emit_event({std::string(voxel_resource_consumed_event_type), inventory_owner,
                          item.item.value() + '@' + encode_position(position)});
}

[[nodiscard]] core::Result<std::optional<PreparedResourceGrant>>
prepare_resource_grant(world::VoxelCell previous, core::SaveId inventory_owner,
                       const net::CommandExecutionContext& context) {
    if (context.voxel_palette == nullptr) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(
            "voxel_command.missing_palette",
            "voxel removal requires the authoritative voxel palette");
    }
    const auto* voxel = context.voxel_palette->find_by_type(previous.type);
    if (voxel == nullptr) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(
            "voxel_command.unknown_voxel",
            "removed voxel type is not present in the authoritative palette");
    }
    if (!voxel->interaction.break_resource_item.has_value()) {
        return core::Result<std::optional<PreparedResourceGrant>>::success(std::nullopt);
    }
    if (!inventory_owner.is_valid() || context.world_state == nullptr) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(
            "voxel_command.missing_inventory",
            "voxel resource reward requires an authoritative player inventory");
    }
    auto* inventory = context.world_state->inventories().find(inventory_owner);
    if (inventory == nullptr) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(
            "voxel_command.missing_inventory", "voxel resource reward inventory is not loaded");
    }
    if (context.prototypes == nullptr) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(
            "voxel_command.missing_prototypes",
            "voxel resource reward requires the authoritative prototype registry");
    }
    const auto* prototype = context.prototypes->find(*voxel->interaction.break_resource_item);
    if (prototype == nullptr) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(
            "voxel_command.missing_resource_item",
            "voxel resource reward prototype is not loaded: " +
                voxel->interaction.break_resource_item->value());
    }
    auto definition = items::item_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(
            definition.error().code, definition.error().message);
    }
    auto stack = definition.value().create_stack(1);
    if (!stack) {
        return core::Result<std::optional<PreparedResourceGrant>>::failure(stack.error().code,
                                                                           stack.error().message);
    }

    PreparedResourceGrant grant;
    grant.inventory = inventory;
    grant.stack = std::move(stack).value();
    for (std::size_t index = 0; index < inventory->stacks.size(); ++index) {
        const auto& candidate = inventory->stacks[index];
        if (candidate.can_merge_with(grant.stack) && candidate.remaining_capacity() != 0) {
            grant.merge_index = index;
            break;
        }
    }
    return core::Result<std::optional<PreparedResourceGrant>>::success(std::move(grant));
}

void commit_resource_grant(PreparedResourceGrant grant, core::SaveId inventory_owner,
                           world::BlockCoord position, world::WorldOperation& operation) {
    if (grant.merge_index.has_value()) {
        ++grant.inventory->stacks[*grant.merge_index].count;
    } else {
        grant.inventory->stacks.push_back(std::move(grant.stack));
    }
    (void)operation.record_mutation("grant voxel resource item");
    operation.emit_event({std::string(voxel_resource_granted_event_type), inventory_owner,
                          (grant.merge_index.has_value()
                               ? grant.inventory->stacks[*grant.merge_index].prototype_id.value()
                               : grant.inventory->stacks.back().prototype_id.value()) +
                              '@' + encode_position(position)});
}

[[nodiscard]] core::Status commit_voxel(world::BlockCoord position, world::VoxelCell next,
                                        const net::CommandEnvelope& envelope,
                                        const net::CommandExecutionContext& context,
                                        world::WorldOperation& operation) {
    if (context.world_state == nullptr || context.voxel_palette == nullptr) {
        return core::Status::failure("voxel_command.missing_authority",
                                     "voxel command requires authoritative world content");
    }
    const auto address = world::block_to_chunk_local(position);
    auto* chunk = context.world_state->chunks().find(address.chunk);
    if (chunk == nullptr) {
        return core::Status::failure("voxel_command.chunk_not_loaded",
                                     "voxel target chunk is not loaded");
    }
    auto previous = chunk->get(address.local);
    if (!previous) {
        return core::Status::failure(previous.error().code, previous.error().message);
    }
    const auto collision_changed =
        !context.voxel_palette->same_collision_geometry(previous.value(), next);
    const auto lighting_changed =
        !context.voxel_palette->same_lighting_behavior(previous.value(), next);
    const auto fluid_changed =
        !context.voxel_palette->same_fluid_simulation_behavior(previous.value(), next);
    auto status = context.world_state->chunks().set(address.chunk, address.local, next,
                                                    context.world_state->dirty_regions(),
                                                    *context.voxel_palette);
    if (!status) {
        return status;
    }
    chunk = context.world_state->chunks().find(address.chunk);
    auto current = chunk->get(address.local);
    if (!current) {
        return core::Status::failure(current.error().code, current.error().message);
    }
    const world::VoxelChangeRecord change{position, previous.value(), current.value(),
                                          chunk->identity(), chunk->content_revision()};
    status = operation.record_mutation("set terrain voxel");
    if (!status) {
        return status;
    }
    operation.record_derived_update("chunk_mesh");
    if (collision_changed) {
        operation.record_derived_update("chunk_collision");
    }
    if (lighting_changed) {
        operation.record_derived_update("chunk_lighting");
    }
    if (fluid_changed) {
        operation.record_derived_update("voxel_fluids");
    }
    operation.emit_event({std::string(world::voxel_changed_event_type),
                          {},
                          world::VoxelChangeTextCodec::encode(change),
                          address.chunk});
    operation.mark_replication_dirty();
    operation.mark_save_dirty();
    (void)envelope;
    return core::Status::ok();
}

} // namespace

core::Status validate_voxel_interaction_reach(world::BlockCoord position,
                                              const movement::PlayerControllerState& player) {
    return validate_voxel_interaction_reach_impl(position, player);
}

std::string VoxelCommandTextCodec::encode(const PlaceVoxelCommand& command) {
    net::CommandPayload payload;
    (void)payload.set("position", encode_position(command.position));
    (void)payload.set("voxel", command.voxel.value());
    return net::CommandPayloadTextCodec::encode(payload);
}

std::string VoxelCommandTextCodec::encode(const RemoveVoxelCommand& command) {
    net::CommandPayload payload;
    (void)payload.set("position", encode_position(command.position));
    return net::CommandPayloadTextCodec::encode(payload);
}

core::Result<PlaceVoxelCommand> VoxelCommandTextCodec::decode_place(std::string_view payload) {
    auto decoded = net::CommandPayloadTextCodec::decode(payload);
    if (!decoded) {
        return core::Result<PlaceVoxelCommand>::failure(decoded.error().code,
                                                        decoded.error().message);
    }
    auto position_text = decoded.value().require("position");
    auto voxel_text = decoded.value().require("voxel");
    if (!position_text || !voxel_text) {
        const auto& error = !position_text ? position_text.error() : voxel_text.error();
        return core::Result<PlaceVoxelCommand>::failure(error.code, error.message);
    }
    auto position = decode_position(position_text.value());
    auto voxel = core::PrototypeId::parse(voxel_text.value());
    if (!position || !voxel.has_value()) {
        return core::Result<PlaceVoxelCommand>::failure(
            !position ? position.error().code : "voxel_command.invalid_prototype",
            !position ? position.error().message : "voxel prototype id is invalid");
    }
    return core::Result<PlaceVoxelCommand>::success({position.value(), *voxel});
}

core::Result<RemoveVoxelCommand> VoxelCommandTextCodec::decode_remove(std::string_view payload) {
    auto decoded = net::CommandPayloadTextCodec::decode(payload);
    if (!decoded) {
        return core::Result<RemoveVoxelCommand>::failure(decoded.error().code,
                                                         decoded.error().message);
    }
    auto position_text = decoded.value().require("position");
    if (!position_text) {
        return core::Result<RemoveVoxelCommand>::failure(position_text.error().code,
                                                         position_text.error().message);
    }
    auto position = decode_position(position_text.value());
    if (!position) {
        return core::Result<RemoveVoxelCommand>::failure(position.error().code,
                                                         position.error().message);
    }
    return core::Result<RemoveVoxelCommand>::success({position.value()});
}

core::Status execute_place_voxel(const PlaceVoxelCommand& command,
                                 const movement::PlayerControllerState& player,
                                 core::SaveId inventory_owner, const net::CommandEnvelope& envelope,
                                 const net::CommandExecutionContext& context,
                                 world::WorldOperation& operation,
                                 const VoxelPlacementValidator& validate_placement) {
    auto status = validate_voxel_interaction_reach(command.position, player);
    if (!status) {
        return status;
    }
    if (context.voxel_palette == nullptr) {
        return core::Status::failure("voxel_command.missing_palette",
                                     "voxel placement requires the authoritative palette");
    }
    auto cell = context.voxel_palette->cell_for(command.voxel);
    if (!cell) {
        return core::Status::failure(cell.error().code, cell.error().message);
    }
    const auto address = world::block_to_chunk_local(command.position);
    if (context.world_state == nullptr) {
        return core::Status::failure("voxel_command.missing_world",
                                     "voxel placement requires an authoritative world");
    }
    if (context.world_state->chunks().find(address.chunk) == nullptr) {
        return core::Status::failure("voxel_command.chunk_not_loaded",
                                     "voxel target chunk is not loaded");
    }
    auto previous = context.world_state->chunks().get(address.chunk, address.local);
    if (!previous) {
        return core::Status::failure(previous.error().code, previous.error().message);
    }
    if (!previous.value().is_air()) {
        return core::Status::failure("voxel_command.target_occupied",
                                     "voxel placement target is already occupied");
    }
    if (validate_placement) {
        status = validate_placement(command.position, cell.value());
        if (!status) {
            return status;
        }
    }
    auto item = prepare_placement_item(command, inventory_owner, context);
    if (!item) {
        return core::Status::failure(item.error().code, item.error().message);
    }
    status = commit_voxel(command.position, cell.value(), envelope, context, operation);
    if (!status) {
        return status;
    }
    commit_placement_item(std::move(item).value(), inventory_owner, command.position, operation);
    return core::Status::ok();
}

core::Status execute_remove_voxel(const RemoveVoxelCommand& command,
                                  const movement::PlayerControllerState& player,
                                  core::SaveId inventory_owner,
                                  const net::CommandEnvelope& envelope,
                                  const net::CommandExecutionContext& context,
                                  world::WorldOperation& operation) {
    auto status = validate_voxel_interaction_reach(command.position, player);
    if (!status) {
        return status;
    }
    if (context.world_state == nullptr) {
        return core::Status::failure("voxel_command.missing_world",
                                     "voxel removal requires an authoritative world");
    }
    const auto address = world::block_to_chunk_local(command.position);
    if (context.world_state->chunks().find(address.chunk) == nullptr) {
        return core::Status::failure("voxel_command.chunk_not_loaded",
                                     "voxel target chunk is not loaded");
    }
    auto previous = context.world_state->chunks().get(address.chunk, address.local);
    if (!previous) {
        return core::Status::failure(previous.error().code, previous.error().message);
    }
    if (previous.value().is_air()) {
        return core::Status::failure("voxel_command.target_empty",
                                     "voxel removal target is already empty");
    }
    auto resource_grant = prepare_resource_grant(previous.value(), inventory_owner, context);
    if (!resource_grant) {
        return core::Status::failure(resource_grant.error().code, resource_grant.error().message);
    }
    status = commit_voxel(command.position, world::VoxelCell::air(), envelope, context, operation);
    if (!status) {
        return status;
    }
    if (resource_grant.value().has_value()) {
        commit_resource_grant(std::move(*resource_grant.value()), inventory_owner, command.position,
                              operation);
    }
    return core::Status::ok();
}

} // namespace heartstead::game::interaction
