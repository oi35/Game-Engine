// File: CollisionSystem.cpp
// Purpose: Implements AABB collision broadphase, contact generation, and impulse-based resolution.

#include "engine/physics/CollisionSystem.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/physics/Components.h"

namespace engine::physics {

bool CollisionSystem::intersects(const math::Vec3& aCenter,
                                 const math::Vec3& aHalfExtents,
                                 const math::Vec3& bCenter,
                                 const math::Vec3& bHalfExtents,
                                 math::Vec3& outNormal,
                                 float& outPenetration) const {
    // AABB overlap test on each axis.
    const math::Vec3 delta = bCenter - aCenter;
    const float overlapX = (aHalfExtents.x + bHalfExtents.x) - std::abs(delta.x);
    const float overlapY = (aHalfExtents.y + bHalfExtents.y) - std::abs(delta.y);
    const float overlapZ = (aHalfExtents.z + bHalfExtents.z) - std::abs(delta.z);

    if (overlapX <= 0.0F || overlapY <= 0.0F || overlapZ <= 0.0F) {
        return false;
    }

    // Choose the minimum-overlap axis as contact normal.
    outPenetration = overlapX;
    outNormal = {delta.x >= 0.0F ? 1.0F : -1.0F, 0.0F, 0.0F};

    if (overlapY < outPenetration) {
        outPenetration = overlapY;
        outNormal = {0.0F, delta.y >= 0.0F ? 1.0F : -1.0F, 0.0F};
    }

    if (overlapZ < outPenetration) {
        outPenetration = overlapZ;
        outNormal = {0.0F, 0.0F, delta.z >= 0.0F ? 1.0F : -1.0F};
    }

    return true;
}

std::vector<Contact> CollisionSystem::detect(const ecs::Registry& registry) const {
    std::vector<Contact> contacts;
    // Rebuild SoA broadphase buffers each frame.
    broadphaseEntities_.clear();
    broadphaseCenterX_.clear();
    broadphaseCenterY_.clear();
    broadphaseCenterZ_.clear();
    broadphaseHalfX_.clear();
    broadphaseHalfY_.clear();
    broadphaseHalfZ_.clear();
    broadphaseMinX_.clear();
    broadphaseMaxX_.clear();
    broadphaseMinY_.clear();
    broadphaseMaxY_.clear();
    broadphaseMinZ_.clear();
    broadphaseMaxZ_.clear();
    broadphaseSortedIndices_.clear();
    broadphaseCandidatePairsA_.clear();
    broadphaseCandidatePairsB_.clear();

    registry.forEach<Transform, BoxCollider>(
        [this](ecs::Entity entity, const Transform& transform, const BoxCollider& collider) {
            broadphaseEntities_.push_back(entity);
            broadphaseCenterX_.push_back(transform.position.x);
            broadphaseCenterY_.push_back(transform.position.y);
            broadphaseCenterZ_.push_back(transform.position.z);
            broadphaseHalfX_.push_back(collider.halfExtents.x);
            broadphaseHalfY_.push_back(collider.halfExtents.y);
            broadphaseHalfZ_.push_back(collider.halfExtents.z);
            broadphaseMinX_.push_back(transform.position.x - collider.halfExtents.x);
            broadphaseMaxX_.push_back(transform.position.x + collider.halfExtents.x);
            broadphaseMinY_.push_back(transform.position.y - collider.halfExtents.y);
            broadphaseMaxY_.push_back(transform.position.y + collider.halfExtents.y);
            broadphaseMinZ_.push_back(transform.position.z - collider.halfExtents.z);
            broadphaseMaxZ_.push_back(transform.position.z + collider.halfExtents.z);
        });

    const std::size_t colliderCount = broadphaseEntities_.size();
    lastBroadphaseStats_ = {};
    lastBroadphaseStats_.colliderCount = colliderCount;
    if (colliderCount < 2) {
        return contacts;
    }

    constexpr std::size_t kBroadphaseGroupSize = 64;
    constexpr std::size_t kNarrowphaseGroupSize = 128;
    lastBroadphaseStats_.broadphaseGroupSize = kBroadphaseGroupSize;
    lastBroadphaseStats_.narrowphaseGroupSize = kNarrowphaseGroupSize;

    // Sweep-and-prune along X to prune pair candidates early.
    broadphaseSortedIndices_.resize(colliderCount);
    std::iota(broadphaseSortedIndices_.begin(), broadphaseSortedIndices_.end(), 0U);
    std::sort(broadphaseSortedIndices_.begin(),
              broadphaseSortedIndices_.end(),
              [this](std::size_t lhs, std::size_t rhs) {
                  if (broadphaseMinX_[lhs] == broadphaseMinX_[rhs]) {
                      return broadphaseMaxX_[lhs] < broadphaseMaxX_[rhs];
                  }
                  return broadphaseMinX_[lhs] < broadphaseMinX_[rhs];
              });

    const std::size_t reserveHint =
        std::min<std::size_t>(colliderCount * 4, (colliderCount * (colliderCount - 1)) / 2);
    broadphaseCandidatePairsA_.reserve(reserveHint);
    broadphaseCandidatePairsB_.reserve(reserveHint);
    contacts.reserve(reserveHint);

    for (std::size_t groupBase = 0; groupBase < colliderCount; groupBase += kBroadphaseGroupSize) {
        const std::size_t groupEnd = std::min(groupBase + kBroadphaseGroupSize, colliderCount);
        lastBroadphaseStats_.broadphaseDispatchGroups += 1;
        for (std::size_t sortedPos = groupBase; sortedPos < groupEnd; ++sortedPos) {
            const std::size_t indexA = broadphaseSortedIndices_[sortedPos];
            for (std::size_t nextPos = sortedPos + 1; nextPos < colliderCount; ++nextPos) {
                const std::size_t indexB = broadphaseSortedIndices_[nextPos];
                if (broadphaseMinX_[indexB] > broadphaseMaxX_[indexA]) {
                    break;
                }

                lastBroadphaseStats_.pairChecks += 1;
                // Additional AABB checks on Y/Z before entering narrowphase.
                const bool rejectAxis =
                    broadphaseMaxY_[indexA] < broadphaseMinY_[indexB] ||
                    broadphaseMinY_[indexA] > broadphaseMaxY_[indexB] ||
                    broadphaseMaxZ_[indexA] < broadphaseMinZ_[indexB] ||
                    broadphaseMinZ_[indexA] > broadphaseMaxZ_[indexB];
                if (rejectAxis) {
                    lastBroadphaseStats_.axisRejects += 1;
                    continue;
                }

                broadphaseCandidatePairsA_.push_back(indexA);
                broadphaseCandidatePairsB_.push_back(indexB);
            }
        }
    }

    // Narrowphase exact overlap + contact generation.
    lastBroadphaseStats_.candidatePairs = broadphaseCandidatePairsA_.size();
    for (std::size_t groupBase = 0; groupBase < broadphaseCandidatePairsA_.size(); groupBase += kNarrowphaseGroupSize) {
        const std::size_t groupEnd = std::min(groupBase + kNarrowphaseGroupSize, broadphaseCandidatePairsA_.size());
        lastBroadphaseStats_.narrowphaseDispatchGroups += 1;
        for (std::size_t pairIndex = groupBase; pairIndex < groupEnd; ++pairIndex) {
            const std::size_t indexA = broadphaseCandidatePairsA_[pairIndex];
            const std::size_t indexB = broadphaseCandidatePairsB_[pairIndex];
            math::Vec3 normal{};
            float penetration = 0.0F;
            if (!intersects({broadphaseCenterX_[indexA], broadphaseCenterY_[indexA], broadphaseCenterZ_[indexA]},
                            {broadphaseHalfX_[indexA], broadphaseHalfY_[indexA], broadphaseHalfZ_[indexA]},
                            {broadphaseCenterX_[indexB], broadphaseCenterY_[indexB], broadphaseCenterZ_[indexB]},
                            {broadphaseHalfX_[indexB], broadphaseHalfY_[indexB], broadphaseHalfZ_[indexB]},
                            normal,
                            penetration)) {
                continue;
            }

            contacts.push_back({broadphaseEntities_[indexA], broadphaseEntities_[indexB], normal, penetration});
        }
    }

    lastBroadphaseStats_.contactCount = contacts.size();
    return contacts;
}

void CollisionSystem::resolve(ecs::Registry& registry, const std::vector<Contact>& contacts) const {
    for (const Contact& contact : contacts) {
        Transform* transformA = registry.tryGet<Transform>(contact.a);
        Transform* transformB = registry.tryGet<Transform>(contact.b);
        RigidBody* bodyA = registry.tryGet<RigidBody>(contact.a);
        RigidBody* bodyB = registry.tryGet<RigidBody>(contact.b);
        if (transformA == nullptr || transformB == nullptr || bodyA == nullptr || bodyB == nullptr) {
            continue;
        }

        const float invMassA = bodyA->isStatic ? 0.0F : bodyA->inverseMass;
        const float invMassB = bodyB->isStatic ? 0.0F : bodyB->inverseMass;
        const float totalInvMass = invMassA + invMassB;
        if (totalInvMass <= 0.0F) {
            continue;
        }

        // Positional correction proportional to inverse mass.
        const math::Vec3 correction = contact.normal * (contact.penetration / totalInvMass);
        if (invMassA > 0.0F) {
            transformA->position -= correction * invMassA;
        }
        if (invMassB > 0.0F) {
            transformB->position += correction * invMassB;
        }

        const math::Vec3 relativeVelocity = bodyB->velocity - bodyA->velocity;
        const float normalSpeed = math::dot(relativeVelocity, contact.normal);
        if (normalSpeed >= 0.0F) {
            continue;
        }

        // Single normal impulse with low restitution to damp bouncing.
        const float restitution = 0.1F;
        const float impulseScalar = (-(1.0F + restitution) * normalSpeed) / totalInvMass;
        const math::Vec3 impulse = contact.normal * impulseScalar;

        if (invMassA > 0.0F) {
            bodyA->velocity -= impulse * invMassA;
        }
        if (invMassB > 0.0F) {
            bodyB->velocity += impulse * invMassB;
        }
    }
}

const BroadphaseStats& CollisionSystem::lastBroadphaseStats() const {
    return lastBroadphaseStats_;
}

}  // namespace engine::physics

