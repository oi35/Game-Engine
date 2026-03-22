// File: CollisionSystem.h
// Purpose: Declares broadphase/narrowphase collision detection and contact resolution APIs.

#pragma once

#include <vector>

#include "engine/ecs/Registry.h"
#include "engine/math/Vec3.h"

namespace engine::physics {

// Contact manifold reduced to normal + penetration depth.
struct Contact {
    ecs::Entity a = 0;
    ecs::Entity b = 0;
    math::Vec3 normal{};
    float penetration = 0.0F;
};

// Debug/telemetry snapshot for broadphase and narrowphase work distribution.
struct BroadphaseStats {
    std::size_t colliderCount = 0;
    std::size_t pairChecks = 0;
    std::size_t axisRejects = 0;
    std::size_t candidatePairs = 0;
    std::size_t contactCount = 0;
    std::size_t broadphaseDispatchGroups = 0;
    std::size_t narrowphaseDispatchGroups = 0;
    std::size_t broadphaseGroupSize = 0;
    std::size_t narrowphaseGroupSize = 0;
};

// Broadphase + narrowphase collision pipeline for AABB colliders.
class CollisionSystem {
public:
    // Build contact list from current transforms/colliders.
    std::vector<Contact> detect(const ecs::Registry& registry) const;
    // Apply positional correction and impulse response for contacts.
    void resolve(ecs::Registry& registry, const std::vector<Contact>& contacts) const;
    // Return stats from the last detect() call.
    const BroadphaseStats& lastBroadphaseStats() const;

private:
    // Test two AABBs and produce minimum-penetration separating normal when overlapping.
    bool intersects(const math::Vec3& aCenter,
                    const math::Vec3& aHalfExtents,
                    const math::Vec3& bCenter,
                    const math::Vec3& bHalfExtents,
                    math::Vec3& outNormal,
                    float& outPenetration) const;

    mutable BroadphaseStats lastBroadphaseStats_{};
    mutable std::vector<ecs::Entity> broadphaseEntities_;
    mutable std::vector<float> broadphaseCenterX_;
    mutable std::vector<float> broadphaseCenterY_;
    mutable std::vector<float> broadphaseCenterZ_;
    mutable std::vector<float> broadphaseHalfX_;
    mutable std::vector<float> broadphaseHalfY_;
    mutable std::vector<float> broadphaseHalfZ_;
    mutable std::vector<float> broadphaseMinX_;
    mutable std::vector<float> broadphaseMaxX_;
    mutable std::vector<float> broadphaseMinY_;
    mutable std::vector<float> broadphaseMaxY_;
    mutable std::vector<float> broadphaseMinZ_;
    mutable std::vector<float> broadphaseMaxZ_;
    mutable std::vector<std::size_t> broadphaseSortedIndices_;
    mutable std::vector<std::size_t> broadphaseCandidatePairsA_;
    mutable std::vector<std::size_t> broadphaseCandidatePairsB_;
};

}  // namespace engine::physics

