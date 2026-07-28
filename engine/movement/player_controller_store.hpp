#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/movement/player_controller.hpp"

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace heartstead::movement {

struct PlayerControllerRecord {
    core::RuntimeHandle runtime_handle;
    core::NetId net_id;
    core::SaveId save_id;
    PlayerControllerState state;
    PlayerMovementModifiers modifiers;
    bool persistent = true;

    [[nodiscard]] core::Status validate(const PlayerMovementConfig& config = {}) const;
};

class PlayerControllerStore {
  public:
    [[nodiscard]] core::Status insert(PlayerControllerRecord record,
                                      const PlayerMovementConfig& config = {});
    [[nodiscard]] PlayerControllerRecord* find(core::RuntimeHandle handle) noexcept;
    [[nodiscard]] const PlayerControllerRecord* find(core::RuntimeHandle handle) const noexcept;
    [[nodiscard]] PlayerControllerRecord* find_by_net_id(core::NetId net_id) noexcept;
    [[nodiscard]] const PlayerControllerRecord*
    find_by_save_id(core::SaveId save_id) const noexcept;
    bool erase(core::RuntimeHandle handle) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::vector<const PlayerControllerRecord*> records() const;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::unordered_map<std::uint64_t, PlayerControllerRecord> records_;
    std::unordered_map<std::uint64_t, std::uint64_t> runtime_by_net_;
    std::unordered_map<std::uint64_t, std::uint64_t> runtime_by_save_;
};

[[nodiscard]] core::Result<PlayerControllerState>
normalize_player_controller_after_load(const PlayerControllerState& saved,
                                       const PlayerController& controller,
                                       ICharacterCollisionWorld& collision);

} // namespace heartstead::movement
