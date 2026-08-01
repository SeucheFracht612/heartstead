#include "engine/entities/entity.hpp"

#include <string>
#include <utility>

namespace heartstead::entities {

core::Status EntityDefinition::validate() const {
    if (!prototype_id.is_valid()) {
        return core::Status::failure("entity_definition.invalid_prototype",
                                     "entity definition prototype id must be valid");
    }
    if (!is_known_entity_kind(kind)) {
        return core::Status::failure("entity_definition.invalid_kind",
                                     "entity definition kind is unknown");
    }
    if (kind == EntityKind::player && carry_capacity_grams == 0) {
        return core::Status::failure("entity_definition.missing_carry_capacity",
                                     "player definitions need a positive carry capacity");
    }
    return core::Status::ok();
}

core::Result<EntityRecord> EntityDefinition::create_record(core::RuntimeHandle runtime_handle,
                                                           core::NetId net_id, core::SaveId save_id,
                                                           Transform transform) const {
    auto status = validate();
    if (!status) {
        return core::Result<EntityRecord>::failure(status.error().code, status.error().message);
    }

    EntityRecord record;
    record.runtime_handle = runtime_handle;
    record.net_id = net_id;
    record.save_id = save_id;
    record.prototype_id = prototype_id;
    record.kind = kind;
    record.transform = transform;
    record.persistent = persistent;

    status = record.validate();
    if (!status) {
        return core::Result<EntityRecord>::failure(status.error().code, status.error().message);
    }
    return core::Result<EntityRecord>::success(std::move(record));
}

core::Status EntityRecord::validate() const {
    if (!runtime_handle.is_valid()) {
        return core::Status::failure("entity.invalid_runtime_handle",
                                     "entity runtime handle must be valid");
    }
    if (!net_id.is_valid()) {
        return core::Status::failure("entity.invalid_net_id", "entity net id must be valid");
    }
    if (!prototype_id.is_valid()) {
        return core::Status::failure("entity.invalid_prototype",
                                     "entity prototype id must be valid");
    }
    if (!is_known_entity_kind(kind)) {
        return core::Status::failure("entity.invalid_kind", "entity kind is unknown");
    }
    if (!transform.is_finite()) {
        return core::Status::failure("entity.invalid_transform",
                                     "entity transform must contain finite values");
    }
    if (!transform.has_non_zero_scale()) {
        return core::Status::failure("entity.invalid_transform_scale",
                                     "entity transform scale must be non-zero");
    }
    if (persistent && !save_id.is_valid()) {
        return core::Status::failure("entity.missing_save_id",
                                     "persistent entities need a stable save id");
    }
    if (!persistent && save_id.is_valid()) {
        return core::Status::failure("entity.unexpected_save_id",
                                     "non-persistent entities must not claim a save id");
    }

    return core::Status::ok();
}

RuntimeHandleAllocator::RuntimeHandleAllocator(std::uint64_t next_value)
    : next_value_(next_value == 0 ? 1 : next_value) {}

core::Result<core::RuntimeHandle> RuntimeHandleAllocator::reserve() {
    if (next_value_ == 0) {
        return core::Result<core::RuntimeHandle>::failure(
            "entity.runtime_handle_exhausted", "runtime handle allocator exhausted its id range");
    }

    const auto handle = core::RuntimeHandle::from_value(next_value_);
    ++next_value_;
    return core::Result<core::RuntimeHandle>::success(handle);
}

core::RuntimeHandle RuntimeHandleAllocator::peek_next() const noexcept {
    return core::RuntimeHandle::from_value(next_value_);
}

EntityNetIdAllocator::EntityNetIdAllocator(std::uint64_t next_value)
    : next_value_(next_value == 0 ? 1'000'000 : next_value) {}

core::Result<core::NetId> EntityNetIdAllocator::reserve() {
    if (next_value_ == 0) {
        return core::Result<core::NetId>::failure("entity.net_id_exhausted",
                                                  "entity net id allocator exhausted its id range");
    }

    const auto id = core::NetId::from_value(next_value_);
    ++next_value_;
    return core::Result<core::NetId>::success(id);
}

core::NetId EntityNetIdAllocator::peek_next() const noexcept {
    return core::NetId::from_value(next_value_);
}

core::Result<EntityKind> entity_kind_from_name(std::string_view name) {
    if (name == "player") {
        return core::Result<EntityKind>::success(EntityKind::player);
    }
    if (name == "creature") {
        return core::Result<EntityKind>::success(EntityKind::creature);
    }
    if (name == "animal") {
        return core::Result<EntityKind>::success(EntityKind::animal);
    }
    if (name == "cart") {
        return core::Result<EntityKind>::success(EntityKind::cart);
    }
    if (name == "boat") {
        return core::Result<EntityKind>::success(EntityKind::boat);
    }
    if (name == "dropped_item") {
        return core::Result<EntityKind>::success(EntityKind::dropped_item);
    }
    if (name == "projectile") {
        return core::Result<EntityKind>::success(EntityKind::projectile);
    }
    if (name == "machine") {
        return core::Result<EntityKind>::success(EntityKind::machine);
    }
    if (name == "container") {
        return core::Result<EntityKind>::success(EntityKind::container);
    }
    if (name == "temporary_physics") {
        return core::Result<EntityKind>::success(EntityKind::temporary_physics);
    }
    return core::Result<EntityKind>::failure("entity.invalid_kind",
                                             "unsupported entity kind: " + std::string(name));
}

bool is_known_entity_kind(EntityKind kind) noexcept {
    switch (kind) {
    case EntityKind::player:
    case EntityKind::creature:
    case EntityKind::animal:
    case EntityKind::cart:
    case EntityKind::boat:
    case EntityKind::dropped_item:
    case EntityKind::projectile:
    case EntityKind::machine:
    case EntityKind::container:
    case EntityKind::temporary_physics:
        return true;
    }
    return false;
}

std::string_view entity_kind_name(EntityKind kind) noexcept {
    switch (kind) {
    case EntityKind::player:
        return "player";
    case EntityKind::creature:
        return "creature";
    case EntityKind::animal:
        return "animal";
    case EntityKind::cart:
        return "cart";
    case EntityKind::boat:
        return "boat";
    case EntityKind::dropped_item:
        return "dropped_item";
    case EntityKind::projectile:
        return "projectile";
    case EntityKind::machine:
        return "machine";
    case EntityKind::container:
        return "container";
    case EntityKind::temporary_physics:
        return "temporary_physics";
    }
    return "unknown";
}

} // namespace heartstead::entities
