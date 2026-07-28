#include "engine/movement/player_controller_store.hpp"

#include <algorithm>

namespace heartstead::movement {

core::Status PlayerControllerRecord::validate(const PlayerMovementConfig& config) const {
    if (!runtime_handle.is_valid() || !net_id.is_valid() || (persistent && !save_id.is_valid())) {
        return core::Status::failure("player_controller_store.invalid_identity",
                                     "player controller record identity is invalid");
    }
    if (modifiers.stamina_regen_per_mille > 10'000) {
        return core::Status::failure("player_controller_store.invalid_modifiers",
                                     "player controller modifiers are invalid");
    }
    return state.validate(config);
}

core::Status PlayerControllerStore::insert(PlayerControllerRecord record,
                                           const PlayerMovementConfig& config) {
    auto status = record.validate(config);
    if (!status) {
        return status;
    }
    if (records_.contains(record.runtime_handle.value()) ||
        runtime_by_net_.contains(record.net_id.value()) ||
        (record.save_id.is_valid() && runtime_by_save_.contains(record.save_id.value()))) {
        return core::Status::failure("player_controller_store.duplicate_identity",
                                     "player controller identity is already registered");
    }
    const auto runtime = record.runtime_handle.value();
    runtime_by_net_.emplace(record.net_id.value(), runtime);
    if (record.save_id.is_valid()) {
        runtime_by_save_.emplace(record.save_id.value(), runtime);
    }
    records_.emplace(runtime, std::move(record));
    return core::Status::ok();
}

PlayerControllerRecord* PlayerControllerStore::find(core::RuntimeHandle handle) noexcept {
    const auto found = records_.find(handle.value());
    return found == records_.end() ? nullptr : &found->second;
}

const PlayerControllerRecord*
PlayerControllerStore::find(core::RuntimeHandle handle) const noexcept {
    const auto found = records_.find(handle.value());
    return found == records_.end() ? nullptr : &found->second;
}

PlayerControllerRecord* PlayerControllerStore::find_by_net_id(core::NetId net_id) noexcept {
    const auto found = runtime_by_net_.find(net_id.value());
    return found == runtime_by_net_.end() ? nullptr
                                          : find(core::RuntimeHandle::from_value(found->second));
}

const PlayerControllerRecord*
PlayerControllerStore::find_by_save_id(core::SaveId save_id) const noexcept {
    const auto found = runtime_by_save_.find(save_id.value());
    return found == runtime_by_save_.end() ? nullptr
                                           : find(core::RuntimeHandle::from_value(found->second));
}

bool PlayerControllerStore::erase(core::RuntimeHandle handle) noexcept {
    const auto found = records_.find(handle.value());
    if (found == records_.end()) {
        return false;
    }
    runtime_by_net_.erase(found->second.net_id.value());
    if (found->second.save_id.is_valid()) {
        runtime_by_save_.erase(found->second.save_id.value());
    }
    records_.erase(found);
    return true;
}

void PlayerControllerStore::clear() noexcept {
    records_.clear();
    runtime_by_net_.clear();
    runtime_by_save_.clear();
}

std::vector<const PlayerControllerRecord*> PlayerControllerStore::records() const {
    std::vector<const PlayerControllerRecord*> result;
    result.reserve(records_.size());
    for (const auto& [_, record] : records_) {
        result.push_back(&record);
    }
    std::ranges::sort(result, [](const auto* lhs, const auto* rhs) {
        return lhs->runtime_handle.value() < rhs->runtime_handle.value();
    });
    return result;
}

std::size_t PlayerControllerStore::size() const noexcept {
    return records_.size();
}

core::Result<PlayerControllerState>
normalize_player_controller_after_load(const PlayerControllerState& saved,
                                       const PlayerController& controller,
                                       ICharacterCollisionWorld& collision) {
    auto status = saved.validate(controller.config());
    if (!status) {
        return core::Result<PlayerControllerState>::failure(status.error().code,
                                                            status.error().message);
    }
    auto result = saved;
    result.scripted_kind = ScriptedMovementKind::none;
    result.mode_ticks = 0;
    result.mode_duration_ticks = 0;
    result.invulnerable = false;
    result.pending_fall_distance.reset();
    result.landing_roll_window_ticks = 0;
    result.velocity = {};
    result.crouched = saved.crouched || saved.mode == PlayerControllerMode::rolling;
    auto support = collision.has_support(result.position, controller.shape_for(result));
    if (!support) {
        return core::Result<PlayerControllerState>::failure(support.error().code,
                                                            support.error().message);
    }
    result.grounded = support.value();
    result.mode = result.grounded ? PlayerControllerMode::grounded : PlayerControllerMode::airborne;
    result.scripted_start = result.position;
    result.scripted_target = result.position;
    result.fall_origin = result.position;
    return core::Result<PlayerControllerState>::success(std::move(result));
}

} // namespace heartstead::movement
