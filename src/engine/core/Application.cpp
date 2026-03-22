// File: Application.cpp
// Purpose: Implements the fixed-step main loop, simulation stepping, and renderer submission flow.

#include "engine/core/Application.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "engine/physics/Components.h"

namespace engine::core {

Application::Application(AppConfig config, std::unique_ptr<render::IRenderer> renderer)
    : config_(config), assets_(std::filesystem::current_path()), renderer_(std::move(renderer)) {
    if (!renderer_) {
        throw std::runtime_error("Application requires a renderer instance");
    }
}

void Application::run(GameLogic& game) {
    // Standard engine lifecycle.
    renderer_->initialize();
    game.onCreate(*this);

    const float fixedDelta = std::max(config_.fixedDeltaTime, 0.0F);
    const auto targetFrameDuration = std::chrono::duration<float>(fixedDelta);
    float simulationTimeSeconds = 0.0F;
    float accumulatorSeconds = 0.0F;
    constexpr std::uint32_t kMaxSimulationStepsPerFrame = 4;
    auto previousFrameTime = std::chrono::steady_clock::now();

    for (frameIndex_ = 0;; ++frameIndex_) {
        if (config_.maxFrames > 0 && frameIndex_ >= config_.maxFrames) {
            break;
        }

        const auto frameBegin = std::chrono::steady_clock::now();

        // Collect OS/window events before simulation and rendering.
        renderer_->pumpEvents();
        if (renderer_->shouldClose()) {
            break;
        }

        // Clamp frame delta to prevent simulation explosions after stalls.
        const auto now = std::chrono::steady_clock::now();
        float elapsedSeconds = std::chrono::duration<float>(now - previousFrameTime).count();
        previousFrameTime = now;
        elapsedSeconds = std::clamp(elapsedSeconds, 0.0F, 0.25F);
        accumulatorSeconds += elapsedSeconds;

        if (fixedDelta > 0.0F) {
            std::uint32_t simulationSteps = 0;
            // Fixed-step simulation with capped catch-up work per frame.
            while (accumulatorSeconds >= fixedDelta && simulationSteps < kMaxSimulationStepsPerFrame) {
                game.onUpdate(*this, fixedDelta);
                simulate(fixedDelta);
                simulationTimeSeconds += fixedDelta;
                accumulatorSeconds -= fixedDelta;
                simulationSteps += 1;
            }

            // Drop extra backlog if update work cannot keep up.
            if (simulationSteps == kMaxSimulationStepsPerFrame && accumulatorSeconds >= fixedDelta) {
                accumulatorSeconds = std::fmod(accumulatorSeconds, fixedDelta);
            }
        } else {
            game.onUpdate(*this, elapsedSeconds);
            simulate(elapsedSeconds);
            simulationTimeSeconds += elapsedSeconds;
            accumulatorSeconds = 0.0F;
        }

        // Submit renderable entities.
        renderer_->beginFrame(frameIndex_, simulationTimeSeconds);
        std::size_t drawnCount = 0;
        registry_.forEach<physics::Transform, render::RenderMesh>(
            [this, &drawnCount](ecs::Entity entity, physics::Transform& transform, render::RenderMesh& renderMesh) {
                renderer_->drawEntity(entity, transform, renderMesh);
                ++drawnCount;
            });

        // Submit debug collision geometry.
        registry_.forEach<physics::Transform, physics::BoxCollider>(
            [this](ecs::Entity entity, physics::Transform& transform, physics::BoxCollider& collider) {
                renderer_->drawDebugAabb(entity, transform, collider);
            });

        // Submit contact normals at pair midpoints.
        for (const auto& contact : contacts_) {
            const auto* transformA = registry_.tryGet<physics::Transform>(contact.a);
            const auto* transformB = registry_.tryGet<physics::Transform>(contact.b);
            if (transformA == nullptr || transformB == nullptr) {
                continue;
            }

            const math::Vec3 midpoint = (transformA->position + transformB->position) * 0.5F;
            renderer_->drawDebugContact(midpoint, contact.normal);
        }

        renderer_->endFrame(drawnCount);

        if (targetFrameDuration.count() > 0.0F) {
            // Frame limiter for predictable pacing.
            const auto frameElapsed = std::chrono::steady_clock::now() - frameBegin;
            if (frameElapsed < targetFrameDuration) {
                std::this_thread::sleep_for(targetFrameDuration - frameElapsed);
            }
        }
    }

    game.onDestroy(*this);
    renderer_->shutdown();
}

ecs::Registry& Application::registry() {
    return registry_;
}

const ecs::Registry& Application::registry() const {
    return registry_;
}

assets::AssetManager& Application::assets() {
    return assets_;
}

const assets::AssetManager& Application::assets() const {
    return assets_;
}

render::IRenderer& Application::renderer() {
    return *renderer_;
}

const render::IRenderer& Application::renderer() const {
    return *renderer_;
}

const AppConfig& Application::config() const {
    return config_;
}

std::uint32_t Application::frameIndex() const {
    return frameIndex_;
}

const std::vector<physics::Contact>& Application::contacts() const {
    return contacts_;
}

void Application::simulate(float deltaTime) {
    // Integrate motion then detect/resolve contacts.
    physicsSystem_.integrate(registry_, deltaTime, config_.gravity);
    contacts_ = collisionSystem_.detect(registry_);
    collisionSystem_.resolve(registry_, contacts_);

    if (frameIndex_ % 60 == 0) {
        const physics::BroadphaseStats& stats = collisionSystem_.lastBroadphaseStats();
        const std::size_t n = stats.colliderCount;
        const std::size_t naivePairs = (n > 1) ? ((n * (n - 1)) / 2) : 0;
        const double reductionPercent =
            naivePairs > 0
                ? (100.0 * (1.0 - (static_cast<double>(stats.pairChecks) / static_cast<double>(naivePairs))))
                : 0.0;
        std::cout << "[Broadphase] colliders=" << stats.colliderCount
                  << " pairChecks=" << stats.pairChecks
                  << " naivePairs=" << naivePairs
                  << " axisRejects=" << stats.axisRejects
                  << " candidates=" << stats.candidatePairs
                  << " contacts=" << stats.contactCount
                  << " dispatch(B/N)=" << stats.broadphaseDispatchGroups << "/" << stats.narrowphaseDispatchGroups
                  << " groupSize(B/N)=" << stats.broadphaseGroupSize << "/" << stats.narrowphaseGroupSize
                  << " reduction~" << reductionPercent << "%"
                  << '\n';
    }
}

}  // namespace engine::core

