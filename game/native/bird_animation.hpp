#pragma once

#include "scene/animation/PoseModifiers.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace r1 {

// Authored content over the shipped rigs. Saida owns evaluation, blending,
// skinning and LOD. No mesh, texture, weight, bone length or source clip changes.
class BirdPose final : public saida::PoseModifier {
public:
    BirdPose(const saida::Rig& rig,float facing) : forward_(std::sin(facing),0,std::cos(facing)) {
        saida::LocalPose rest;
        for(const auto& b:rig.bones())rest.localTransforms.push_back(b.restLocal);
        rest_.computeFrom(rest,rig);
        simple_=rig.findBoneIndex("wing_upper.L")>=0;
        shrike_=rig.findBoneIndex("Wing.L")>=0;
        legacy_=rig.findBoneIndex("Humerus_L")>=0;
        valid_=simple_||shrike_||legacy_||rig.findBoneIndex("LeftWing")>=0||rig.findBoneIndex("Upper_Arm_l")>=0;
        forward_=glm::normalize(glm::inverse(glm::mat3(rig.rootParent()))*forward_);
        up_=glm::normalize(glm::inverse(glm::mat3(rig.rootParent()))*glm::vec3(0,1,0));
    }
    bool valid() const {return valid_;}
    void setFlight(bool flying,float flap,float frequency,float speed,uint64_t identity) {
        wanted_=flying?1.f:0.f;wantedFlap_=flap;frequency_=frequency;speed_=speed;
        if(!seeded_){clock_=double(identity%997)*.137;seeded_=true;}
    }
    void stop() {stopped_=true;}
    void update(float dt) override {
        if(stopped_)return;
        clock_+=dt; const float blend=1.f-std::exp(-9.f*dt);
        flight_+=(wanted_-flight_)*blend;flap_+=(wantedFlap_-flap_)*blend;
    }
    bool active() const override {return valid_&&!stopped_;}
    void apply(saida::LocalPose& pose,const saida::Rig& rig) const override {
        const float beat=std::sin(float(clock_*6.283185307179586*frequency_));
        // Diagnostic Rig_Check clips are never used as locomotion. These rigs
        // receive a full local pose derived from their own bind skeleton.
        if(simple_||shrike_)for(size_t i=0;i<rig.boneCount();++i)pose.localTransforms[i]=rig.bones()[i].restLocal;
        if(legacy_)for(size_t i=0;i<rig.boneCount();++i) {
            auto& t=pose.localTransforms[i];const auto& rest=rig.bones()[i].restLocal;
            t.rotation=glm::slerp(rest.rotation,t.rotation,flight_);
            t.position=glm::mix(rest.position,t.position,flight_);
            t.scale=glm::mix(rest.scale,t.scale,flight_);
        }
        auto rotate=[&](const std::string& name,glm::vec3 axis,float angle) {
            const int i=rig.findBoneIndex(name);if(i<0)return;
            const int parent=rig.bones()[size_t(i)].parentIndex;
            const glm::mat3 frame=parent>=0?glm::mat3(rest_.globalMatrices[size_t(parent)]):glm::mat3(1);
            axis=glm::normalize(glm::inverse(frame)*axis);
            pose.localTransforms[size_t(i)].rotation=glm::normalize(glm::angleAxis(angle,axis)*pose.localTransforms[size_t(i)].rotation);
        };
        for(const char* side:{".L",".R"}) {
            const float sign=std::string(side)==".L"?-1.f:1.f;
            if(simple_) {
                rotate(std::string("wing_upper")+side,forward_,sign*flight_*(1.05f+.55f*flap_*beat));
                rotate(std::string("wing_tip")+side,up_,sign*flight_*.65f);
                rotate(std::string("wing_tip")+side,forward_,sign*flight_*flap_*.18f*std::sin(float(clock_*6.2831853*frequency_)-.7f));
                rotate(std::string("leg")+side,glm::cross(up_,forward_),flight_*.65f);
            } else if(shrike_) {
                rotate(std::string("Wing")+side,forward_,sign*((1.f-flight_)*1.1f+flight_*flap_*beat*.6f));
            }
        }
        if(legacy_) {
            // Original flapping/gliding clips drive flight; only landing folds
            // the authored wings, smoothly, in their own object frame.
            rotate("Humerus_L",forward_,-(1.f-flight_)*1.2f);
            rotate("Humerus_R",forward_, (1.f-flight_)*1.2f);
        }
        if(!simple_&&!shrike_&&!legacy_) {
            rotate("LeftWing",forward_,flight_*(1.f+.5f*beat));
            rotate("RightWing",forward_,-flight_*(1.f+.5f*beat));
            rotate("Upper_Arm_l",forward_,flight_*(1.f+.5f*beat));
            rotate("Upper_Arm_r",forward_,-flight_*(1.f+.5f*beat));
        }
        if(simple_||shrike_) {
            const float look=(1.f-flight_)*.14f*std::sin(float(clock_)*1.3f);
            rotate("head",up_,look);
            rotate("head",glm::cross(up_,forward_),(1.f-flight_)*std::min(.15f,speed_*.1f)*std::sin(float(clock_)*9.f));
            rotate(simple_?"tail_1":"tail_base",glm::cross(up_,forward_),flight_*.15f+.04f*std::sin(float(clock_)*2.f));
            if(speed_>.01f&&flight_<.1f)for(const char* side:{".L",".R"}) {
                const float sign=std::string(side)==".L"?1.f:-1.f;
                rotate(std::string(simple_?"leg":"thigh")+side,glm::cross(up_,forward_),sign*.25f*std::sin(float(clock_)*speed_*15.f));
            }
        }
    }
private:
    saida::GlobalPose rest_;
    glm::vec3 forward_,up_{0,1,0};
    bool valid_=false,simple_=false,shrike_=false,legacy_=false,stopped_=false,seeded_=false;
    double clock_=0;
    float flight_=0,wanted_=0,flap_=1,wantedFlap_=1,frequency_=4,speed_=0;
};

} // namespace r1
