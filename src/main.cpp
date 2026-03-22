// File: main.cpp
// Purpose: Entry point that creates the renderer, runs the game loop, and falls back when DX12 init fails.

#include <exception>
#include <iostream>
#include <memory>

#include "engine/core/Application.h"
#include "engine/render/NullRenderer.h"
#ifdef _WIN32
#include "engine/render/Dx12Renderer.h"
#endif
#include "game/SampleGame.h"

int main() {
    // Fixed-step simulation config shared by primary and fallback renderer paths.
    engine::core::AppConfig config{};
    config.fixedDeltaTime = 1.0F / 60.0F;
    config.maxFrames = 0;
    config.gravity = {0.0F, -9.81F, 0.0F};

    try {
#ifdef _WIN32
        // Prefer DX12 renderer on Windows.
        auto renderer = std::make_unique<engine::render::Dx12Renderer>(
            1280, 720, L"Game Engine MVP - Phase 2 (DX12)", true, true);
#else
        // Use null renderer on non-Windows builds.
        auto renderer = std::make_unique<engine::render::NullRenderer>();
#endif
        engine::core::Application app(config, std::move(renderer));
        game::SampleGame sampleGame;
        app.run(sampleGame);
    } catch (const std::exception& ex) {
#ifdef _WIN32
        // If DX12 initialization fails, keep the simulation runnable with NullRenderer.
        std::cerr << "[main] DX12 path failed: " << ex.what() << '\n';
        std::cerr << "[main] Falling back to NullRenderer.\n";

        try {
            auto fallbackRenderer = std::make_unique<engine::render::NullRenderer>();
            engine::core::Application fallbackApp(config, std::move(fallbackRenderer));
            game::SampleGame fallbackGame;
            fallbackApp.run(fallbackGame);
            return 0;
        } catch (const std::exception& fallbackEx) {
            std::cerr << "[main] Fallback renderer failed: " << fallbackEx.what() << '\n';
            return 1;
        }
#else
        std::cerr << "[main] Renderer failed: " << ex.what() << '\n';
        return 1;
#endif
    }

    return 0;
}

