#include "engine/world/chunks/chunk_stage_ledger.hpp"

#include <exception>
#include <limits>

namespace heartstead::world {

namespace {

[[nodiscard]] core::Status invalid_stage_status() {
    return core::Status::failure("chunk_stage.invalid_stage", "chunk stage is outside the ledger");
}

[[nodiscard]] core::Status invalid_revision_status() {
    return core::Status::failure("chunk_stage.invalid_revision",
                                 "chunk stage revision must be nonzero");
}

[[nodiscard]] core::Status stale_ticket_status() {
    return core::Status::failure("chunk_stage.stale_ticket",
                                 "chunk stage ticket no longer names the requested revision");
}

[[nodiscard]] core::Status invalid_transition_status() {
    return core::Status::failure("chunk_stage.invalid_transition",
                                 "chunk stage transition is not valid from its current state");
}

} // namespace

bool ChunkStageTicket::is_valid() const noexcept {
    return identity.is_valid() && static_cast<std::size_t>(stage) < chunk_stage_count &&
           revision != 0;
}

bool ChunkStageRecord::has_resident_output() const noexcept {
    return resident_request_revision != 0 && output_revision != 0;
}

bool ChunkStageRecord::resident_is_current() const noexcept {
    return state == ChunkStageState::resident && resident_request_revision == requested_revision &&
           has_resident_output();
}

const ChunkStageRecord& ChunkStageLedger::record(ChunkStage stage) const noexcept {
    return stages_[index_of(stage)];
}

std::uint64_t ChunkStageLedger::requested_revision(ChunkStage stage) const noexcept {
    return record(stage).requested_revision;
}

bool ChunkStageLedger::is_current(ChunkStage stage, std::uint64_t revision) const noexcept {
    return stage != ChunkStage::count && revision != 0 &&
           record(stage).requested_revision == revision;
}

std::uint64_t ChunkStageLedger::request(ChunkStage stage) noexcept {
    if (stage == ChunkStage::content || stage == ChunkStage::count) {
        return 0;
    }
    auto& current = stages_[index_of(stage)];
    if (current.requested_revision == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++current.requested_revision;
    current.state = ChunkStageState::requested;
    return current.requested_revision;
}

std::uint64_t ChunkStageLedger::ensure_requested(ChunkStage stage) noexcept {
    if (stage == ChunkStage::content || stage == ChunkStage::count) {
        return 0;
    }
    const auto state = record(stage).state;
    if (state == ChunkStageState::requested || state == ChunkStageState::running ||
        state == ChunkStageState::ready) {
        return requested_revision(stage);
    }
    return request(stage);
}

core::Status ChunkStageLedger::mark_running(ChunkStage stage, std::uint64_t revision) {
    auto status = validate_transition(stage, revision, ChunkStageState::requested);
    if (!status) {
        return status;
    }
    stages_[index_of(stage)].state = ChunkStageState::running;
    return core::Status::ok();
}

core::Status ChunkStageLedger::mark_ready(ChunkStage stage, std::uint64_t revision) {
    auto status = validate_transition(stage, revision, ChunkStageState::running);
    if (!status) {
        return status;
    }
    stages_[index_of(stage)].state = ChunkStageState::ready;
    return core::Status::ok();
}

core::Status ChunkStageLedger::publish(ChunkStage stage, std::uint64_t revision,
                                       bool output_changed) {
    auto status = validate_transition(stage, revision, ChunkStageState::ready);
    if (!status) {
        return status;
    }
    auto& current = stages_[index_of(stage)];
    if (output_changed) {
        if (current.output_revision == std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++current.output_revision;
    }
    current.resident_request_revision = revision;
    current.state = ChunkStageState::resident;
    return core::Status::ok();
}

core::Status ChunkStageLedger::retry(ChunkStage stage, std::uint64_t revision) {
    if (stage == ChunkStage::content || stage == ChunkStage::count) {
        return invalid_stage_status();
    }
    if (revision == 0) {
        return invalid_revision_status();
    }
    if (!is_current(stage, revision)) {
        return stale_ticket_status();
    }
    auto& current = stages_[index_of(stage)];
    if (current.state != ChunkStageState::running && current.state != ChunkStageState::ready) {
        return invalid_transition_status();
    }
    current.state = ChunkStageState::requested;
    return core::Status::ok();
}

core::Status ChunkStageLedger::note_stale(ChunkStage stage, std::uint64_t revision) {
    if (stage == ChunkStage::content || stage == ChunkStage::count) {
        return invalid_stage_status();
    }
    if (revision == 0) {
        return invalid_revision_status();
    }
    auto& current = stages_[index_of(stage)];
    if (current.stale_results == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++current.stale_results;
    if (current.requested_revision == revision) {
        current.state = ChunkStageState::stale;
    }
    return core::Status::ok();
}

core::Status ChunkStageLedger::note_cancelled(ChunkStage stage, std::uint64_t revision) {
    if (stage == ChunkStage::content || stage == ChunkStage::count) {
        return invalid_stage_status();
    }
    if (revision == 0) {
        return invalid_revision_status();
    }
    auto& current = stages_[index_of(stage)];
    if (current.cancelled_results == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++current.cancelled_results;
    if (current.requested_revision == revision) {
        current.state = ChunkStageState::cancelled;
    }
    return core::Status::ok();
}

void ChunkStageLedger::publish_content(std::uint64_t content_revision) noexcept {
    if (content_revision == 0) {
        std::terminate();
    }
    auto& content = stages_[index_of(ChunkStage::content)];
    content.requested_revision = content_revision;
    content.resident_request_revision = content_revision;
    content.output_revision = content_revision;
    content.state = ChunkStageState::resident;
}

std::size_t ChunkStageLedger::index_of(ChunkStage stage) noexcept {
    const auto index = static_cast<std::size_t>(stage);
    if (index >= chunk_stage_count) {
        std::terminate();
    }
    return index;
}

core::Status ChunkStageLedger::validate_transition(ChunkStage stage, std::uint64_t revision,
                                                   ChunkStageState expected) const {
    if (stage == ChunkStage::content || stage == ChunkStage::count) {
        return invalid_stage_status();
    }
    if (revision == 0) {
        return invalid_revision_status();
    }
    if (!is_current(stage, revision)) {
        return stale_ticket_status();
    }
    if (record(stage).state != expected) {
        return invalid_transition_status();
    }
    return core::Status::ok();
}

std::string_view chunk_stage_name(ChunkStage stage) noexcept {
    switch (stage) {
    case ChunkStage::content:
        return "content";
    case ChunkStage::lighting:
        return "lighting";
    case ChunkStage::mesh:
        return "mesh";
    case ChunkStage::collision:
        return "collision";
    case ChunkStage::persistence:
        return "persistence";
    case ChunkStage::replication:
        return "replication";
    case ChunkStage::count:
        return "count";
    }
    return "unknown";
}

std::string_view chunk_stage_state_name(ChunkStageState state) noexcept {
    switch (state) {
    case ChunkStageState::requested:
        return "requested";
    case ChunkStageState::running:
        return "running";
    case ChunkStageState::ready:
        return "ready";
    case ChunkStageState::resident:
        return "resident";
    case ChunkStageState::stale:
        return "stale";
    case ChunkStageState::cancelled:
        return "cancelled";
    }
    return "unknown";
}

} // namespace heartstead::world
