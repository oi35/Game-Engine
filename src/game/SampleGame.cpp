// File: SampleGame.cpp
// Purpose: Builds a small demo scene and drives per-frame game behavior for testing the engine stack.

#include "game/SampleGame.h"

#include <iostream>
#include <stdexcept>

#include "engine/physics/Components.h"
#include "engine/render/RenderComponents.h"

namespace game {

void SampleGame::onCreate(engine::core::Application& app) {
    auto& registry = app.registry();

    try {
        // Load one mesh/texture pair and derive multiple material variants from it.
        const auto mesh = app.assets().loadMeshObj("assets/models/cube.obj");
        const auto texture = app.assets().loadTexturePpm("assets/textures/checker.ppm");
        sharedMeshHandle_ = app.renderer().registerMesh(*mesh);
        sharedTextureHandle_ = app.renderer().registerTexture(*texture);

        groundMaterialHandle_ = app.renderer().registerMaterial(
            engine::render::MaterialDesc{sharedTextureHandle_, {0.45F, 0.55F, 0.60F}});
        obstacleMaterialHandle_ = app.renderer().registerMaterial(
            engine::render::MaterialDesc{sharedTextureHandle_, {0.95F, 0.55F, 0.25F}});
        playerMaterialHandle_ = app.renderer().registerMaterial(
            engine::render::MaterialDesc{sharedTextureHandle_, {0.30F, 0.80F, 1.00F}});

        std::cout << "[SampleGame] Loaded mesh vertices=" << mesh->vertices.size()
                  << " indices=" << mesh->indices.size()
                  << " texture=" << texture->width << "x" << texture->height
                  << " meshHandle=" << sharedMeshHandle_
                  << " textureHandle=" << sharedTextureHandle_
                  << " materials=(" << groundMaterialHandle_ << ", "
                  << obstacleMaterialHandle_ << ", " << playerMaterialHandle_ << ")\n";
    } catch (const std::exception& ex) {
        // Keep scene creation resilient: renderer defaults are used when asset loading fails.
        sharedMeshHandle_ = 0;
        sharedTextureHandle_ = 0;
        groundMaterialHandle_ = 0;
        obstacleMaterialHandle_ = 0;
        playerMaterialHandle_ = 0;
        std::cout << "[SampleGame] Asset load failed, using renderer defaults. reason=" << ex.what() << '\n';
    }

    // Ground: static collider + render mesh.
    ground_ = registry.createEntity();
    registry.emplace<engine::physics::Transform>(ground_, engine::physics::Transform{{0.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 1.0F}});
    registry.emplace<engine::physics::BoxCollider>(ground_, engine::physics::BoxCollider{{20.0F, 1.0F, 20.0F}});
    registry.emplace<engine::physics::RigidBody>(ground_, engine::physics::RigidBody{{0.0F, 0.0F, 0.0F}, 0.0F, false, true});
    registry.emplace<engine::render::RenderMesh>(
        ground_, engine::render::RenderMesh{sharedMeshHandle_, groundMaterialHandle_});

    // Obstacle: additional static blocker.
    obstacle_ = registry.createEntity();
    registry.emplace<engine::physics::Transform>(obstacle_, engine::physics::Transform{{2.5F, 0.5F, 0.0F}, {1.0F, 1.0F, 1.0F}});
    registry.emplace<engine::physics::BoxCollider>(obstacle_, engine::physics::BoxCollider{{0.5F, 0.5F, 0.5F}});
    registry.emplace<engine::physics::RigidBody>(obstacle_, engine::physics::RigidBody{{0.0F, 0.0F, 0.0F}, 0.0F, false, true});
    registry.emplace<engine::render::RenderMesh>(
        obstacle_, engine::render::RenderMesh{sharedMeshHandle_, obstacleMaterialHandle_});

    // Player: dynamic body affected by gravity.
    player_ = registry.createEntity();
    registry.emplace<engine::physics::Transform>(player_, engine::physics::Transform{{0.0F, 5.0F, 0.0F}, {1.0F, 1.0F, 1.0F}});
    registry.emplace<engine::physics::BoxCollider>(player_, engine::physics::BoxCollider{{0.5F, 0.5F, 0.5F}});
    registry.emplace<engine::physics::RigidBody>(player_, engine::physics::RigidBody{{1.25F, 0.0F, 0.0F}, 1.0F, true, false});
    registry.emplace<engine::render::RenderMesh>(
        player_, engine::render::RenderMesh{sharedMeshHandle_, playerMaterialHandle_});

    std::cout << "[SampleGame] Scene created with ground, obstacle, and player.\n";
}

void SampleGame::onUpdate(engine::core::Application& app, float deltaTime) {
    (void)deltaTime;

    auto& registry = app.registry();
    auto& playerBody = registry.get<engine::physics::RigidBody>(player_);

    // Freeze horizontal motion after warmup to make collision outcomes deterministic.
    if (app.frameIndex() > 180) {
        playerBody.velocity.x = 0.0F;
    }
}

void SampleGame::onDestroy(engine::core::Application& app) {
    app.assets().releaseUnused();

    const auto& registry = app.registry();
    const auto& playerTransform = registry.get<engine::physics::Transform>(player_);
    const auto& playerBody = registry.get<engine::physics::RigidBody>(player_);

    std::cout << "[SampleGame] Player final position: ("
              << playerTransform.position.x << ", "
              << playerTransform.position.y << ", "
              << playerTransform.position.z << ")\n";
    std::cout << "[SampleGame] Player final velocity: ("
              << playerBody.velocity.x << ", "
              << playerBody.velocity.y << ", "
              << playerBody.velocity.z << ")\n";
    std::cout << "[SampleGame] Asset cache sizes after release meshes=" << app.assets().meshCacheSize()
              << " textures=" << app.assets().textureCacheSize() << '\n';
}

}  // namespace game

