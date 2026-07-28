#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/movement/player_controller.hpp"
#include "engine/net/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::movement {

inline constexpr std::uint16_t player_controller_snapshot_version = 3;
inline constexpr std::string_view movement_input_payload_type = "movement.input.v1";
inline constexpr std::string_view legacy_movement_snapshot_payload_type =
    "movement.snapshot.v3";
inline constexpr std::string_view movement_snapshot_payload_type = "movement.snapshot.bin.v1";
inline constexpr std::string_view legacy_movement_input_bundle_payload_type =
    "movement.input_bundle.v1";
inline constexpr std::string_view movement_input_bundle_payload_type = "movement.input_bundle.v2";
inline constexpr std::string_view player_assignment_payload_type = "movement.player_assignment.v1";
inline constexpr std::string_view player_removal_payload_type = "movement.player_removal.v1";

struct PlayerInputBundle {
    std::vector<PlayerInputFrame> frames;

    [[nodiscard]] core::Status validate() const;
};

class PlayerInputBundleTextCodec {
  public:
    [[nodiscard]] static std::string encode(const PlayerInputBundle& bundle);
    [[nodiscard]] static core::Result<PlayerInputBundle> decode(std::string_view payload);
};

class PlayerInputBundleBinaryCodec {
  public:
    [[nodiscard]] static std::string encode(const PlayerInputBundle& bundle);
    [[nodiscard]] static core::Result<PlayerInputBundle> decode(std::string_view payload);
};

struct PlayerControllerSnapshot {
    std::uint16_t version = player_controller_snapshot_version;
    core::NetId player_net_id;
    core::SaveId player_save_id;
    PlayerControllerState state;
    std::uint64_t last_processed_input_sequence = 0;
    std::uint64_t collision_world_revision = 0;

    [[nodiscard]] core::Status validate(const PlayerMovementConfig& config) const;
};

class PlayerControllerSnapshotTextCodec {
  public:
    [[nodiscard]] static std::string encode(const PlayerControllerSnapshot& snapshot);
    [[nodiscard]] static core::Result<PlayerControllerSnapshot>
    decode(std::string_view payload, const PlayerMovementConfig& config = {});
};

class PlayerControllerSnapshotBinaryCodec {
  public:
    [[nodiscard]] static std::string encode(const PlayerControllerSnapshot& snapshot);
    [[nodiscard]] static core::Result<PlayerControllerSnapshot>
    decode(std::string_view payload, const PlayerMovementConfig& config = {});
};

struct ReconciliationResult {
    PlayerControllerState state;
    std::size_t replayed_input_count = 0;
    std::size_t acknowledged_input_count = 0;
    double correction_distance = 0.0;
    bool hard_correction = false;
};

class MovementPredictionBuffer {
  public:
    explicit MovementPredictionBuffer(std::size_t capacity = 256);

    [[nodiscard]] core::Status record(PlayerInputFrame input);
    void set_collision_world_revision(std::uint64_t revision) noexcept;
    [[nodiscard]] core::Result<ReconciliationResult>
    reconcile(const PlayerControllerState& predicted, const PlayerControllerSnapshot& authoritative,
              const PlayerController& controller, const PlayerMovementModifiers& modifiers,
              ICharacterCollisionWorld& collision);
    void clear() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::vector<PlayerInputFrame> unacknowledged() const;

  private:
    std::size_t capacity_ = 0;
    std::uint64_t collision_world_revision_ = 0;
    std::deque<PlayerInputFrame> inputs_;
};

class ServerMovementInputQueue {
  public:
    explicit ServerMovementInputQueue(std::size_t capacity = 256);

    [[nodiscard]] core::Status push(PlayerInputFrame input);
    [[nodiscard]] core::Result<std::size_t> push_bundle(const PlayerInputBundle& bundle);
    [[nodiscard]] std::vector<PlayerInputFrame> drain(std::size_t maximum_count = 8);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    std::size_t capacity_ = 0;
    std::uint64_t last_sequence_ = 0;
    std::uint64_t last_tick_ = 0;
    std::deque<PlayerInputFrame> inputs_;
};

[[nodiscard]] net::TransportMessage make_movement_input_message(const PlayerInputFrame& input,
                                                                std::int64_t timestamp_ms);
[[nodiscard]] core::Result<PlayerInputFrame>
movement_input_from_transport(const net::TransportEnvelope& envelope);
[[nodiscard]] net::TransportMessage
make_movement_input_bundle_message(const PlayerInputBundle& bundle, std::int64_t timestamp_ms);
[[nodiscard]] core::Result<PlayerInputBundle>
movement_input_bundle_from_transport(const net::TransportEnvelope& envelope);
[[nodiscard]] net::TransportMessage
make_movement_snapshot_message(const PlayerControllerSnapshot& snapshot, std::int64_t timestamp_ms,
                               std::uint64_t transport_sequence = 0);
[[nodiscard]] core::Result<PlayerControllerSnapshot>
movement_snapshot_from_transport(const net::TransportEnvelope& envelope,
                                 const PlayerMovementConfig& config = {});
[[nodiscard]] net::TransportMessage make_player_assignment_message(core::NetId player_net_id,
                                                                   std::uint64_t sequence,
                                                                   std::int64_t timestamp_ms);
[[nodiscard]] core::Result<core::NetId>
player_assignment_from_transport(const net::TransportEnvelope& envelope);
[[nodiscard]] net::TransportMessage make_player_removal_message(core::NetId player_net_id,
                                                                std::uint64_t sequence,
                                                                std::int64_t timestamp_ms);
[[nodiscard]] core::Result<core::NetId>
player_removal_from_transport(const net::TransportEnvelope& envelope);

} // namespace heartstead::movement
