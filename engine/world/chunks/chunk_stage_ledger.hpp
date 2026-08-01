#pragma once

#include "engine/core/result.hpp"
#include "engine/world/chunks/chunk_identity.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace heartstead::world {

enum class ChunkStage : std::uint8_t {
    content,
    lighting,
    mesh,
    collision,
    persistence,
    replication,
    count,
};

enum class ChunkStageState : std::uint8_t {
    requested,
    running,
    ready,
    resident,
    stale,
    cancelled,
};

inline constexpr std::size_t chunk_stage_count = static_cast<std::size_t>(ChunkStage::count);

struct ChunkStageTicket {
    ChunkIdentity identity{};
    ChunkStage stage = ChunkStage::content;
    std::uint64_t revision = 0;

    [[nodiscard]] bool is_valid() const noexcept;

    friend auto operator<=>(const ChunkStageTicket&, const ChunkStageTicket&) = default;
};

struct ChunkStageRecord {
    std::uint64_t requested_revision = 1;
    std::uint64_t resident_request_revision = 1;
    std::uint64_t output_revision = 1;
    std::uint64_t stale_results = 0;
    std::uint64_t cancelled_results = 0;
    ChunkStageState state = ChunkStageState::resident;

    [[nodiscard]] bool has_resident_output() const noexcept;
    [[nodiscard]] bool resident_is_current() const noexcept;
};

struct ChunkStageCounts {
    std::size_t requested = 0;
    std::size_t running = 0;
    std::size_t ready = 0;
    std::size_t resident = 0;
    std::size_t stale = 0;
    std::size_t cancelled = 0;
    std::size_t available_resident_outputs = 0;
    std::uint64_t stale_results = 0;
    std::uint64_t cancelled_results = 0;
};

class ChunkStageLedger {
  public:
    // The ledger belongs to the world owner thread. Workers may carry tickets, but must never
    // mutate a live ledger or retain a pointer to one.
    ChunkStageLedger() = default;

    [[nodiscard]] const ChunkStageRecord& record(ChunkStage stage) const noexcept;
    [[nodiscard]] std::uint64_t requested_revision(ChunkStage stage) const noexcept;
    [[nodiscard]] bool is_current(ChunkStage stage, std::uint64_t revision) const noexcept;

    [[nodiscard]] std::uint64_t request(ChunkStage stage) noexcept;
    [[nodiscard]] std::uint64_t ensure_requested(ChunkStage stage) noexcept;
    [[nodiscard]] core::Status mark_running(ChunkStage stage, std::uint64_t revision);
    [[nodiscard]] core::Status mark_ready(ChunkStage stage, std::uint64_t revision);
    [[nodiscard]] core::Status publish(ChunkStage stage, std::uint64_t revision,
                                       bool output_changed = true);
    [[nodiscard]] core::Status retry(ChunkStage stage, std::uint64_t revision);
    [[nodiscard]] core::Status note_stale(ChunkStage stage, std::uint64_t revision);
    [[nodiscard]] core::Status note_cancelled(ChunkStage stage, std::uint64_t revision);

    void publish_content(std::uint64_t content_revision) noexcept;

  private:
    [[nodiscard]] static std::size_t index_of(ChunkStage stage) noexcept;
    [[nodiscard]] core::Status validate_transition(ChunkStage stage, std::uint64_t revision,
                                                   ChunkStageState expected) const;

    std::array<ChunkStageRecord, chunk_stage_count> stages_{};
};

[[nodiscard]] std::string_view chunk_stage_name(ChunkStage stage) noexcept;
[[nodiscard]] std::string_view chunk_stage_state_name(ChunkStageState state) noexcept;

} // namespace heartstead::world
