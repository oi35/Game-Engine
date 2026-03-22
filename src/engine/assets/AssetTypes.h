// File: AssetTypes.h
// Purpose: Declares in-memory mesh and texture data structures produced by asset loading.

#pragma once

#include <cstdint>
#include <vector>

#include "engine/math/Vec3.h"

namespace engine::assets {

// Raw mesh vertex loaded from external model files.
struct MeshVertex {
    math::Vec3 position{};
    math::Vec3 normal{0.0F, 1.0F, 0.0F};
    float u = 0.0F;
    float v = 0.0F;
};

// Indexed mesh payload used for renderer registration.
struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Uncompressed RGBA texture payload.
struct TextureData {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

}  // namespace engine::assets

