#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/math/vector.hpp"
#include "engine/world/coords/world_position.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace heartstead::entities {

using Vec3 = math::Vec3d;
using Transform = world::WorldTransform;

enum class EntityKind {
    player,
    creature,
    animal,
    cart,
    boat,
    dropped_item,
    projectile,
    temporary_physics,
    machine,
    container,
};

struct EntityRecord {
    core::RuntimeHandle runtime_handle;
    core::NetId net_id;
    core::SaveId save_id;
    core::PrototypeId prototype_id;
    EntityKind kind = EntityKind::temporary_physics;
    Transform transform;
    std::string encoded_state;
    bool persistent = false;
    bool sleeping = false;

    [[nodiscard]] core::Status validate() const;
};

struct EntityDefinition {
    core::PrototypeId prototype_id;
    EntityKind kind = EntityKind::temporary_physics;
    bool persistent = false;
    std::uint64_t carry_capacity_grams = 0;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] core::Result<EntityRecord> create_record(core::RuntimeHandle runtime_handle,
                                                           core::NetId net_id,
                                                           core::SaveId save_id = {},
                                                           Transform transform = {}) const;
};

class RuntimeHandleAllocator {
  public:
    explicit RuntimeHandleAllocator(std::uint64_t next_value = 1);

    [[nodiscard]] core::Result<core::RuntimeHandle> reserve();
    [[nodiscard]] core::RuntimeHandle peek_next() const noexcept;

  private:
    std::uint64_t next_value_ = 1;
};

class EntityNetIdAllocator {
  public:
    explicit EntityNetIdAllocator(std::uint64_t next_value = 1'000'000);

    [[nodiscard]] core::Result<core::NetId> reserve();
    [[nodiscard]] core::NetId peek_next() const noexcept;

  private:
    std::uint64_t next_value_ = 1'000'000;
};

[[nodiscard]] core::Result<EntityKind> entity_kind_from_name(std::string_view name);
[[nodiscard]] bool is_known_entity_kind(EntityKind kind) noexcept;
[[nodiscard]] std::string_view entity_kind_name(EntityKind kind) noexcept;

} // namespace heartstead::entities
