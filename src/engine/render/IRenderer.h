// File: IRenderer.h
// Purpose: Declares the renderer abstraction used by the engine core and game code.

#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/assets/AssetTypes.h"
#include "engine/ecs/Registry.h"
#include "engine/math/Vec3.h"
#include "engine/physics/Components.h"
#include "engine/render/RenderComponents.h"

namespace engine::render {

// Rendering abstraction used by core systems and gameplay code.
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Lifetime and platform event handling.
    virtual void initialize() = 0;
    virtual void shutdown() = 0;
    virtual void pumpEvents() = 0;
    virtual bool shouldClose() const = 0;
    // Resource registration APIs.
    virtual std::uint32_t registerMesh(const assets::MeshData& mesh) = 0;
    virtual std::uint32_t registerTexture(const assets::TextureData& texture) = 0;
    virtual std::uint32_t registerMaterial(const MaterialDesc& material) = 0;
    // Per-frame submission APIs.
    virtual void beginFrame(std::uint32_t frameIndex, float simulationTimeSeconds) = 0;
    virtual void drawEntity(ecs::Entity entity,
                            const physics::Transform& transform,
                            const RenderMesh& renderMesh) = 0;
    virtual void drawDebugAabb(ecs::Entity entity,
                               const physics::Transform& transform,
                               const physics::BoxCollider& collider) = 0;
    virtual void drawDebugContact(const math::Vec3& position, const math::Vec3& normal) = 0;
    virtual void endFrame(std::size_t entityCount) = 0;
};

}  // namespace engine::render

