#pragma once

#include "combat.hpp"
#include "behaviours/RagdollBehaviour.hpp"
#include "physics/CollisionObjectNode.hpp"

namespace r1 {

// Game-side fall/recovery policy shared by the player and passers-by. Physics,
// collider ownership and pose transfer belong to Saida's RagdollBehaviour.
class Knockdown {
public:
    void bind(saida::RagdollBehaviour& ragdoll, saida::CollisionObjectNode& animatedBody,
              double lieSeconds) {
        ragdoll_ = &ragdoll;
        animatedBody_ = &animatedBody;
        lieSeconds_ = lieSeconds;
        resting_ = 0;
    }

    saida::RagdollBehaviour* ragdoll() const { return ragdoll_; }

    bool activate(const glm::vec3& velocity = {}, const glm::vec3& spin = {}) {
        if (!ragdoll_ || !animatedBody_ || !ragdoll_->activateReplacing(*animatedBody_, velocity, spin))
            return false;
        resting_ = 0;
        return true;
    }

    // True only when recovery begins: callers then resume their locomotion
    // state at the engine's recovered position. Dead actors keep their pose.
    template<class Ground>
    bool update(double dt, bool canRecover, Ground&& ground,
                const saida::Transform* parentRest = nullptr) {
        if (!ragdoll_) return false;
        using State = saida::RagdollBehaviour::State;
        if (ragdoll_->state() == State::Simulated &&
            ragdoll_->simulatedSeconds() >= combat::kLongestTumble) ragdoll_->freeze();
        if (ragdoll_->state() != State::Frozen || !canRecover) return false;
        resting_ += std::max(0.0, dt);
        if (resting_ < lieSeconds_) return false;
        const float height = ground(glm::vec3(ragdoll_->node()->worldTransform()[3]));
        if (parentRest) return ragdoll_->blendOutToParent(float(combat::kStandSeconds), *parentRest, height);
        ragdoll_->blendOut(float(combat::kStandSeconds), height);
        return ragdoll_->state() == State::Blending;
    }

    void release() {
        if (ragdoll_) ragdoll_->release();
        resting_ = 0;
    }

private:
    saida::RagdollBehaviour* ragdoll_ = nullptr;
    saida::CollisionObjectNode* animatedBody_ = nullptr;
    double lieSeconds_ = 0, resting_ = 0;
};

} // namespace r1
