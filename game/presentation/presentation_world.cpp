#include "game/presentation/presentation_world.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] bool finite_color(const std::array<float, 4>& color) noexcept {
    return std::ranges::all_of(
        color, [](float value) { return std::isfinite(value) && value >= 0.0F && value <= 1.0F; });
}

} // namespace

core::Status validate_presentation_object_update(const PresentationObjectUpdate& update) {
    const auto simulation_tick = update.source_revision == std::numeric_limits<std::uint64_t>::max()
                                     ? update.source_revision
                                     : update.source_revision - 1U;
    std::unordered_set<std::string> state_channels;
    const auto valid_states =
        update.visual_states.size() <= 32U &&
        std::ranges::all_of(update.visual_states, [&](const entities::VisualStateValue& state) {
            return !state.channel.empty() && state.channel.size() <= 64U && !state.value.empty() &&
                   state.value.size() <= 128U && state_channels.insert(state.channel).second;
        });
    if (!update.source_net_id.is_valid() || !update.visual_prototype.is_valid() ||
        !update.transform.is_finite() || !update.transform.has_non_zero_scale() ||
        !update.local_bounds.is_valid() || !finite_color(update.color) ||
        update.source_revision == 0 || !update.locomotion.validate(simulation_tick) ||
        !valid_states) {
        return core::Status::failure(
            "presentation_world.invalid_object",
            "presentation object requires valid identity, visual, transform, bounds, and revision");
    }
    return core::Status::ok();
}

core::Result<PresentationObjectId> PresentationWorld::allocate_id() {
    if (!free_slots_.empty()) {
        const auto index = free_slots_.back();
        free_slots_.pop_back();
        auto& slot = slots_[index];
        slot.occupied = true;
        return core::Result<PresentationObjectId>::success(
            PresentationObjectId::from_parts(index, slot.generation));
    }
    if (slots_.size() >= std::numeric_limits<std::uint32_t>::max()) {
        return core::Result<PresentationObjectId>::failure(
            "presentation_world.capacity_exhausted", "presentation object capacity is exhausted");
    }
    const auto index = static_cast<std::uint32_t>(slots_.size());
    slots_.push_back({});
    slots_.back().occupied = true;
    return core::Result<PresentationObjectId>::success(PresentationObjectId::from_parts(index, 1));
}

PresentationWorld::Slot* PresentationWorld::find_slot(PresentationObjectId id) noexcept {
    if (!id.is_valid() || id.index() >= slots_.size()) {
        return nullptr;
    }
    auto& slot = slots_[id.index()];
    return slot.occupied && slot.generation == id.generation() ? &slot : nullptr;
}

const PresentationWorld::Slot*
PresentationWorld::find_slot(PresentationObjectId id) const noexcept {
    if (!id.is_valid() || id.index() >= slots_.size()) {
        return nullptr;
    }
    const auto& slot = slots_[id.index()];
    return slot.occupied && slot.generation == id.generation() ? &slot : nullptr;
}

core::Result<PresentationObjectId>
PresentationWorld::upsert_object(const PresentationObjectUpdate& update) {
    auto status = validate_presentation_object_update(update);
    if (!status) {
        return core::Result<PresentationObjectId>::failure(status.error().code,
                                                           status.error().message);
    }
    const auto found = source_objects_.find(update.source_net_id.value());
    if (found != source_objects_.end()) {
        auto* slot = find_slot(found->second);
        if (slot == nullptr) {
            return core::Result<PresentationObjectId>::failure(
                "presentation_world.corrupt_source_index",
                "presentation source index references a stale object generation");
        }
        if (update.source_revision < slot->object.source_revision) {
            return core::Result<PresentationObjectId>::failure(
                "presentation_world.stale_update",
                "presentation update is older than the retained source revision");
        }
        if (update.source_revision == slot->object.source_revision) {
            return core::Result<PresentationObjectId>::success(slot->object.id);
        }
        slot->object.previous_transform =
            update.teleport ? update.transform : slot->object.current_transform;
        slot->object.current_transform = update.transform;
        slot->object.previous_locomotion =
            update.teleport ? update.locomotion : slot->object.current_locomotion;
        slot->object.current_locomotion = update.locomotion;
        slot->object.visual_prototype = update.visual_prototype;
        slot->object.local_bounds = update.local_bounds;
        slot->object.color = update.color;
        slot->object.visual_states = update.visual_states;
        slot->object.source_revision = update.source_revision;
        slot->object.visible = update.visible;
        slot->object.teleported = update.teleport;
        ++revision_;
        return core::Result<PresentationObjectId>::success(slot->object.id);
    }

    auto allocated = allocate_id();
    if (!allocated) {
        return allocated;
    }
    auto* slot = find_slot(allocated.value());
    slot->object.id = allocated.value();
    slot->object.source_net_id = update.source_net_id;
    slot->object.visual_prototype = update.visual_prototype;
    slot->object.previous_transform = update.transform;
    slot->object.current_transform = update.transform;
    slot->object.previous_locomotion = update.locomotion;
    slot->object.current_locomotion = update.locomotion;
    slot->object.local_bounds = update.local_bounds;
    slot->object.color = update.color;
    slot->object.visual_states = update.visual_states;
    slot->object.source_revision = update.source_revision;
    slot->object.visible = update.visible;
    slot->object.teleported = update.teleport;
    source_objects_.emplace(update.source_net_id.value(), allocated.value());
    ++revision_;
    return allocated;
}

core::Status PresentationWorld::remove_object(core::NetId source_net_id) {
    const auto found = source_objects_.find(source_net_id.value());
    if (found == source_objects_.end()) {
        return core::Status::failure("presentation_world.unknown_source",
                                     "presentation removal references an unknown source object");
    }
    auto* slot = find_slot(found->second);
    if (slot == nullptr) {
        return core::Status::failure("presentation_world.corrupt_source_index",
                                     "presentation source index references a stale object");
    }
    const auto index = found->second.index();
    source_objects_.erase(found);
    slot->occupied = false;
    slot->object = {};
    ++slot->generation;
    if (slot->generation == 0) {
        std::terminate();
    }
    free_slots_.push_back(index);
    ++revision_;
    return core::Status::ok();
}

const RenderObjectSnapshot*
PresentationWorld::find_object(core::NetId source_net_id) const noexcept {
    const auto found = source_objects_.find(source_net_id.value());
    if (found == source_objects_.end()) {
        return nullptr;
    }
    const auto* slot = find_slot(found->second);
    return slot == nullptr ? nullptr : &slot->object;
}

RenderSnapshot PresentationWorld::extract(std::uint64_t simulation_tick) const {
    RenderSnapshot snapshot;
    snapshot.simulation_tick = simulation_tick;
    snapshot.presentation_revision = revision_;
    snapshot.objects.reserve(source_objects_.size());
    for (const auto& slot : slots_) {
        if (slot.occupied) {
            snapshot.objects.push_back(slot.object);
        }
    }
    return snapshot;
}

PresentationWorldStats PresentationWorld::stats() const noexcept {
    return {static_cast<std::uint32_t>(source_objects_.size()), revision_};
}

void PresentationWorld::clear() noexcept {
    source_objects_.clear();
    free_slots_.clear();
    free_slots_.reserve(slots_.size());
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        auto& slot = slots_[index];
        slot.occupied = false;
        slot.object = {};
        ++slot.generation;
        if (slot.generation == 0) {
            std::terminate();
        }
        free_slots_.push_back(index);
    }
    ++revision_;
}

} // namespace heartstead::game
