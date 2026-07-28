#include "game/presentation/client_presentation.hpp"

#include "game/runtime/client_runtime.hpp"

#include <limits>
#include <utility>

namespace heartstead::game {

core::Result<PresentationSynchronizationStats>
ClientPresentationSynchronizer::synchronize(const ClientRuntime& client,
                                            PresentationWorld& presentation) {
    PresentationSynchronizationStats stats;
    stats.adapter_count = 1;
    std::unordered_set<std::uint64_t> current_players;
    const auto player_prototype = core::PrototypeId::parse("base:entities/player");
    if (!player_prototype.has_value()) {
        return core::Result<PresentationSynchronizationStats>::failure(
            "client_presentation.invalid_player_prototype",
            "built-in player visual prototype id is invalid");
    }
    for (const auto* snapshot : client.movement_snapshots()) {
        current_players.insert(snapshot->player_net_id.value());
        PresentationObjectUpdate update;
        update.source_net_id = snapshot->player_net_id;
        update.visual_prototype = *player_prototype;
        update.transform.position = snapshot->state.position;
        update.transform.rotation_degrees = {
            static_cast<double>(snapshot->state.pitch_centidegrees) * 0.01,
            static_cast<double>(snapshot->state.yaw_centidegrees) * 0.01, 0.0};
        update.locomotion = snapshot->state.locomotion_animation;
        update.local_bounds = {{-0.3F, 0.0F, -0.3F}, {0.3F, 1.8F, 0.3F}};
        update.source_revision =
            snapshot->state.simulation_tick == std::numeric_limits<std::uint64_t>::max()
                ? snapshot->state.simulation_tick
                : snapshot->state.simulation_tick + 1;
        const auto* previous = presentation.find_object(snapshot->player_net_id);
        const auto previous_revision = previous == nullptr ? 0 : previous->source_revision;
        auto synchronized = presentation.upsert_object(update);
        if (!synchronized) {
            return core::Result<PresentationSynchronizationStats>::failure(
                synchronized.error().code, synchronized.error().message);
        }
        if (previous == nullptr) {
            ++stats.inserted_objects;
        } else if (previous_revision == update.source_revision) {
            ++stats.unchanged_objects;
        } else {
            ++stats.updated_objects;
        }
    }
    for (const auto retained : retained_players_) {
        if (current_players.contains(retained)) {
            continue;
        }
        auto status = presentation.remove_object(core::NetId::from_value(retained));
        if (!status) {
            return core::Result<PresentationSynchronizationStats>::failure(status.error().code,
                                                                           status.error().message);
        }
        ++stats.removed_objects;
    }
    retained_players_ = std::move(current_players);
    return core::Result<PresentationSynchronizationStats>::success(stats);
}

void ClientPresentationSynchronizer::clear() noexcept {
    retained_players_.clear();
}

} // namespace heartstead::game
