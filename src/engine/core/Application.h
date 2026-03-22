// File: Application.h
// Purpose: Declares the application orchestrator, configuration, and game-logic lifecycle interface.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/assets/AssetManager.h"
#include "engine/ecs/Registry.h"
#include "engine/math/Vec3.h"
#include "engine/physics/CollisionSystem.h"
#include "engine/physics/PhysicsSystem.h"
#include "engine/render/IRenderer.h"

namespace engine::core {

// Runtime configuration for the main loop and simulation behavior.
struct AppConfig {
    float fixedDeltaTime = 1.0F / 60.0F;
    // 0 means "run until renderer requests close".
    std::uint32_t maxFrames = 0;
    math::Vec3 gravity{0.0F, -9.81F, 0.0F};
};

class Application;

// Game callback interface implemented by title-specific gameplay code.
class GameLogic {
public:
    virtual ~GameLogic() = default;
    virtual void onCreate(Application& app) = 0;
    virtual void onUpdate(Application& app, float deltaTime) = 0;
    virtual void onDestroy(Application& app) = 0;
};

// Owns engine subsystems and drives the update/render loop.
class Application {
public:
    explicit Application(AppConfig config, std::unique_ptr<render::IRenderer> renderer);

    // Execute full lifecycle: initialize, update/render loop, shutdown.
    void run(GameLogic& game);

    // Subsystem accessors for gameplay code.
    ecs::Registry& registry();
    const ecs::Registry& registry() const;
    assets::AssetManager& assets();
    const assets::AssetManager& assets() const;
    render::IRenderer& renderer();
    const render::IRenderer& renderer() const;
    const AppConfig& config() const;
    std::uint32_t frameIndex() const;
    const std::vector<physics::Contact>& contacts() const;

private:
    // Advance one simulation step and resolve physics/collisions.
    void simulate(float deltaTime);

    AppConfig config_;
    ecs::Registry registry_;
    physics::PhysicsSystem physicsSystem_;
    physics::CollisionSystem collisionSystem_;
    std::vector<physics::Contact> contacts_;
    assets::AssetManager assets_;
    std::unique_ptr<render::IRenderer> renderer_;
    std::uint32_t frameIndex_ = 0;
};

}  // namespace engine::core

