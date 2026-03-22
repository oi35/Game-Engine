// File: RenderComponents.h
// Purpose: Defines render-facing ECS component payloads and material descriptors.

#pragma once

#include <cstdint>

#include "engine/math/Vec3.h"

namespace engine::render {

// Material registration payload.
struct MaterialDesc {
    std::uint32_t textureHandle = 0;
    math::Vec3 baseColor{1.0F, 1.0F, 1.0F};
};

// ECS render component that binds mesh and material handles.
struct RenderMesh {
    std::uint32_t meshHandle = 0;
    std::uint32_t materialHandle = 0;
};

}  // namespace engine::render

