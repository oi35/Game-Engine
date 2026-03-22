// File: NullRenderer.h
// Purpose: Declares a no-op renderer implementation for non-graphics fallback paths.

#pragma once

#include "engine/render/IRenderer.h"

namespace engine::render {

// Logging-only renderer for environments where real graphics output is unavailable.
class NullRenderer final : public IRenderer {
public:
    void initialize() override;
    void shutdown() override;
    void pumpEvents() override;
    bool shouldClose() const override;
    std::uint32_t registerMesh(const assets::MeshData& mesh) override;
    std::uint32_t registerTexture(const assets::TextureData& texture) override;
    std::uint32_t registerMaterial(const MaterialDesc& material) override;
    void beginFrame(std::uint32_t frameIndex, float simulationTimeSeconds) override;
    void drawEntity(ecs::Entity entity,
                    const physics::Transform& transform,
                    const RenderMesh& renderMesh) override;
    void drawDebugAabb(ecs::Entity entity,
                       const physics::Transform& transform,
                       const physics::BoxCollider& collider) override;
    void drawDebugContact(const math::Vec3& position, const math::Vec3& normal) override;
    void endFrame(std::size_t entityCount) override;

private:
    std::uint32_t currentFrame_ = 0;
    float currentTime_ = 0.0F;
    std::uint32_t nextMeshHandle_ = 1;
    std::uint32_t nextTextureHandle_ = 1;
    std::uint32_t nextMaterialHandle_ = 1;
};

}  // namespace engine::render

