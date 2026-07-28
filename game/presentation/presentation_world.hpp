#pragma once

#include "engine/core/result.hpp"
#include "game/presentation/render_snapshot.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace heartstead::game {

struct PresentationObjectUpdate {
    core::NetId source_net_id;
    core::PrototypeId visual_prototype;
    world::WorldTransform transform;
    animation::ReplicatedLocomotionAnimation locomotion;
    math::Bounds3f local_bounds{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::uint64_t source_revision = 0;
    bool visible = true;
    bool teleport = false;
};

struct PresentationWorldStats {
    std::uint32_t retained_object_count = 0;
    std::uint64_t presentation_revision = 0;
};

class PresentationWorld final {
  public:
    [[nodiscard]] core::Result<PresentationObjectId>
    upsert_object(const PresentationObjectUpdate& update);
    [[nodiscard]] core::Status remove_object(core::NetId source_net_id);
    [[nodiscard]] const RenderObjectSnapshot* find_object(core::NetId source_net_id) const noexcept;
    [[nodiscard]] RenderSnapshot extract(std::uint64_t simulation_tick) const;
    [[nodiscard]] PresentationWorldStats stats() const noexcept;
    void clear() noexcept;

  private:
    struct Slot {
        std::uint32_t generation = 1;
        bool occupied = false;
        RenderObjectSnapshot object;
    };

    [[nodiscard]] core::Result<PresentationObjectId> allocate_id();
    [[nodiscard]] Slot* find_slot(PresentationObjectId id) noexcept;
    [[nodiscard]] const Slot* find_slot(PresentationObjectId id) const noexcept;

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<std::uint64_t, PresentationObjectId> source_objects_;
    std::uint64_t revision_ = 0;
};

[[nodiscard]] core::Status
validate_presentation_object_update(const PresentationObjectUpdate& update);

} // namespace heartstead::game
