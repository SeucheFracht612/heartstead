#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/save/save_metadata.hpp"
#include "engine/world/coords/world_coords.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::world {

enum class OperationStage {
    begun,
    validated,
    ids_reserved,
    mutated,
    derived_updated,
    events_emitted,
    replication_marked,
    save_marked,
    committed,
    rolled_back,
};

struct OperationEvent {
    OperationEvent() = default;
    OperationEvent(std::string event_type, core::SaveId event_subject, std::string event_message,
                   std::optional<ChunkCoord> event_routing_chunk = std::nullopt)
        : type(std::move(event_type)), subject(event_subject), message(std::move(event_message)),
          routing_chunk(event_routing_chunk) {}

    std::string type;
    core::SaveId subject;
    std::string message;
    // Owner-side routing metadata. Replication codecs intentionally omit this field after the
    // authoritative host has selected recipients, so adding a spatial scope does not revise the
    // event wire format.
    std::optional<ChunkCoord> routing_chunk;
};

[[nodiscard]] std::string_view operation_stage_name(OperationStage stage) noexcept;

class WorldOperation {
  public:
    explicit WorldOperation(std::string name);

    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] bool is_committed() const noexcept;
    [[nodiscard]] bool is_rolled_back() const noexcept;
    [[nodiscard]] bool has_failed() const noexcept;
    [[nodiscard]] const std::vector<OperationStage>& stages() const noexcept;
    [[nodiscard]] const std::vector<OperationEvent>& events() const noexcept;
    [[nodiscard]] const std::vector<core::SaveId>& reserved_ids() const noexcept;
    [[nodiscard]] const std::vector<std::string>& mutations() const noexcept;
    [[nodiscard]] const std::vector<std::string>& derived_updates() const noexcept;
    [[nodiscard]] bool replication_dirty() const noexcept;
    [[nodiscard]] bool save_dirty() const noexcept;
    [[nodiscard]] const core::Error& failure() const;

    [[nodiscard]] core::Status validate(bool condition, std::string message);
    [[nodiscard]] core::Result<core::SaveId> reserve_save_id(save::SaveIdAllocator& allocator);
    [[nodiscard]] core::Status record_mutation(std::string description);
    void record_derived_update(std::string system_name);
    void emit_event(OperationEvent event);
    void mark_replication_dirty();
    void mark_save_dirty();
    [[nodiscard]] core::Status commit();
    void rollback(std::string reason);

  private:
    void push_stage(OperationStage stage);
    [[nodiscard]] bool is_closed() const noexcept;
    [[nodiscard]] core::Status fail(std::string code, std::string message);

    std::string name_;
    std::vector<OperationStage> stages_;
    std::vector<OperationEvent> events_;
    std::vector<core::SaveId> reserved_ids_;
    std::vector<std::string> mutations_;
    std::vector<std::string> derived_updates_;
    bool replication_dirty_ = false;
    bool save_dirty_ = false;
    bool committed_ = false;
    bool rolled_back_ = false;
    std::optional<core::Error> failure_;
};

} // namespace heartstead::world
