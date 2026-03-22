// File: Components.h
// Purpose: Defines physics-related ECS components such as transforms, rigid bodies, and colliders.

#pragma once

#include "engine/math/Vec3.h"

namespace engine::physics {

// World-space position and scale.
struct Transform {
    math::Vec3 position{};
    math::Vec3 scale{1.0F, 1.0F, 1.0F};
};

// Simple rigid-body properties for linear motion.
struct RigidBody {
    math::Vec3 velocity{};
    float inverseMass = 1.0F;
    bool useGravity = true;
    bool isStatic = false;
};

// Axis-aligned box collider represented by half-extents.
struct BoxCollider {
    math::Vec3 halfExtents{0.5F, 0.5F, 0.5F};
};

}  // namespace engine::physics

