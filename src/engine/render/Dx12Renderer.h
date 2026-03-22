// File: Dx12Renderer.h
// Purpose: Declares the DirectX 12 renderer front-end that fulfills the renderer interface.

#pragma once

#ifdef _WIN32

#include <memory>
#include <string>

#include "engine/render/IRenderer.h"

namespace engine::render {

// DirectX 12 renderer implementation backed by an internal pImpl state object.
class Dx12Renderer final : public IRenderer {
public:
    Dx12Renderer(std::uint32_t width,
                 std::uint32_t height,
                 std::wstring windowTitle,
                 bool vsync = true,
                 bool enableValidation = false);
    ~Dx12Renderer() override;

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
    // Keeps heavy DX12 headers and state out of the public interface.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine::render

#endif  // _WIN32

