#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/net/transport.hpp"
#include "engine/world/operations/world_operation.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::net {

inline constexpr std::string_view replication_world_events_payload_type =
    "replication.world_events.bin.v1";
inline constexpr std::string_view replication_world_events_legacy_payload_type =
    "replication.world_events";

struct ReplicationBatch {
    std::uint64_t command_sequence = 0;
    std::string command_type;
    std::vector<world::OperationEvent> events;
    std::vector<core::SaveId> reserved_ids;
    std::uint64_t replication_sequence = 0;
    core::NetId source_client_id;
};

[[nodiscard]] std::uint64_t replication_stream_sequence(const ReplicationBatch& batch) noexcept;

struct ReplicationInterestRule {
    core::NetId client_id;
    std::vector<core::SaveId> visible_subjects;
    bool receives_global_events = true;
};

struct ReplicationPrivateAccessRule {
    core::NetId client_id;
    std::vector<core::SaveId> private_subjects;
};

struct ReplicationChunkInterestRule {
    core::NetId client_id;
    std::vector<world::ChunkCoord> visible_chunks;
};

struct ReplicationRelevancePolicy {
    bool broadcast_by_default = true;
    std::vector<ReplicationInterestRule> client_rules;
    std::vector<ReplicationPrivateAccessRule> private_access_rules;
    std::vector<ReplicationChunkInterestRule> chunk_interest_rules;
};

struct ReplicationRelevanceDecision {
    core::NetId client_id;
    bool relevant = false;
    bool explicit_rule = false;
    std::uint32_t relevant_event_count = 0;
    std::uint32_t relevant_spatial_event_count = 0;
    std::uint32_t filtered_spatial_event_count = 0;
    std::uint64_t filtered_spatial_payload_bytes = 0;
    std::string reason;
};

struct ReplicationRelevanceReport {
    std::uint64_t command_sequence = 0;
    std::string command_type;
    bool broadcast_by_default = true;
    std::uint32_t event_count = 0;
    std::uint32_t candidate_client_count = 0;
    std::uint32_t relevant_client_count = 0;
    std::uint32_t filtered_client_count = 0;
    std::uint32_t spatial_event_count = 0;
    std::uint64_t relevant_spatial_event_delivery_count = 0;
    std::uint64_t filtered_spatial_event_delivery_count = 0;
    std::uint64_t filtered_spatial_payload_bytes = 0;
    std::vector<ReplicationRelevanceDecision> decisions;
    std::uint64_t replication_sequence = 0;
    core::NetId source_client_id;
};

[[nodiscard]] std::uint64_t
replication_stream_sequence(const ReplicationRelevanceReport& report) noexcept;

struct ReplicationIntakeBatchReport {
    std::uint64_t command_sequence = 0;
    std::string command_type;
    std::uint32_t event_count = 0;
    std::uint32_t reserved_id_count = 0;
    bool has_global_events = false;
    bool has_subject_events = false;
};

struct ReplicationIntakeReport {
    std::uint32_t batch_count = 0;
    std::uint32_t event_count = 0;
    std::uint32_t reserved_id_count = 0;
    bool strictly_increasing_sequences = true;
    bool has_global_events = false;
    bool has_subject_events = false;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::vector<ReplicationIntakeBatchReport> batches;
};

class ReplicationTextCodec {
  public:
    [[nodiscard]] static std::string encode(const ReplicationBatch& batch);
    [[nodiscard]] static core::Result<ReplicationBatch> decode(std::string_view text);
};

class ReplicationBinaryCodec {
  public:
    [[nodiscard]] static std::string encode(const ReplicationBatch& batch);
    [[nodiscard]] static core::Result<ReplicationBatch> decode(std::string_view bytes);
};

class ReplicationRelevance {
  public:
    [[nodiscard]] static ReplicationRelevanceReport
    evaluate(const ReplicationRelevancePolicy& policy, const ReplicationBatch& batch,
             const std::vector<core::NetId>& candidate_clients);
    [[nodiscard]] static bool subject_is_visible(const ReplicationRelevancePolicy& policy,
                                                 core::NetId client_id,
                                                 core::SaveId subject) noexcept;
    [[nodiscard]] static bool private_subject_is_visible(const ReplicationRelevancePolicy& policy,
                                                         core::NetId client_id,
                                                         core::SaveId subject) noexcept;
    [[nodiscard]] static bool chunk_is_visible(const ReplicationRelevancePolicy& policy,
                                               core::NetId client_id,
                                               world::ChunkCoord chunk) noexcept;
    [[nodiscard]] static bool event_is_visible(const ReplicationRelevancePolicy& policy,
                                               core::NetId client_id,
                                               const world::OperationEvent& event) noexcept;
    [[nodiscard]] static ReplicationBatch
    filter_for_client(const ReplicationRelevancePolicy& policy, const ReplicationBatch& batch,
                      core::NetId client_id);
};

[[nodiscard]] bool
replication_event_requires_private_access(const world::OperationEvent& event) noexcept;

class ReplicationIntake {
  public:
    [[nodiscard]] static ReplicationIntakeReport
    summarize(std::span<const ReplicationBatch> batches);
};

[[nodiscard]] TransportMessage make_replication_transport_message(const ReplicationBatch& batch,
                                                                  std::int64_t server_time_ms);
[[nodiscard]] core::Result<ReplicationBatch>
replication_batch_from_transport(const TransportEnvelope& envelope);

} // namespace heartstead::net
