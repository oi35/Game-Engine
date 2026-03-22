// File: SampleGame.h
// Purpose: Declares the sample gameplay implementation used by the application loop.

#pragma once

#include <cstdint>

#include "engine/core/Application.h"
#include "engine/ecs/Registry.h"

namespace game {

// Small integration test scene that exercises loading, physics, collision, and rendering paths.
class SampleGame final : public engine::core::GameLogic {
public:
    // Build sample entities and bind render/physics components.
    void onCreate(engine::core::Application& app) override;
    // Apply per-frame gameplay tweaks (for example, stopping player motion after a while).
    void onUpdate(engine::core::Application& app, float deltaTime) override;
    // Emit summary stats and release transient resources.
    void onDestroy(engine::core::Application& app) override;

private:
    // Scene entities.
    engine::ecs::Entity player_ = 0;
    engine::ecs::Entity ground_ = 0;
    engine::ecs::Entity obstacle_ = 0;
    // Shared GPU asset handles.
    std::uint32_t sharedMeshHandle_ = 0;
    std::uint32_t sharedTextureHandle_ = 0;
    // Material variants used by different scene objects.
    std::uint32_t groundMaterialHandle_ = 0;
    std::uint32_t obstacleMaterialHandle_ = 0;
    std::uint32_t playerMaterialHandle_ = 0;
};

}  // namespace game

