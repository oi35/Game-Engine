// File: NullRenderer.cpp
// Purpose: Implements logging-only renderer behavior used when real graphics is unavailable.

#include "engine/render/NullRenderer.h"

#include <iostream>

namespace engine::render {

void NullRenderer::initialize() {
    std::cout << "[NullRenderer] Initialized\n";
}

void NullRenderer::shutdown() {
    std::cout << "[NullRenderer] Shutdown\n";
}

void NullRenderer::pumpEvents() {
}

bool NullRenderer::shouldClose() const {
    return false;
}

std::uint32_t NullRenderer::registerMesh(const assets::MeshData& mesh) {
    const std::uint32_t handle = nextMeshHandle_++;
    std::cout << "[NullRenderer] Registered mesh #" << handle << " (vertices=" << mesh.vertices.size()
              << ", indices=" << mesh.indices.size() << ")\n";
    return handle;
}

std::uint32_t NullRenderer::registerTexture(const assets::TextureData& texture) {
    const std::uint32_t handle = nextTextureHandle_++;
    std::cout << "[NullRenderer] Registered texture #" << handle << " (" << texture.width << "x"
              << texture.height << ")\n";
    return handle;
}

std::uint32_t NullRenderer::registerMaterial(const MaterialDesc& material) {
    const std::uint32_t handle = nextMaterialHandle_++;
    std::cout << "[NullRenderer] Registered material #" << handle << " (texture=" << material.textureHandle
              << ", baseColor=" << material.baseColor.x << ", " << material.baseColor.y << ", "
              << material.baseColor.z << ")\n";
    return handle;
}

void NullRenderer::beginFrame(std::uint32_t frameIndex, float simulationTimeSeconds) {
    // Store frame context for periodic diagnostics in endFrame.
    currentFrame_ = frameIndex;
    currentTime_ = simulationTimeSeconds;
}

void NullRenderer::drawEntity(ecs::Entity, const physics::Transform&, const RenderMesh&) {
}

void NullRenderer::drawDebugAabb(ecs::Entity, const physics::Transform&, const physics::BoxCollider&) {
}

void NullRenderer::drawDebugContact(const math::Vec3&, const math::Vec3&) {
}

void NullRenderer::endFrame(std::size_t entityCount) {
    // Emit lightweight telemetry once per second at 60Hz.
    if (currentFrame_ % 60 == 0) {
        std::cout << "[Frame " << currentFrame_ << "] t=" << currentTime_ << "s entities=" << entityCount << '\n';
    }
}

}  // namespace engine::render

