#pragma once

#include "engine/animation/locomotion_animation.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/entities/entity_id.hpp"
#include "engine/world/coords/world_position.hpp"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heartstead::simulation {
class TickEvents;
}

namespace heartstead::entities {

enum class EntityLifecycle : std::uint8_t {
    created,
    active,
    sleeping,
    pending_destruction,
};

struct TransformComponent {
    world::WorldTransform previous;
    world::WorldTransform current;
};

struct CharacterComponent {
    core::NetId owner;
    float movement_speed = 4.5F;
};

struct NetworkIdentityComponent {
    core::NetId net_id;
};

struct LocomotionAnimationComponent {
    animation::ReplicatedLocomotionAnimation state;
};

struct HealthComponent {
    float current = 100.0F;
    float maximum = 100.0F;
};

struct PhysicsBodyComponent {
    std::uint64_t body_id = 0;
};

struct VisualComponent {
    core::PrototypeId mesh_asset;
    core::PrototypeId material_asset;
};

struct EntityWorldRecord {
    EntityId id;
    core::PrototypeId prototype;
    EntityLifecycle lifecycle = EntityLifecycle::created;
    std::uint64_t revision = 1;
};

struct EntityTombstone {
    EntityId id;
    std::uint64_t revision = 0;
    std::uint64_t destroyed_tick = 0;
};

class IComponentStore {
  public:
    virtual ~IComponentStore() = default;
    virtual void erase(EntityId id) noexcept = 0;
    virtual void clear() noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

template <typename Component> class ComponentStore final : public IComponentStore {
  public:
    template <typename... Args>
    [[nodiscard]] core::Result<Component*> emplace(EntityId id, Args&&... args) {
        if (!id.is_valid()) {
            return core::Result<Component*>::failure("component_store.invalid_entity",
                                                     "component entity id must be valid");
        }
        ensure_sparse(id.index());
        if (contains(id)) {
            return core::Result<Component*>::failure("component_store.duplicate_component",
                                                     "entity already owns this component type");
        }
        dense_entities_.push_back(id);
        dense_components_.emplace_back(std::forward<Args>(args)...);
        sparse_[id.index()] = static_cast<std::uint32_t>(dense_entities_.size());
        return core::Result<Component*>::success(&dense_components_.back());
    }

    [[nodiscard]] bool contains(EntityId id) const noexcept {
        if (!id.is_valid() || id.index() >= sparse_.size()) {
            return false;
        }
        const auto dense_plus_one = sparse_[id.index()];
        return dense_plus_one != 0 && dense_entities_[dense_plus_one - 1U] == id;
    }

    [[nodiscard]] Component* find(EntityId id) noexcept {
        if (!contains(id)) {
            return nullptr;
        }
        return &dense_components_[sparse_[id.index()] - 1U];
    }

    [[nodiscard]] const Component* find(EntityId id) const noexcept {
        if (!contains(id)) {
            return nullptr;
        }
        return &dense_components_[sparse_[id.index()] - 1U];
    }

    void erase(EntityId id) noexcept override {
        if (!contains(id)) {
            return;
        }
        const auto dense_index = static_cast<std::size_t>(sparse_[id.index()] - 1U);
        const auto last_index = dense_entities_.size() - 1U;
        if (dense_index != last_index) {
            dense_entities_[dense_index] = dense_entities_[last_index];
            dense_components_[dense_index] = std::move(dense_components_[last_index]);
            sparse_[dense_entities_[dense_index].index()] =
                static_cast<std::uint32_t>(dense_index + 1U);
        }
        dense_entities_.pop_back();
        dense_components_.pop_back();
        sparse_[id.index()] = 0;
    }

    void clear() noexcept override {
        sparse_.clear();
        dense_entities_.clear();
        dense_components_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return dense_entities_.size();
    }

    [[nodiscard]] std::span<const EntityId> entities() const noexcept {
        return dense_entities_;
    }

    [[nodiscard]] std::span<Component> components() noexcept {
        return dense_components_;
    }

    [[nodiscard]] std::span<const Component> components() const noexcept {
        return dense_components_;
    }

  private:
    void ensure_sparse(std::uint32_t index) {
        if (index >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(index) + 1U, 0);
        }
    }

    std::vector<std::uint32_t> sparse_;
    std::vector<EntityId> dense_entities_;
    std::vector<Component> dense_components_;
};

using EntityCleanupCallback = std::function<core::Status(EntityId, const EntityWorldRecord&)>;

struct EntityWorldStats {
    std::size_t live_entities = 0;
    std::size_t active_entities = 0;
    std::size_t sleeping_entities = 0;
    std::size_t pending_destruction = 0;
    std::size_t component_type_count = 0;
    std::size_t component_count = 0;
    std::size_t tombstone_count = 0;
};

class EntityWorld {
  public:
    [[nodiscard]] core::Result<EntityId> create_entity(core::PrototypeId prototype);
    [[nodiscard]] core::Status activate_entity(EntityId id,
                                               simulation::TickEvents* events = nullptr);
    [[nodiscard]] core::Status deactivate_entity(EntityId id);
    [[nodiscard]] core::Status destroy_entity(EntityId id);
    [[nodiscard]] core::Result<std::size_t>
    finalize_destruction(std::uint64_t tick, simulation::TickEvents* events = nullptr);

    [[nodiscard]] bool is_alive(EntityId id) const noexcept;
    [[nodiscard]] EntityWorldRecord* find(EntityId id) noexcept;
    [[nodiscard]] const EntityWorldRecord* find(EntityId id) const noexcept;
    [[nodiscard]] std::vector<EntityWorldRecord> records() const;

    template <typename Component, typename... Args>
    [[nodiscard]] core::Result<Component*> emplace(EntityId id, Args&&... args) {
        const auto* record = find(id);
        if (record == nullptr || record->lifecycle == EntityLifecycle::pending_destruction) {
            return core::Result<Component*>::failure(
                "entity_world.entity_not_mutable",
                "components require a live entity that is not pending destruction");
        }
        return component_store<Component>().emplace(id, std::forward<Args>(args)...);
    }

    template <typename Component> [[nodiscard]] Component* find_component(EntityId id) noexcept {
        const auto found = component_stores_.find(std::type_index(typeid(Component)));
        if (found == component_stores_.end()) {
            return nullptr;
        }
        return static_cast<ComponentStore<Component>*>(found->second.get())->find(id);
    }

    template <typename Component>
    [[nodiscard]] const Component* find_component(EntityId id) const noexcept {
        const auto found = component_stores_.find(std::type_index(typeid(Component)));
        if (found == component_stores_.end()) {
            return nullptr;
        }
        return static_cast<const ComponentStore<Component>*>(found->second.get())->find(id);
    }

    template <typename Component> [[nodiscard]] ComponentStore<Component>& component_store() {
        const auto key = std::type_index(typeid(Component));
        auto found = component_stores_.find(key);
        if (found == component_stores_.end()) {
            found =
                component_stores_.emplace(key, std::make_unique<ComponentStore<Component>>()).first;
        }
        return *static_cast<ComponentStore<Component>*>(found->second.get());
    }

    [[nodiscard]] core::Status register_cleanup(std::string name, EntityCleanupCallback callback);
    [[nodiscard]] std::span<const EntityTombstone> tombstones() const noexcept;
    void clear_tombstones() noexcept;
    [[nodiscard]] EntityWorldStats stats() const noexcept;
    void clear() noexcept;

  private:
    struct Slot {
        EntityWorldRecord record;
        std::uint32_t generation = 1;
        bool occupied = false;
    };

    struct CleanupRegistration {
        std::string name;
        EntityCleanupCallback callback;
    };

    [[nodiscard]] Slot* slot(EntityId id) noexcept;
    [[nodiscard]] const Slot* slot(EntityId id) const noexcept;
    [[nodiscard]] core::Result<EntityId> allocate_id();

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<std::type_index, std::unique_ptr<IComponentStore>> component_stores_;
    std::vector<CleanupRegistration> cleanup_callbacks_;
    std::vector<EntityTombstone> tombstones_;
    std::size_t live_count_ = 0;
    std::uint64_t next_revision_ = 1;
};

[[nodiscard]] std::string_view entity_lifecycle_name(EntityLifecycle lifecycle) noexcept;

} // namespace heartstead::entities
