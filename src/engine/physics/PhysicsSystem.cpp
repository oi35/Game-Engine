// File: PhysicsSystem.cpp
// Purpose: Integrates velocity and position for dynamic bodies each simulation tick.

#include "engine/physics/PhysicsSystem.h"

#include "engine/physics/Components.h"

namespace engine::physics {

void PhysicsSystem::integrate(ecs::Registry& registry, float deltaTime, const math::Vec3& gravity) const {
    // Iterate all dynamic physics bodies and advance state using explicit Euler integration.
    registry.forEach<Transform, RigidBody>([deltaTime, &gravity](ecs::Entity, Transform& transform, RigidBody& body) {
        if (body.isStatic) {
            return;
        }

        if (body.useGravity) {
            body.velocity += gravity * deltaTime;
        }

        transform.position += body.velocity * deltaTime;
    });
}

}  // namespace engine::physics

