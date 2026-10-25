// What a fight needs from the engine that is not the world's own: the
// Rocketbox body as a ragdoll and its arms as reaching limbs, the pistol the
// project draws, the flash and puffs of a shot, and its sounds. The rules of
// the fight are native/combat.hpp; the world's glue is native/world.cpp.
#pragma once

#include "audio/AudioManager.hpp"
#include "behaviours/BoneAttachmentBehaviour.hpp"
#include "behaviours/RagdollBehaviour.hpp"
#include "core/Log.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "scene/animation/PoseModifiers.hpp"
#include "scene/animation/Rig.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipNode.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>

namespace r1 {

// ── the body ────────────────────────────────────────────────────────────────

// The 3ds Max biped every Rocketbox scan is rigged on (r1/humans.py).
inline saida::HumanoidBones rocketboxBones() {
    saida::HumanoidBones b;
    b.pelvis = "Bip01 Pelvis"; b.spine = "Bip01 Spine1"; b.chest = "Bip01 Spine2";
    b.neck = "Bip01 Neck"; b.head = "Bip01 Head";
    b.leftUpperArm = "Bip01 L UpperArm"; b.leftForearm = "Bip01 L Forearm"; b.leftHand = "Bip01 L Hand";
    b.rightUpperArm = "Bip01 R UpperArm"; b.rightForearm = "Bip01 R Forearm"; b.rightHand = "Bip01 R Hand";
    b.leftThigh = "Bip01 L Thigh"; b.leftShin = "Bip01 L Calf"; b.leftFoot = "Bip01 L Foot";
    b.rightThigh = "Bip01 R Thigh"; b.rightShin = "Bip01 R Calf"; b.rightFoot = "Bip01 R Foot";
    return b;
}

// An adult of 72 kg, in the rig's metres, facing +Z. The scans are bound in
// an A-pose -- arms 44 degrees below the shoulder line, measured on the bind
// matrices -- so the shoulders' cones lean less than a T-pose's would to let
// an arm hang at the side without reaching through the chest.
inline saida::RagdollProfile rocketboxRagdoll() {
    saida::RagdollProfile profile = saida::humanoidRagdoll(rocketboxBones(), 72.f, {0.f, 0.f, 1.f}, .058f);
    for (auto& part : profile.parts)
        if (part.bone.find("UpperArm") != std::string::npos) { part.swingTilt = .45f; part.swing = 1.f; }
    return profile;
}

inline saida::TwoBoneIKModifier::Settings rocketboxArm(bool right) {
    return right ? saida::TwoBoneIKModifier::Settings{"Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand", 22.f}
                 : saida::TwoBoneIKModifier::Settings{"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", 22.f};
}

inline saida::GazeModifier::Settings rocketboxRidingLean() {
    saida::GazeModifier::Settings settings;
    settings.chain={{"Bip01 Spine",.5f,.26f},{"Bip01 Spine1",.65f,.24f},{"Bip01 Spine2",1.f,.20f}};
    settings.maxYaw=0.f;settings.maxPitchDown=.9f;settings.maxPitchUp=.1f;settings.response=12.f;
    return settings;
}
inline saida::GazeModifier::Settings rocketboxRidingLook() {
    saida::GazeModifier::Settings settings;
    // Rocketbox clavicles descend from Neck: rotating Neck to look at a
    // target would also pull both shoulders away from their hand contacts.
    settings.chain={{"Bip01 Head",1.f,1.1f}};
    settings.maxYaw=1.5f;settings.maxPitchDown=.35f;settings.maxPitchUp=.35f;settings.response=12.f;
    return settings;
}
// This authored Rocketbox correction turns only the chest about world up.
// A gaze correction on the whole spine would also undo the forward riding
// lean, lifting the shoulder beyond the handlebar arm's reach.
class RocketboxRidingAim final : public saida::PoseModifier {
public:
    explicit RocketboxRidingAim(const saida::Rig& rig) {
        const char* names[]={"Bip01 Spine","Bip01 Spine1","Bip01 Spine2"};
        for(int i=0;i<3;++i)bones_[i]=rig.findBoneIndex(names[i]);
        shoulder_=rig.findBoneIndex("Bip01 L UpperArm");elbow_=rig.findBoneIndex("Bip01 L Forearm");hand_=rig.findBoneIndex("Bip01 L Hand");
    }
    void keepLeftHandAt(glm::vec3 wrist){support_=wrist;}
    void lookAt(glm::vec3 direction) {
        float yaw=std::atan2(direction.x,direction.z);
        if(std::abs(yaw)<2.8f)side_=yaw<0?-1.f:1.f;
        else yaw=side_*std::abs(yaw);
        wanted_=std::clamp(yaw,-.95f,.95f);
    }
    void release(){wanted_=0.f;}
    void update(float dt) override {
        angle_+=(wanted_-angle_)*(1.f-std::exp(-14.f*std::max(0.f,dt)));
        if(std::abs(angle_)<.0001f&&wanted_==0)angle_=0;
    }
    bool active()const override{return std::abs(angle_)>.0001f||wanted_!=0;}
    void apply(saida::LocalPose& pose,const saida::Rig& rig)const override {
        auto global=[&](const saida::LocalPose& local,int bone){
            glm::mat4 matrix(1);
            for(int b=bone;b>=0;b=rig.bones()[size_t(b)].parentIndex)matrix=local.localTransforms[size_t(b)].matrix()*matrix;
            return matrix;
        };
        auto rotation=[](glm::mat4 matrix){
            glm::mat3 basis(matrix);for(int i=0;i<3;++i)basis[i]=glm::normalize(basis[i]);
            return glm::normalize(glm::quat_cast(basis));
        };
        constexpr float shares[]={.05f,.15f,.8f};
        auto twist=[&](float amount,saida::LocalPose& candidate) {
            candidate=pose;
            for(int i=0;i<3;++i)if(bones_[i]>=0) {
                const int bone=bones_[i],parent=rig.bones()[size_t(bone)].parentIndex;
                const auto above=parent>=0?rotation(global(candidate,parent)):glm::quat(1,0,0,0);
                const auto turn=glm::angleAxis(angle_*amount*shares[i],glm::vec3(0,1,0));
                candidate.localTransforms[size_t(bone)].rotation=glm::normalize(glm::inverse(above)*turn*rotation(global(candidate,bone)));
            }
            if(shoulder_<0||elbow_<0||hand_<0)return true;
            const auto shoulder=glm::vec3(global(candidate,shoulder_)[3]),elbow=glm::vec3(global(candidate,elbow_)[3]),
                hand=glm::vec3(global(candidate,hand_)[3]);
            return glm::distance(shoulder,support_)<.98f*(glm::distance(shoulder,elbow)+glm::distance(elbow,hand));
        };
        saida::LocalPose candidate;
        if(!twist(1.f,candidate)) {
            // Only turn as far as the arm holding the bar permits. The firing
            // arm and head can continue toward a target behind the rider.
            float low=0,high=1;
            for(int i=0;i<9;++i){const float mid=(low+high)*.5f;if(twist(mid,candidate))low=mid;else high=mid;}
            twist(low,candidate);
        }
        pose=std::move(candidate);
    }
private:
    int bones_[3]{},shoulder_=-1,elbow_=-1,hand_=-1;
    glm::vec3 support_{0};float wanted_=0,angle_=0,side_=1;
};
// Put the palm on the rubber grip, rather than putting the wrist there and
// leaving the fingers floating beyond it. All measurements come from this rig.
inline saida::Transform rocketboxBarGrip(const saida::Rig& rig,bool right) {
    const std::string prefix=right?"Bip01 R ":"Bip01 L ";
    const int hand=rig.findBoneIndex(prefix+"Hand"),thumb=rig.findBoneIndex(prefix+"Finger0"),
        middle=rig.findBoneIndex(prefix+"Finger2");
    saida::Transform result;
    if(hand<0||thumb<0||middle<0)return result;
    auto bind=[&](int bone){return glm::inverse(rig.bones()[size_t(bone)].inverseBindMatrix);};
    const auto h=bind(hand);const glm::vec3 wrist(h[3]),knuckles(bind(middle)[3]);
    const auto bore=glm::normalize(knuckles-wrist);
    auto top=glm::vec3(bind(thumb)[3])-wrist;top=glm::normalize(top-bore*glm::dot(top,bore));
    const auto side=glm::cross(top,bore);
    const float palmSign=side.x*-wrist.x>0.f?1.f:-1.f;
    result.position=glm::vec3(glm::inverse(h)*glm::vec4(wrist+(knuckles-wrist)*.6f+side*palmSign*.012f,1.f));
    const glm::vec3 forward=glm::normalize(glm::vec3(0,-.35f,1)),palm(0,-forward.z,forward.y);
    glm::mat3 source(side,top,bore),target(palm*palmSign,glm::cross(forward,palm*palmSign),forward);
    result.rotation=glm::normalize(glm::quat_cast(target*glm::transpose(source)*glm::mat3(h)));
    return result;
}
class RocketboxRidingHands final : public saida::PoseModifier {
public:
    explicit RocketboxRidingHands(const saida::Rig& rig) {
        auto bind=[&](int b){return glm::inverse(rig.bones()[size_t(b)].inverseBindMatrix);};
        for(int h=0;h<2;++h) {
            const std::string prefix=h?"Bip01 R ":"Bip01 L ";
            const int hand=rig.findBoneIndex(prefix+"Hand"),thumb=rig.findBoneIndex(prefix+"Finger0"),middle=rig.findBoneIndex(prefix+"Finger2");
            if(hand<0||thumb<0||middle<0)continue;
            const auto wrist=glm::vec3(bind(hand)[3]),forward=glm::normalize(glm::vec3(bind(middle)[3])-wrist);
            auto top=glm::vec3(bind(thumb)[3])-wrist;top=glm::normalize(top-forward*glm::dot(top,forward));
            auto palm=glm::cross(top,forward);if(palm.x*-wrist.x<0)palm=-palm;
            for(int finger=0;finger<5;++finger)for(int joint=0;joint<3;++joint) {
                const std::string root=prefix+"Finger"+std::to_string(finger);
                const int bone=rig.findBoneIndex(root+(joint?std::to_string(joint):""));
                if(bone<0)continue;
                const int neighbour=joint<2?rig.findBoneIndex(root+std::to_string(joint+1)):rig.bones()[size_t(bone)].parentIndex;
                if(neighbour<0)continue;
                const auto direction=glm::normalize((glm::vec3(bind(neighbour)[3])-glm::vec3(bind(bone)[3]))*(joint<2?1.f:-1.f));
                auto axis=glm::cross(direction,palm);if(glm::length(axis)<1e-5f)continue;
                const auto rotation=glm::normalize(glm::quat_cast(glm::mat3(bind(bone))));
                axis=glm::inverse(rotation)*glm::normalize(axis);
                constexpr float curl[]={.65f,1.15f,.7f};
                links_[h].push_back({bone,glm::normalize(rig.bones()[size_t(bone)].restLocal.rotation*
                    glm::angleAxis(curl[joint]*(finger==0?.65f:1.f),axis))});
            }
        }
    }
    void hold(bool on){wanted_=on?1.f:0.f;}
    void update(float dt)override{weight_+=(wanted_-weight_)*(1.f-std::exp(-22.f*std::max(0.f,dt)));}
    bool active()const override{return weight_>.001f||wanted_>0;}
    void apply(saida::LocalPose& pose,const saida::Rig&)const override {
        for(const auto& hand:links_)for(const auto& link:hand) {
            auto& rotation=pose.localTransforms[size_t(link.bone)].rotation;
            rotation=glm::normalize(glm::slerp(rotation,link.rotation,weight_));
        }
    }
private:
    struct Link{int bone;glm::quat rotation;};
    std::vector<Link> links_[2];float weight_=0,wanted_=0;
};
inline glm::quat rocketboxFootRotation(const saida::Rig& rig,bool right) {
    const int index=rig.findBoneIndex(right?"Bip01 R Foot":"Bip01 L Foot");
    if(index<0)return glm::quat(1,0,0,0);
    return glm::normalize(glm::quat_cast(glm::mat3(glm::inverse(rig.bones()[size_t(index)].inverseBindMatrix))));
}

// The bind-pose length of an arm, shoulder to wrist, in the rig's metres.
inline float armLength(const saida::Rig& rig, bool right) {
    const char* names[3] = {right ? "Bip01 R UpperArm" : "Bip01 L UpperArm", right ? "Bip01 R Forearm" : "Bip01 L Forearm",
                            right ? "Bip01 R Hand" : "Bip01 L Hand"};
    glm::vec3 p[3];
    for (int i = 0; i < 3; ++i) {
        const int b = rig.findBoneIndex(names[i]);
        if (b < 0) return .55f;
        p[i] = glm::vec3(glm::inverse(rig.bones()[size_t(b)].inverseBindMatrix)[3]);
    }
    return glm::length(p[1] - p[0]) + glm::length(p[2] - p[1]);
}

inline glm::vec3 rocketboxRidingSeat(const saida::Rig& rig,bool cross,float vehicleScale,float scale,float drop) {
    // Shorter adult rigs sit nearer the tank instead of stretching their
    // wrists beyond their reach. This is measured from the bones, not sex.
    const float nearer=std::clamp(.56f-armLength(rig,true),0.f,.12f)*scale*1.6f;
    return {0,(cross?.97f:.82f)*vehicleScale-drop,(cross?.19f:.16f)*vehicleScale-nearer};
}
inline void holdRocketboxRidingBase(saida::Animator& animator) {
    // Sample the chair clip once for seated hips. Its 36-second fidgets are
    // inappropriate on a bike and can lift the shoulders out of bar reach.
    // Modifiers keep updating: steering, aim, hand contacts and road banking
    // remain live while only this source playhead is held.
    if(auto* seat=animator.activeClipNode()){seat->setTime(3.f);seat->setPlaybackSpeed(0.f);}
}

// ── the pistol ──────────────────────────────────────────────────────────────

// Its frame: +Z along the bore, +Y the slide's top, origin in the middle of
// the grip where the palm closes. A compact 9 mm service pistol, 186 mm long
// and 30 mm wide, drawn by the project: no CC0 scan of a firearm exists among
// its photographed sources, and none is downloaded for it.
constexpr glm::vec3 kPistolMuzzle{0.f, .046f, .14f};

namespace detail {
struct PistolBuilder {
    saida::MeshData data;
    // A box of `size` centred at `centre`, turned `pitch` radians about X.
    void box(glm::vec3 centre, glm::vec3 size, float pitch = 0.f) {
        const glm::quat turn = glm::angleAxis(pitch, glm::vec3(1, 0, 0));
        const glm::vec3 h = size * .5f;
        const glm::vec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 3; ++a)
            for (int sign = -1; sign <= 1; sign += 2) {
                const glm::vec3 n = axes[a] * float(sign);
                const glm::vec3 u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
                const uint32_t base = uint32_t(data.vertices.size());
                const float su = h[(a + 1) % 3], sv = h[(a + 2) % 3];
                const glm::vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                for (const auto& c : corners) {
                    saida::Vertex vertex{};
                    vertex.pos = centre + turn * (n * h[a] + u * (c.x * su) + v * (c.y * sv));
                    vertex.normal = turn * n;
                    vertex.color = glm::vec3(1.f);
                    vertex.texCoord = vertex.lightmapUV = glm::vec2(c.x * .5f + .5f, c.y * .5f + .5f);
                    vertex.tangent = glm::vec4(turn * u, 1.f);
                    data.vertices.push_back(vertex);
                }
                // Counter-clockwise seen from outside.
                const bool flip = glm::dot(glm::cross(u, v), n) < 0.f;
                const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
                const uint32_t reversed[6] = {0, 2, 1, 0, 3, 2};
                for (uint32_t k : flip ? reversed : order) data.indices.push_back(base + k);
            }
    }
};
}  // namespace detail

inline std::unique_ptr<saida::Node> buildPistol(saida::ResourceManager& resources) {
    // Two finishes, both black: a nitrided steel slide with a little sheen,
    // a matte polymer frame. Albedo 0.04-0.05, the charcoal end of the scale.
    detail::PistolBuilder steel, polymer;
    steel.box({0.f, .046f, .045f}, {.025f, .029f, .186f});      // slide
    steel.box({0.f, .061f, -.040f}, {.017f, .006f, .007f});     // rear sight
    steel.box({0.f, .061f, .128f}, {.004f, .006f, .006f});      // front sight
    steel.box({0.f, .046f, .1385f}, {.012f, .012f, .003f});     // muzzle crown
    polymer.box({0.f, .022f, .078f}, {.023f, .018f, .118f});    // dust cover
    polymer.box({0.f, -.024f, -.012f}, {.028f, .098f, .049f}, -.33f);  // grip, raked 19 degrees
    polymer.box({0.f, -.073f, -.029f}, {.031f, .009f, .054f}, -.33f);  // magazine base
    polymer.box({0.f, -.006f, .051f}, {.011f, .005f, .052f});   // trigger guard, bottom
    polymer.box({0.f, .006f, .075f}, {.011f, .028f, .005f});    // trigger guard, front
    polymer.box({0.f, .004f, .040f}, {.006f, .018f, .005f});    // trigger
    auto root = std::make_unique<saida::Node>("pistol");
    saida::MaterialDesc slide;
    slide.baseColor = {.05f, .05f, .052f, 1.f};
    slide.metallic = .55f;
    slide.roughness = .36f;
    saida::MaterialDesc frame;
    frame.baseColor = {.038f, .038f, .036f, 1.f};
    frame.roughness = .68f;
    for (auto* part : {&steel, &polymer}) {
        auto* mesh = resources.getMesh(resources.queueMemoryMesh(saida::prepareMesh(std::move(part->data))));
        if (!mesh) { saida::Log::error("[World combat] the geometry arena refused the pistol"); return nullptr; }
        auto node = std::make_unique<saida::MeshNode>(part == &steel ? "pistol-slide" : "pistol-frame", mesh,
                                                      resources.getMaterial(part == &steel ? slide : frame));
        node->castShadows() = false;  // 18 cm in a hand: its shadow is the hand's
        root->addChild(std::move(node));
    }
    return root;
}

// Where the pistol sits in the right hand, measured on the rig's bind pose:
// the bore along the hand toward the middle knuckles, the slide's top on the
// thumb side, the grip in the palm between the wrist and the knuckles.
inline bool pistolGrip(const saida::Rig& rig, saida::Transform& offset, bool right=true) {
    const std::string prefix=right?"Bip01 R ":"Bip01 L ";
    const int hand = rig.findBoneIndex(prefix+"Hand"), thumb = rig.findBoneIndex(prefix+"Finger0"),
              middle = rig.findBoneIndex(prefix+"Finger2");
    if (hand < 0 || thumb < 0 || middle < 0) return false;
    auto bind = [&](int b) { return glm::inverse(rig.bones()[size_t(b)].inverseBindMatrix); };
    const glm::mat4 h = bind(hand);
    const glm::vec3 wrist(h[3]), thumbBase(bind(thumb)[3]), knuckles(bind(middle)[3]);
    const glm::vec3 bore = glm::normalize(knuckles - wrist);
    glm::vec3 top = thumbBase - wrist;
    top = glm::normalize(top - bore * glm::dot(top, bore));
    glm::vec3 side = glm::cross(top, bore);
    // The palm faces the body's midline in the A-pose.
    const glm::vec3 palm = side.x * -wrist.x > 0.f ? side : -side;
    glm::mat4 gun(1.f);
    gun[0] = glm::vec4(side, 0.f);
    gun[1] = glm::vec4(top, 0.f);
    gun[2] = glm::vec4(bore, 0.f);
    gun[3] = glm::vec4(wrist + (knuckles - wrist) * .6f + palm * .012f, 1.f);
    const glm::mat4 local = glm::inverse(h) * gun;
    offset.position = glm::vec3(local[3]);
    offset.rotation = glm::normalize(glm::quat_cast(glm::mat3(local)));
    offset.scale = glm::vec3(1.f);
    return true;
}

// ── flashes and puffs ───────────────────────────────────────────────────────

// Pooled emitters moved to where they burst: a burst's particles live in world
// space from the moment they are born, so one emitter of each kind serves
// every shot. The engine's cone emits about world up, which is what a puff off
// a pavement wants; off a wall it still rises (engine ROADMAP).
class CombatEffects {
public:
    enum Kind { Flash, Smoke, Dust, Sparks, Blood, Kinds };

    void build(saida::Node& world) {
        auto make = [&](Kind kind) {
            auto* e = world.createChild<saida::ParticleSystemNode>();
            e->setName(std::string("combat-") + names()[kind]);
            e->looping = false;
            e->playing = false;  // burst() starts it
            e->spawnRate = 0.f;
            e->shape = saida::ParticleSystemNode::Shape::Cone;
            switch (kind) {
                case Flash:
                    e->blendMode = saida::ParticleSystemNode::BlendMode::Additive;
                    e->maxParticles = 32; e->burstCount = 7; e->lifetime = .055f; e->startSize = .075f;
                    e->endSizeScale = 1.7f; e->startSpeed = 1.6f; e->coneAngle = 25.f; e->emissive = 7.f;
                    e->startColor = {1.f, .78f, .42f, 1.f}; e->endColor = {1.f, .38f, .08f, 0.f};
                    e->gravity = glm::vec3(0.f); e->radius = .01f;
                    break;
                case Smoke:
                    e->blendMode = saida::ParticleSystemNode::BlendMode::Alpha;
                    e->maxParticles = 48; e->burstCount = 6; e->lifetime = 1.1f; e->startSize = .05f;
                    e->endSizeScale = 5.f; e->startSpeed = .35f; e->coneAngle = 40.f; e->emissive = 0.f;
                    e->startColor = {.32f, .32f, .31f, .32f}; e->endColor = {.36f, .36f, .35f, 0.f};
                    e->gravity = {0.f, .25f, 0.f}; e->drag = 1.4f; e->noiseStrength = .25f; e->radius = .01f;
                    break;
                case Dust:
                    e->blendMode = saida::ParticleSystemNode::BlendMode::Alpha;
                    e->maxParticles = 96; e->burstCount = 14; e->lifetime = .8f; e->startSize = .045f;
                    e->endSizeScale = 3.6f; e->startSpeed = 1.7f; e->coneAngle = 55.f; e->emissive = 0.f;
                    // Concrete and stone dust, the albedo of a pavement.
                    e->startColor = {.24f, .22f, .2f, .75f}; e->endColor = {.26f, .24f, .22f, 0.f};
                    e->gravity = {0.f, -2.6f, 0.f}; e->drag = 2.2f; e->radius = .03f;
                    break;
                case Sparks:
                    e->blendMode = saida::ParticleSystemNode::BlendMode::Additive;
                    e->maxParticles = 96; e->burstCount = 16; e->lifetime = .32f; e->startSize = .012f;
                    e->endSizeScale = .5f; e->startSpeed = 4.2f; e->coneAngle = 65.f; e->emissive = 5.f;
                    e->stretch = 4.f;
                    e->startColor = {1.f, .74f, .34f, 1.f}; e->endColor = {1.f, .3f, .05f, 0.f};
                    e->gravity = {0.f, -9.8f, 0.f}; e->radius = .01f;
                    break;
                case Blood:
                    e->blendMode = saida::ParticleSystemNode::BlendMode::Alpha;
                    e->maxParticles = 96; e->burstCount = 12; e->lifetime = .5f; e->startSize = .028f;
                    e->endSizeScale = 1.6f; e->startSpeed = 1.6f; e->coneAngle = 50.f; e->emissive = 0.f;
                    e->startColor = {.24f, .015f, .015f, .9f}; e->endColor = {.16f, .01f, .01f, 0.f};
                    e->gravity = {0.f, -9.8f, 0.f}; e->drag = .6f; e->radius = .02f;
                    break;
                default: break;
            }
            // No applyEffectPreset(): it would replace every setting above
            // with its class's defaults, a looping fountain at the origin.
            emitters_[kind] = e;
        };
        for (int k = 0; k < Kinds; ++k) make(Kind(k));
        light_ = world.createChild<saida::LightNode>("combat-muzzle-light", saida::LightType::Point);
        light_->color = {1.f, .72f, .4f};
        light_->intensity = 0.f;
        light_->range = 7.f;
        light_->castShadows = false;
        light_->setEnabled(false);
    }

    void burst(Kind kind, const glm::vec3& at) {
        auto* e = emitters_[kind];
        if (!e) return;
        e->transform().position = at;
        e->burst();
    }

    // A shot leaving the muzzle: the flash, a wisp of smoke, and the street lit
    // for a frame or two.
    void muzzle(const glm::vec3& at) {
        burst(Flash, at);
        burst(Smoke, at);
        if (!light_) return;
        light_->transform().position = at;
        light_->intensity = 9.f;
        light_->setEnabled(true);
        lit_ = kFlashSeconds;
    }

    void update(float dt) {
        if (lit_ <= 0.f || !light_) return;
        lit_ -= dt;
        light_->intensity = 9.f * std::max(0.f, lit_ / kFlashSeconds);
        if (lit_ <= 0.f) light_->setEnabled(false);
    }

private:
    static constexpr float kFlashSeconds = .06f;
    static const std::array<const char*, Kinds>& names() {
        static const std::array<const char*, Kinds> n{"flash", "smoke", "dust", "sparks", "blood"};
        return n;
    }
    std::array<saida::ParticleSystemNode*, Kinds> emitters_{};
    saida::LightNode* light_ = nullptr;
    float lit_ = 0.f;
};

// ── sounds ──────────────────────────────────────────────────────────────────

// The fight's sounds (r1/combat_sounds.py), decoded once at start-up so a shot
// never reads the disk. Heard flat, quieter with distance: the engine's
// listener does not follow this world's camera.
class CombatSounds {
public:
    enum Sound { Shot, Punch, Swing, Fall, CarHit, DryFire, Reload, Impact, RocketLaunch, RocketExplosion, Count };

    void load() {
        auto& audio = saida::AudioManager::get();
        for (int s = 0; s < Count; ++s) {
            ready_[s] = audio.preload(path(Sound(s)));
            if (!ready_[s]) saida::Log::error("[World combat] missing sound ", path(Sound(s)),
                                              " (python -m r1.combat_sounds): it will be silent");
        }
    }

    // `distance` in metres from the camera.
    void play(Sound s, double distance = 0, float gain = 1.f) {
        if (!ready_[s]) return;
        constexpr double kFullVolumeWithin = 4, kInaudibleBeyond = 220;
        if (distance > kInaudibleBeyond) return;
        saida::AudioSettings settings;
        settings.volume = gain * float(std::min(1.0, kFullVolumeWithin / std::max(kFullVolumeWithin, distance)));
        saida::AudioManager::get().play(path(s), settings);
    }

private:
    static std::string path(Sound s) {
        static const char* names[Count] = {"pistol_shot", "punch_hit", "swing", "body_fall",
                                           "car_hit", "dry_fire", "reload", "bullet_impact", "rocket_launch", "rocket_explosion"};
        return std::string("assets/audio/combat/") + names[s] + ".wav";
    }
    std::array<bool, Count> ready_{};
};

}  // namespace r1
