// File: Registry.h
// Purpose: Provides a lightweight ECS registry for entities and component storage/iteration.

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace engine::ecs {

using Entity = std::uint32_t;

// Sparse ECS registry with per-component hash-map storage.
class Registry {
public:
    // Create a new alive entity id.
    Entity createEntity() {
        const Entity entity = nextEntity_++;
        alive_.insert(entity);
        return entity;
    }

    // Destroy entity and remove all of its components.
    void destroyEntity(Entity entity) {
        alive_.erase(entity);
        for (auto& [_, storage] : storages_) {
            storage->erase(entity);
        }
    }

    // Construct component T on an entity.
    template <typename T, typename... Args>
    T& emplace(Entity entity, Args&&... args) {
        if (!isAlive(entity)) {
            throw std::runtime_error("Entity does not exist");
        }

        auto& data = ensureStorage<T>().data;
        const auto [it, inserted] = data.emplace(entity, T{std::forward<Args>(args)...});
        if (!inserted) {
            throw std::runtime_error("Component already exists on entity");
        }
        return it->second;
    }

    // Check whether entity has component T.
    template <typename T>
    bool has(Entity entity) const {
        const auto* store = tryStorage<T>();
        return store != nullptr && store->data.contains(entity);
    }

    // Get mutable component reference, throws when missing.
    template <typename T>
    T& get(Entity entity) {
        auto* store = tryStorage<T>();
        if (store == nullptr) {
            throw std::runtime_error("Component storage not found");
        }
        auto it = store->data.find(entity);
        if (it == store->data.end()) {
            throw std::runtime_error("Component not found on entity");
        }
        return it->second;
    }

    // Get const component reference, throws when missing.
    template <typename T>
    const T& get(Entity entity) const {
        const auto* store = tryStorage<T>();
        if (store == nullptr) {
            throw std::runtime_error("Component storage not found");
        }
        auto it = store->data.find(entity);
        if (it == store->data.end()) {
            throw std::runtime_error("Component not found on entity");
        }
        return it->second;
    }

    // Fast nullable component lookup.
    template <typename T>
    T* tryGet(Entity entity) {
        auto* store = tryStorage<T>();
        if (store == nullptr) {
            return nullptr;
        }
        auto it = store->data.find(entity);
        if (it == store->data.end()) {
            return nullptr;
        }
        return &it->second;
    }

    template <typename T>
    const T* tryGet(Entity entity) const {
        const auto* store = tryStorage<T>();
        if (store == nullptr) {
            return nullptr;
        }
        auto it = store->data.find(entity);
        if (it == store->data.end()) {
            return nullptr;
        }
        return &it->second;
    }

    // Iterate entities that own all requested component types (mutable view).
    template <typename... Components, typename Fn>
    void forEach(Fn&& fn) {
        static_assert(sizeof...(Components) > 0, "forEach requires at least one component");
        using First = std::tuple_element_t<0, std::tuple<Components...>>;

        auto* primary = tryStorage<First>();
        if (primary == nullptr) {
            return;
        }

        for (const auto& [entity, _] : primary->data) {
            auto components = std::tuple<Components*...>{tryGet<Components>(entity)...};
            if (((std::get<Components*>(components) != nullptr) && ...)) {
                fn(entity, (*std::get<Components*>(components))...);
            }
        }
    }

    // Iterate entities that own all requested component types (const view).
    template <typename... Components, typename Fn>
    void forEach(Fn&& fn) const {
        static_assert(sizeof...(Components) > 0, "forEach requires at least one component");
        using First = std::tuple_element_t<0, std::tuple<Components...>>;

        const auto* primary = tryStorage<First>();
        if (primary == nullptr) {
            return;
        }

        for (const auto& [entity, _] : primary->data) {
            auto components = std::tuple<const Components*...>{tryGet<Components>(entity)...};
            if (((std::get<const Components*>(components) != nullptr) && ...)) {
                fn(entity, (*std::get<const Components*>(components))...);
            }
        }
    }

private:
    // Type-erased storage base class.
    struct IStorage {
        virtual ~IStorage() = default;
        virtual void erase(Entity entity) = 0;
    };

    // Concrete storage for one component type.
    template <typename T>
    struct Storage final : IStorage {
        std::unordered_map<Entity, T> data;

        void erase(Entity entity) override {
            data.erase(entity);
        }
    };

    // Validate entity liveness.
    bool isAlive(Entity entity) const {
        return alive_.contains(entity);
    }

    // Create component storage lazily.
    template <typename T>
    Storage<T>& ensureStorage() {
        const std::type_index key = std::type_index(typeid(T));
        auto it = storages_.find(key);
        if (it == storages_.end()) {
            auto [insertedIt, _] = storages_.emplace(key, std::make_unique<Storage<T>>());
            it = insertedIt;
        }
        return *static_cast<Storage<T>*>(it->second.get());
    }

    // Nullable storage lookup.
    template <typename T>
    Storage<T>* tryStorage() {
        const std::type_index key = std::type_index(typeid(T));
        auto it = storages_.find(key);
        if (it == storages_.end()) {
            return nullptr;
        }
        return static_cast<Storage<T>*>(it->second.get());
    }

    template <typename T>
    const Storage<T>* tryStorage() const {
        const std::type_index key = std::type_index(typeid(T));
        auto it = storages_.find(key);
        if (it == storages_.end()) {
            return nullptr;
        }
        return static_cast<const Storage<T>*>(it->second.get());
    }

    Entity nextEntity_ = 1;
    std::unordered_set<Entity> alive_;
    std::unordered_map<std::type_index, std::unique_ptr<IStorage>> storages_;
};

}  // namespace engine::ecs

