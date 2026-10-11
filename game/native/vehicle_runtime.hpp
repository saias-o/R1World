#pragma once

#include "behaviours/SurfaceDriveBehaviour.hpp"
#include "behaviours/DeformableMeshBehaviour.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "scene/Node.hpp"
#include <algorithm>
#include <array>
#include <string>

namespace r1 {

struct VehicleDimensions { float length, width, height, wheelbase, wheelRadius; };
struct VehicleDynamics {
    float mass, acceleration, braking, grip, maximumSpeed, damageEnergy;
};
inline VehicleDynamics vehicleDynamics(const std::string& kind) {
    struct Entry { const char* kind; VehicleDynamics dynamics; };
    static constexpr std::array<Entry,7> profiles{{
        {"city",{1100,8.5f,12,10,28,65000}}, {"sedan",{1500,8,12,10,32,85000}},
        {"suv",{1900,7,11,9,30,105000}}, {"offroad",{2100,7,10,9,27,115000}},
        {"sport",{1350,11,14,12,42,75000}}, {"truck",{9000,3.5f,7,6,25,300000}},
        {"bus",{12000,3,7,6,25,400000}}
    }};
    for(const auto& profile:profiles)if(kind==profile.kind)return profile.dynamics;
    return profiles.front().dynamics;
}

// The geographic entity owns a solver body containing its render hierarchy.
// Absorbing the body's local motion into the entity keeps graph/geographic
// bookkeeping separate from the engine's physics without changing its world pose.
class VehicleRuntime {
public:
    static constexpr const char* kBodyName = "Vehicle collider";
    static constexpr float kPromotionDistance = 90.f;
    static constexpr float kDemotionDistance = 125.f;
    static constexpr float kPredictionTime = 2.f;
    static constexpr float kDisablingDamageFraction = .35f;
    static constexpr float kSkinFriction = .001f;
    static constexpr float kSteeringDamageBias = .18f;
    static constexpr float kPowerDamageStart = .5f;
    static constexpr float kMaximumBodyworkPowerLoss = .35f;
    static constexpr float kPoweredSlopeAssist = 1.f;
    static constexpr float kGroundStability = 28.f;  // was 16 — stronger roll-restoring couple
    static constexpr float kGroundDamping = 14.f;    // was 8  — bleeds roll rate faster
    static constexpr float kInertiaMultiplier = 3.5f; // was 2.2 — heavier, harder to spin over
    static constexpr float kBodyClearance = .06f;
    static constexpr float kDemotionVerticalSpeed = .5f;
    static constexpr float kDemotionAngularSpeed = .35f;
    static constexpr float kWheelContactHeightFraction=.5f,kWheelContactNormalUp=.7f,kWheelContactEnergyFraction=.1f;
    static constexpr int kChassisSections = 3;
    static constexpr int kFrontChassisIndex = 0; // Forward is local -Z.
    static saida::RigidBodyNode* body(saida::Node& root) {
        return dynamic_cast<saida::RigidBodyNode*>(root.findByPath(kBodyName));
    }
    static saida::SurfaceDriveBehaviour* driver(saida::Node& root) {
        auto* rigid=body(root);return rigid?rigid->getBehaviour<saida::SurfaceDriveBehaviour>():nullptr;
    }
    static bool physical(saida::Node& root) { auto* rigid=body(root);return rigid&&!rigid->kinematic&&rigid->physicsWorld(); }
    static glm::vec3 exitOffset(VehicleDimensions size,glm::quat rotation,glm::vec3 direction) {
        const auto half=glm::vec3(size.width,size.height,size.length)*.5f;
        const auto axes=glm::mat3_cast(rotation);
        const float extent=std::abs(glm::dot(direction,axes[0]))*half.x+
            std::abs(glm::dot(direction,axes[1]))*half.y+std::abs(glm::dot(direction,axes[2]))*half.z;
        return rotation*glm::vec3(0,half.y,0)+direction*(extent+.9f);
    }
    static bool stableForTraffic(saida::Node& root) {
        auto* rigid=body(root);auto* drive=driver(root);
        if(!physical(root)||!drive||!drive->grounded())return false;
        auto& world=*rigid->physicsWorld();
        glm::vec3 position;glm::quat rotation;
        world.getBodyTransform(rigid->bodyId(),position,rotation);
        const auto up=rotation*glm::vec3(0,1,0);
        return glm::dot(up,glm::vec3(0,1,0))>.7f &&
            std::abs(glm::dot(world.linearVelocity(rigid->bodyId()),up))<kDemotionVerticalSpeed &&
            glm::length(world.angularVelocity(rigid->bodyId()))<kDemotionAngularSpeed;
    }
    static float bodyworkPowerLoss(float damageFraction) {
        // Cosmetic impact energy is not an engine-failure model. Minor dents
        // retain full power; severe body damage has a bounded handling penalty.
        return std::clamp((damageFraction-kPowerDamageStart)/(1.f-kPowerDamageStart),0.f,1.f)*kMaximumBodyworkPowerLoss;
    }
    static void install(saida::Node& root, VehicleDimensions dimensions, const std::string& kind) {
        if(body(root))return;
        if(auto* legacy=root.findByPath(kBodyName))root.removeChild(legacy);
        auto rigid=std::make_unique<saida::RigidBodyNode>();rigid->setName(kBodyName);
        const auto profile=vehicleDynamics(kind);
        rigid->mass=profile.mass;rigid->kinematic=true;
        rigid->inertiaMultiplier=kInertiaMultiplier;
        rigid->linearDamping=0.f; // SurfaceDrive owns rolling/aerodynamic resistance.
        rigid->friction=kSkinFriction;rigid->continuousCollision=true;rigid->reportsImpacts=true;
        constexpr float kMassHeightFraction = .25f;  // was .15 — lower CoM, harder to tip over
        rigid->centerOfMass={0,-dimensions.height*kMassHeightFraction,0};
        auto* drive=rigid->addBehaviour<saida::SurfaceDriveBehaviour>();
        drive->acceleration=profile.acceleration;drive->braking=profile.braking;
        drive->slopeAssist=kPoweredSlopeAssist;
        drive->groundStability=kGroundStability;drive->groundDamping=kGroundDamping;
        drive->selfRightAcceleration=16.f;
        drive->lateralAcceleration=profile.grip;drive->maxSpeed=profile.maximumSpeed;
        drive->wheelbase=dimensions.wheelbase;drive->groundReach=dimensions.height*.5f+kBodyClearance;
        while(!root.children().empty())rigid->addChild(root.detachChild(root.children().front().get()));
        for(int section=0;section<kChassisSections;++section) {
            auto* shape=rigid->createChild<saida::CollisionShapeNode>();
            shape->setName("Chassis "+std::to_string(section));shape->shapeType=saida::CollisionShapeType::Box;
            const float clearance=dimensions.wheelRadius;
            const float chassisHeight=dimensions.height-clearance;
            shape->halfExtents={dimensions.width*.5f,chassisHeight*.5f,dimensions.length/(2.f*kChassisSections)};
            shape->offset={0,clearance+chassisHeight*.5f,(section-1)*dimensions.length/kChassisSections};
        }
        const float halfTrack=std::max(dimensions.wheelRadius,dimensions.width*.5f-dimensions.wheelRadius);
        for(float axle:{-1.f,1.f})for(float side:{-1.f,1.f}) {
            auto* wheel=rigid->createChild<saida::CollisionShapeNode>();
            wheel->setName("Wheel contact "+std::to_string(int(axle))+" "+std::to_string(int(side)));
            wheel->shapeType=saida::CollisionShapeType::Sphere;wheel->radius=dimensions.wheelRadius;
            wheel->offset={side*halfTrack,dimensions.wheelRadius,axle*dimensions.wheelbase*.5f};
        }
        auto* deformation=rigid->addBehaviour<saida::DeformableMeshBehaviour>();
        deformation->excludedPrefixes="wheel-,glass-,window-";
        deformation->impactRadius=std::max(dimensions.width*.6f,dimensions.length*.15f);
        deformation->maximumDisplacement=dimensions.width*.25f;
        deformation->maximumImpactEnergy=profile.damageEnergy;
        deformation->stiffness=profile.damageEnergy/(dimensions.width*dimensions.width);
        const float wheelContactHeight=dimensions.wheelRadius*kWheelContactHeightFraction;
        const float wheelContactEnergy=profile.damageEnergy*kWheelContactEnergyFraction;
        deformation->setContactFilter([wheelContactHeight,wheelContactEnergy](glm::vec3 point,glm::vec3 direction,float energy) {
            const bool wheelContact=point.y<wheelContactHeight&&direction.y>kWheelContactNormalUp;
            return !wheelContact||energy>=wheelContactEnergy;
        });
        root.addChild(std::move(rigid));
    }
    static void synchronize(saida::Node& root) {
        auto* rigid=body(root);
        if(!rigid||rigid->kinematic)return;
        auto& entity=root.transform();auto& local=rigid->transform();
        entity.position+=entity.rotation*(entity.scale*local.position);
        entity.rotation=glm::normalize(entity.rotation*local.rotation);
        local.position=glm::vec3(0);local.rotation=glm::quat(1,0,0,0);
        if(auto* deformation=rigid->getBehaviour<saida::DeformableMeshBehaviour>()) {
            auto* drive=driver(root);
            drive->damage=bodyworkPowerLoss(deformation->absorbedEnergy()/std::max(1.f,deformation->maximumImpactEnergy));
            if(auto* front=dynamic_cast<saida::CollisionShapeNode*>(rigid->findByPath("Chassis "+std::to_string(kFrontChassisIndex)))) {
                const auto left=deformation->displacementAt(front->offset-glm::vec3(front->halfExtents.x,0,0));
                const auto right=deformation->displacementAt(front->offset+glm::vec3(front->halfExtents.x,0,0));
                drive->steeringBias=std::clamp((glm::length(left)-glm::length(right))/
                    std::max(.01f,deformation->maximumDisplacement),-1.f,1.f)*kSteeringDamageBias;
            }
        }
    }
    static void activate(saida::Node& root, saida::PhysicsWorld& world, glm::vec3 velocity) {
        auto* rigid=body(root);if(!rigid||!rigid->kinematic)return;
        root.updateTransforms(root.parent()?root.parent()->worldTransform():glm::mat4(1.f),true);
        rigid->kinematic=false;rigid->markDirty();rigid->syncToPhysics(world);
        rigid->setLinearVelocity(velocity);
    }
    static void reset(saida::Node& root) {
        synchronize(root);
        auto* rigid=body(root);if(!rigid)return;
        rigid->kinematic=true;rigid->markDirty();
        auto* drive=driver(root);drive->damage=drive->steeringBias=0;drive->parked=false;drive->command(0,0,0);
        if(auto* deformation=rigid->getBehaviour<saida::DeformableMeshBehaviour>())deformation->resetDeformation();
    }
    static void deactivate(saida::Node& root) {
        synchronize(root);
        if(auto* rigid=body(root)){rigid->kinematic=true;rigid->markDirty();}
    }
};
} // namespace r1
