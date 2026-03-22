// File: PhysicsSystem.h
// Purpose: Declares the integration step for updating rigid-body motion.

#pragma once

#include "engine/ecs/Registry.h"
#include "engine/math/Vec3.h"

namespace engine::physics {

// Integrates dynamic rigid bodies using explicit Euler.
class PhysicsSystem {
public:
    void integrate(ecs::Registry& registry, float deltaTime, const math::Vec3& gravity) const;
};

}  // namespace engine::physics

