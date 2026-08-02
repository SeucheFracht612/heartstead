#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace heartstead::net {

enum class ReplicationBudgetLimit : std::uint8_t {
    none,
    global_message_count,
    global_payload_bytes,
    global_serialization_time,
    client_message_count,
    client_payload_bytes,
    client_serialization_time,
};

struct ReplicationTickBudgetConfig {
    std::uint32_t max_messages_per_tick = 512;
    std::uint64_t max_payload_bytes_per_tick = 256u * 1024u;
    std::uint64_t max_serialization_time_us_per_tick = 4'000;
    std::uint32_t max_messages_per_client_per_tick = 128;
    std::uint64_t max_payload_bytes_per_client_per_tick = 64u * 1024u;
    std::uint64_t max_serialization_time_us_per_client_per_tick = 2'000;

    [[nodiscard]] core::Status validate() const noexcept;
    friend bool operator==(const ReplicationTickBudgetConfig&,
                           const ReplicationTickBudgetConfig&) = default;
};

struct ReplicationBudgetLimitCounts {
    std::uint64_t global_message_count = 0;
    std::uint64_t global_payload_bytes = 0;
    std::uint64_t global_serialization_time = 0;
    std::uint64_t client_message_count = 0;
    std::uint64_t client_payload_bytes = 0;
    std::uint64_t client_serialization_time = 0;

    [[nodiscard]] std::uint64_t total() const noexcept;
    friend bool operator==(const ReplicationBudgetLimitCounts&,
                           const ReplicationBudgetLimitCounts&) = default;
};

struct ReplicationClientTickBudgetStats {
    core::NetId client_id;
    std::uint64_t considered_message_count = 0;
    std::uint64_t admitted_message_count = 0;
    std::uint64_t deferred_message_count = 0;
    std::uint64_t admitted_payload_bytes = 0;
    // A shared payload is encoded once globally. Every recipient that participated in that codec
    // operation is conservatively charged its complete measured cost, even if bytes/messages later
    // reject the candidate, so rejected large payloads cannot bypass the client time boundary.
    std::uint64_t attributed_serialization_time_us = 0;
    std::uint64_t maximum_attributed_serialization_time_us = 0;
    std::uint64_t serialization_time_overshoot_us = 0;
    ReplicationBudgetLimitCounts deferrals;

    friend bool operator==(const ReplicationClientTickBudgetStats&,
                           const ReplicationClientTickBudgetStats&) = default;
};

struct ReplicationTickBudgetStats {
    ReplicationTickBudgetConfig budget;
    std::uint64_t tick = 0;
    std::uint64_t considered_message_count = 0;
    std::uint64_t admitted_message_count = 0;
    std::uint64_t deferred_message_count = 0;
    std::uint64_t admitted_payload_bytes = 0;
    std::uint64_t attributed_serialization_time_us = 0;
    std::uint64_t shared_serialization_operation_count = 0;
    std::uint64_t shared_serialization_time_us = 0;
    std::uint64_t maximum_shared_serialization_time_us = 0;
    // A codec call cannot be preempted. Work starts only below the cap, so this is bounded by one
    // completed shared serialization operation and remains visible instead of being hidden.
    std::uint64_t serialization_time_overshoot_us = 0;
    bool global_serialization_budget_exhausted = false;
    bool shared_serialization_in_progress = false;
    ReplicationBudgetLimitCounts deferrals;
    std::vector<ReplicationClientTickBudgetStats> clients;

    friend bool operator==(const ReplicationTickBudgetStats&,
                           const ReplicationTickBudgetStats&) = default;
};

struct ReplicationBudgetAdmission {
    bool admitted = false;
    ReplicationBudgetLimit limit = ReplicationBudgetLimit::none;

    explicit operator bool() const noexcept {
        return admitted;
    }
};

// Deterministic admission controller for replaceable, already-relevance-filtered replication.
// The caller times codecs and injects elapsed microseconds, which keeps the policy testable without
// wall-clock sleeps and makes the non-preemptible-operation boundary explicit.
class ReplicationTickBudget {
  public:
    explicit ReplicationTickBudget(ReplicationTickBudgetConfig config = {}) noexcept;

    [[nodiscard]] const ReplicationTickBudgetConfig& config() const noexcept;
    [[nodiscard]] core::Status begin_tick(std::uint64_t tick) noexcept;

    // Returns a limit that makes serialization pointless before the payload size/cost is known.
    // A result of none means at least this recipient could still admit a non-empty candidate.
    [[nodiscard]] core::Result<ReplicationBudgetLimit>
    preparation_limit(core::NetId client_id) const;

    // At most one shared codec call may be in flight. A false success means the global time budget
    // is already exhausted; a failed result indicates incorrect begin/finish API use.
    [[nodiscard]] core::Result<bool> begin_shared_serialization() noexcept;
    [[nodiscard]] core::Status finish_shared_serialization(std::uint64_t elapsed_time_us) noexcept;

    [[nodiscard]] core::Result<ReplicationBudgetAdmission>
    admit_prepared(core::NetId client_id, std::uint64_t payload_bytes,
                   std::uint64_t attributed_serialization_time_us);
    [[nodiscard]] core::Status record_deferred(core::NetId client_id, ReplicationBudgetLimit limit);

    [[nodiscard]] ReplicationTickBudgetStats snapshot() const;

  private:
    [[nodiscard]] ReplicationClientTickBudgetStats& client_stats(core::NetId client_id);
    [[nodiscard]] const ReplicationClientTickBudgetStats*
    find_client_stats(core::NetId client_id) const noexcept;
    void increment_deferral(ReplicationClientTickBudgetStats& client,
                            ReplicationBudgetLimit limit) noexcept;

    ReplicationTickBudgetConfig config_;
    ReplicationTickBudgetStats stats_;
    std::map<core::NetId, ReplicationClientTickBudgetStats> clients_;
};

[[nodiscard]] std::string_view replication_budget_limit_name(ReplicationBudgetLimit limit) noexcept;
[[nodiscard]] core::Status
validate_replication_tick_budget_stats(const ReplicationTickBudgetStats& stats) noexcept;

} // namespace heartstead::net
