cmake_minimum_required(VERSION 3.20)
project(GameMVP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(GameMVP
    src/main.cpp
    src/engine/assets/AssetManager.cpp
    src/engine/core/Application.cpp
    src/engine/physics/PhysicsSystem.cpp
    src/engine/physics/CollisionSystem.cpp
    src/engine/render/NullRenderer.cpp
    src/game/SampleGame.cpp
)

target_include_directories(GameMVP PRIVATE src)

if(WIN32)
    target_sources(GameMVP PRIVATE
        src/engine/render/Dx12Renderer.cpp
        src/engine/render/dx12/Dx12RendererBudget.cpp
        src/engine/render/dx12/Dx12RendererSetup.cpp
        src/engine/render/dx12/Dx12RendererResources.cpp
        src/engine/render/dx12/Dx12RendererCulling.cpp
        src/engine/render/dx12/Dx12RendererHud.cpp
    )
    target_compile_definitions(GameMVP PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(GameMVP PRIVATE d3d12 dxgi dxguid d3dcompiler user32 gdi32)
endif()

if(MSVC)
    target_compile_options(GameMVP PRIVATE /W4 /permissive-)
else()
    target_compile_options(GameMVP PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(BudgetProfileCLI
    src/tools/BudgetProfileCli.cpp
)

if(MSVC)
    target_compile_options(BudgetProfileCLI PRIVATE /W4 /permissive-)
else()
    target_compile_options(BudgetProfileCLI PRIVATE -Wall -Wextra -Wpedantic)
endif()
