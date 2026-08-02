#include "engine/net/replication_budget.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace heartstead::net {

namespace {

[[nodiscard]] bool would_exceed(std::uint64_t used, std::uint64_t amount,
                                std::uint64_t limit) noexcept {
    return used >= limit ? amount != 0 : amount > limit - used;
}

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

void saturating_add(std::uint64_t& value, std::uint64_t amount) noexcept {
    value = amount > std::numeric_limits<std::uint64_t>::max() - value
                ? std::numeric_limits<std::uint64_t>::max()
                : value + amount;
}

void increment_limit(ReplicationBudgetLimitCounts& counts, ReplicationBudgetLimit limit) noexcept {
    switch (limit) {
    case ReplicationBudgetLimit::global_message_count:
        saturating_increment(counts.global_message_count);
        return;
    case ReplicationBudgetLimit::global_payload_bytes:
        saturating_increment(counts.global_payload_bytes);
        return;
    case ReplicationBudgetLimit::global_serialization_time:
        saturating_increment(counts.global_serialization_time);
        return;
    case ReplicationBudgetLimit::client_message_count:
        saturating_increment(counts.client_message_count);
        return;
    case ReplicationBudgetLimit::client_payload_bytes:
        saturating_increment(counts.client_payload_bytes);
        return;
    case ReplicationBudgetLimit::client_serialization_time:
        saturating_increment(counts.client_serialization_time);
        return;
    case ReplicationBudgetLimit::none:
        return;
    }
}

[[nodiscard]] bool is_deferral_limit(ReplicationBudgetLimit limit) noexcept {
    switch (limit) {
    case ReplicationBudgetLimit::global_message_count:
    case ReplicationBudgetLimit::global_payload_bytes:
    case ReplicationBudgetLimit::global_serialization_time:
    case ReplicationBudgetLimit::client_message_count:
    case ReplicationBudgetLimit::client_payload_bytes:
    case ReplicationBudgetLimit::client_serialization_time:
        return true;
    case ReplicationBudgetLimit::none:
        return false;
    }
    return false;
}

[[nodiscard]] bool count_equation_holds(std::uint64_t considered, std::uint64_t admitted,
                                        std::uint64_t deferred) noexcept {
    return admitted <= considered && considered - admitted == deferred;
}

[[nodiscard]] bool checked_add(std::uint64_t& total, std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        return false;
    }
    total += value;
    return true;
}

[[nodiscard]] core::Status invalid_stats(std::string message) {
    return core::Status::failure("replication_tick_budget.invalid_stats", std::move(message));
}

} // namespace

core::Status ReplicationTickBudgetConfig::validate() const noexcept {
    if (max_messages_per_tick == 0 || max_payload_bytes_per_tick == 0 ||
        max_serialization_time_us_per_tick == 0 || max_messages_per_client_per_tick == 0 ||
        max_payload_bytes_per_client_per_tick == 0 ||
        max_serialization_time_us_per_client_per_tick == 0) {
        return core::Status::failure(
            "replication_tick_budget.invalid_config",
            "global and per-client replication message, payload-byte, and serialization-time "
            "limits must all be non-zero");
    }
    return core::Status::ok();
}

std::uint64_t ReplicationBudgetLimitCounts::total() const noexcept {
    std::uint64_t result = 0;
    saturating_add(result, global_message_count);
    saturating_add(result, global_payload_bytes);
    saturating_add(result, global_serialization_time);
    saturating_add(result, client_message_count);
    saturating_add(result, client_payload_bytes);
    saturating_add(result, client_serialization_time);
    return result;
}

ReplicationTickBudget::ReplicationTickBudget(ReplicationTickBudgetConfig config) noexcept
    : config_(config) {
    stats_.budget = config_;
}

const ReplicationTickBudgetConfig& ReplicationTickBudget::config() const noexcept {
    return config_;
}

core::Status ReplicationTickBudget::begin_tick(std::uint64_t tick) noexcept {
    auto status = config_.validate();
    if (!status) {
        return status;
    }
    if (stats_.shared_serialization_in_progress) {
        return core::Status::failure(
            "replication_tick_budget.serialization_in_progress",
            "cannot begin a replication tick while shared serialization is in progress");
    }
    stats_ = {};
    stats_.budget = config_;
    stats_.tick = tick;
    clients_.clear();
    return core::Status::ok();
}

core::Result<ReplicationBudgetLimit>
ReplicationTickBudget::preparation_limit(core::NetId client_id) const {
    if (!client_id.is_valid()) {
        return core::Result<ReplicationBudgetLimit>::failure(
            "replication_tick_budget.invalid_client",
            "replication budget candidates require a valid client identity");
    }
    if (stats_.admitted_message_count >= config_.max_messages_per_tick) {
        return core::Result<ReplicationBudgetLimit>::success(
            ReplicationBudgetLimit::global_message_count);
    }
    if (stats_.admitted_payload_bytes >= config_.max_payload_bytes_per_tick) {
        return core::Result<ReplicationBudgetLimit>::success(
            ReplicationBudgetLimit::global_payload_bytes);
    }
    if (stats_.shared_serialization_time_us >= config_.max_serialization_time_us_per_tick) {
        return core::Result<ReplicationBudgetLimit>::success(
            ReplicationBudgetLimit::global_serialization_time);
    }
    const auto* client = find_client_stats(client_id);
    if (client == nullptr) {
        return core::Result<ReplicationBudgetLimit>::success(ReplicationBudgetLimit::none);
    }
    if (client->admitted_message_count >= config_.max_messages_per_client_per_tick) {
        return core::Result<ReplicationBudgetLimit>::success(
            ReplicationBudgetLimit::client_message_count);
    }
    if (client->admitted_payload_bytes >= config_.max_payload_bytes_per_client_per_tick) {
        return core::Result<ReplicationBudgetLimit>::success(
            ReplicationBudgetLimit::client_payload_bytes);
    }
    if (client->attributed_serialization_time_us >=
        config_.max_serialization_time_us_per_client_per_tick) {
        return core::Result<ReplicationBudgetLimit>::success(
            ReplicationBudgetLimit::client_serialization_time);
    }
    return core::Result<ReplicationBudgetLimit>::success(ReplicationBudgetLimit::none);
}

core::Result<bool> ReplicationTickBudget::begin_shared_serialization() noexcept {
    if (stats_.shared_serialization_in_progress) {
        return core::Result<bool>::failure(
            "replication_tick_budget.serialization_already_in_progress",
            "shared replication serialization cannot be nested");
    }
    if (stats_.shared_serialization_time_us >= config_.max_serialization_time_us_per_tick) {
        stats_.global_serialization_budget_exhausted = true;
        return core::Result<bool>::success(false);
    }
    stats_.shared_serialization_in_progress = true;
    return core::Result<bool>::success(true);
}

core::Status
ReplicationTickBudget::finish_shared_serialization(std::uint64_t elapsed_time_us) noexcept {
    if (!stats_.shared_serialization_in_progress) {
        return core::Status::failure(
            "replication_tick_budget.serialization_not_in_progress",
            "shared replication serialization must begin before it can finish");
    }
    stats_.shared_serialization_in_progress = false;
    saturating_increment(stats_.shared_serialization_operation_count);
    saturating_add(stats_.shared_serialization_time_us, elapsed_time_us);
    stats_.maximum_shared_serialization_time_us =
        std::max(stats_.maximum_shared_serialization_time_us, elapsed_time_us);
    stats_.serialization_time_overshoot_us =
        stats_.shared_serialization_time_us > config_.max_serialization_time_us_per_tick
            ? stats_.shared_serialization_time_us - config_.max_serialization_time_us_per_tick
            : 0;
    stats_.global_serialization_budget_exhausted =
        stats_.shared_serialization_time_us >= config_.max_serialization_time_us_per_tick;
    return core::Status::ok();
}

core::Result<ReplicationBudgetAdmission>
ReplicationTickBudget::admit_prepared(core::NetId client_id, std::uint64_t payload_bytes,
                                      std::uint64_t attributed_serialization_time_us) {
    if (!client_id.is_valid()) {
        return core::Result<ReplicationBudgetAdmission>::failure(
            "replication_tick_budget.invalid_client",
            "replication budget candidates require a valid client identity");
    }
    if (stats_.shared_serialization_in_progress) {
        return core::Result<ReplicationBudgetAdmission>::failure(
            "replication_tick_budget.serialization_in_progress",
            "a prepared candidate cannot be admitted before shared serialization finishes");
    }

    auto& client = client_stats(client_id);
    saturating_increment(stats_.considered_message_count);
    saturating_increment(client.considered_message_count);

    // Runtime callers preflight every recipient before the shared codec call. Preserve that
    // contract for direct callers too: once a client's prior work reached the boundary, another
    // payload must not be attributed to it.
    if (client.attributed_serialization_time_us >=
        config_.max_serialization_time_us_per_client_per_tick) {
        increment_deferral(client, ReplicationBudgetLimit::client_serialization_time);
        return core::Result<ReplicationBudgetAdmission>::success(
            {false, ReplicationBudgetLimit::client_serialization_time});
    }
    saturating_add(stats_.attributed_serialization_time_us, attributed_serialization_time_us);
    saturating_add(client.attributed_serialization_time_us, attributed_serialization_time_us);
    client.maximum_attributed_serialization_time_us =
        std::max(client.maximum_attributed_serialization_time_us, attributed_serialization_time_us);
    client.serialization_time_overshoot_us =
        client.attributed_serialization_time_us >
                config_.max_serialization_time_us_per_client_per_tick
            ? client.attributed_serialization_time_us -
                  config_.max_serialization_time_us_per_client_per_tick
            : 0;

    ReplicationBudgetLimit limit = ReplicationBudgetLimit::none;
    if (stats_.admitted_message_count >= config_.max_messages_per_tick) {
        limit = ReplicationBudgetLimit::global_message_count;
    } else if (would_exceed(stats_.admitted_payload_bytes, payload_bytes,
                            config_.max_payload_bytes_per_tick)) {
        limit = ReplicationBudgetLimit::global_payload_bytes;
    } else if (client.admitted_message_count >= config_.max_messages_per_client_per_tick) {
        limit = ReplicationBudgetLimit::client_message_count;
    } else if (would_exceed(client.admitted_payload_bytes, payload_bytes,
                            config_.max_payload_bytes_per_client_per_tick)) {
        limit = ReplicationBudgetLimit::client_payload_bytes;
    }

    if (limit != ReplicationBudgetLimit::none) {
        increment_deferral(client, limit);
        return core::Result<ReplicationBudgetAdmission>::success({false, limit});
    }

    saturating_increment(stats_.admitted_message_count);
    saturating_add(stats_.admitted_payload_bytes, payload_bytes);
    saturating_increment(client.admitted_message_count);
    saturating_add(client.admitted_payload_bytes, payload_bytes);
    return core::Result<ReplicationBudgetAdmission>::success({true, ReplicationBudgetLimit::none});
}

core::Status ReplicationTickBudget::record_deferred(core::NetId client_id,
                                                    ReplicationBudgetLimit limit) {
    if (!client_id.is_valid()) {
        return core::Status::failure(
            "replication_tick_budget.invalid_client",
            "replication budget candidates require a valid client identity");
    }
    if (!is_deferral_limit(limit)) {
        return core::Status::failure(
            "replication_tick_budget.invalid_deferral",
            "a deferred replication candidate requires a concrete limiting quota");
    }
    auto& client = client_stats(client_id);
    saturating_increment(stats_.considered_message_count);
    saturating_increment(client.considered_message_count);
    increment_deferral(client, limit);
    return core::Status::ok();
}

ReplicationTickBudgetStats ReplicationTickBudget::snapshot() const {
    auto result = stats_;
    result.clients.clear();
    result.clients.reserve(clients_.size());
    for (const auto& [_, client] : clients_) {
        result.clients.push_back(client);
    }
    return result;
}

ReplicationClientTickBudgetStats& ReplicationTickBudget::client_stats(core::NetId client_id) {
    auto [found, inserted] = clients_.try_emplace(client_id);
    if (inserted) {
        found->second.client_id = client_id;
    }
    return found->second;
}

const ReplicationClientTickBudgetStats*
ReplicationTickBudget::find_client_stats(core::NetId client_id) const noexcept {
    const auto found = clients_.find(client_id);
    return found == clients_.end() ? nullptr : &found->second;
}

void ReplicationTickBudget::increment_deferral(ReplicationClientTickBudgetStats& client,
                                               ReplicationBudgetLimit limit) noexcept {
    saturating_increment(stats_.deferred_message_count);
    saturating_increment(client.deferred_message_count);
    increment_limit(stats_.deferrals, limit);
    increment_limit(client.deferrals, limit);
}

std::string_view replication_budget_limit_name(ReplicationBudgetLimit limit) noexcept {
    switch (limit) {
    case ReplicationBudgetLimit::none:
        return "none";
    case ReplicationBudgetLimit::global_message_count:
        return "global_message_count";
    case ReplicationBudgetLimit::global_payload_bytes:
        return "global_payload_bytes";
    case ReplicationBudgetLimit::global_serialization_time:
        return "global_serialization_time";
    case ReplicationBudgetLimit::client_message_count:
        return "client_message_count";
    case ReplicationBudgetLimit::client_payload_bytes:
        return "client_payload_bytes";
    case ReplicationBudgetLimit::client_serialization_time:
        return "client_serialization_time";
    }
    return "unknown";
}

core::Status
validate_replication_tick_budget_stats(const ReplicationTickBudgetStats& stats) noexcept {
    auto config_status = stats.budget.validate();
    if (!config_status) {
        return invalid_stats("replication statistics contain an invalid budget");
    }
    if (stats.shared_serialization_in_progress) {
        return invalid_stats("replication statistics were captured during serialization");
    }
    if (!count_equation_holds(stats.considered_message_count, stats.admitted_message_count,
                              stats.deferred_message_count) ||
        stats.deferrals.total() != stats.deferred_message_count) {
        return invalid_stats("global considered/admitted/deferred counters are inconsistent");
    }
    if (stats.admitted_message_count > stats.budget.max_messages_per_tick ||
        stats.admitted_payload_bytes > stats.budget.max_payload_bytes_per_tick) {
        return invalid_stats("global message or payload-byte quota was exceeded");
    }
    const auto expected_overshoot =
        stats.shared_serialization_time_us > stats.budget.max_serialization_time_us_per_tick
            ? stats.shared_serialization_time_us - stats.budget.max_serialization_time_us_per_tick
            : 0;
    if (stats.serialization_time_overshoot_us != expected_overshoot ||
        stats.serialization_time_overshoot_us > stats.maximum_shared_serialization_time_us ||
        stats.global_serialization_budget_exhausted !=
            (stats.shared_serialization_time_us >=
             stats.budget.max_serialization_time_us_per_tick)) {
        return invalid_stats("global serialization time or overshoot counters are inconsistent");
    }
    if ((stats.shared_serialization_operation_count == 0 &&
         (stats.shared_serialization_time_us != 0 ||
          stats.maximum_shared_serialization_time_us != 0)) ||
        stats.maximum_shared_serialization_time_us > stats.shared_serialization_time_us) {
        return invalid_stats("shared serialization operation counters are inconsistent");
    }

    std::uint64_t considered = 0;
    std::uint64_t admitted = 0;
    std::uint64_t deferred = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t attributed_time_us = 0;
    core::NetId previous_client;
    for (const auto& client : stats.clients) {
        if (!client.client_id.is_valid() ||
            (previous_client.is_valid() && client.client_id <= previous_client)) {
            return invalid_stats("per-client statistics are not strictly identity-ordered");
        }
        previous_client = client.client_id;
        if (!count_equation_holds(client.considered_message_count, client.admitted_message_count,
                                  client.deferred_message_count) ||
            client.deferrals.total() != client.deferred_message_count) {
            return invalid_stats(
                "per-client considered/admitted/deferred counters are inconsistent");
        }
        if (client.admitted_message_count > stats.budget.max_messages_per_client_per_tick ||
            client.admitted_payload_bytes > stats.budget.max_payload_bytes_per_client_per_tick) {
            return invalid_stats("a per-client message or payload-byte quota was exceeded");
        }
        const auto expected_client_overshoot =
            client.attributed_serialization_time_us >
                    stats.budget.max_serialization_time_us_per_client_per_tick
                ? client.attributed_serialization_time_us -
                      stats.budget.max_serialization_time_us_per_client_per_tick
                : 0;
        if (client.serialization_time_overshoot_us != expected_client_overshoot ||
            client.serialization_time_overshoot_us >
                client.maximum_attributed_serialization_time_us ||
            (client.attributed_serialization_time_us == 0 &&
             client.maximum_attributed_serialization_time_us != 0) ||
            client.maximum_attributed_serialization_time_us >
                client.attributed_serialization_time_us) {
            return invalid_stats("per-client serialization time counters are inconsistent");
        }
        if (!checked_add(considered, client.considered_message_count) ||
            !checked_add(admitted, client.admitted_message_count) ||
            !checked_add(deferred, client.deferred_message_count) ||
            !checked_add(payload_bytes, client.admitted_payload_bytes) ||
            !checked_add(attributed_time_us, client.attributed_serialization_time_us)) {
            return invalid_stats("per-client replication counters overflow their aggregate");
        }
    }
    if (considered != stats.considered_message_count || admitted != stats.admitted_message_count ||
        deferred != stats.deferred_message_count || payload_bytes != stats.admitted_payload_bytes ||
        attributed_time_us != stats.attributed_serialization_time_us) {
        return invalid_stats("per-client counters do not reproduce global admission totals");
    }
    return core::Status::ok();
}

} // namespace heartstead::net
