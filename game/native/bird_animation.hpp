#pragma once

#include "scene/animation/PoseModifiers.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace r1 {

// R1World flight poses over the shipped rigs. The engine owns evaluation,
// blending and skinning; mesh weights, bone lengths and source clips stay intact.
class BirdPose final : public saida::PoseModifier {
public:
    BirdPose(const saida::Rig& rig,float facing) {
        for(const auto& b:rig.bones())restPose_.localTransforms.push_back(b.restLocal);
        rest_.computeFrom(restPose_,rig);
        const auto object=glm::inverse(glm::mat3(rig.rootParent()));
        forward_=glm::normalize(object*glm::vec3(std::sin(facing),0,std::cos(facing)));
        up_=glm::normalize(object*glm::vec3(0,1,0));
        right_=glm::normalize(glm::cross(up_,forward_));
        simple_=rig.findBoneIndex("wing_upper.L")>=0;
        shrike_=rig.findBoneIndex("Wing.L")>=0;
        legacy_=rig.findBoneIndex("Humerus_L")>=0;
        valid_=simple_||shrike_||legacy_||rig.findBoneIndex("LeftWing")>=0||rig.findBoneIndex("Upper_Arm_l")>=0;

        // The simple rigs (including pigeons) are bound with wings pointing
        // BACK and down, not sideways. Rolling them about the forward axis
        // cannot open them. Author the spread pose from their actual bind
        // directions, shoulder first, then wrist in the deployed parent frame.
        auto spread=restPose_;
        for(int side=0;side<2;++side) {
            const std::string suffix=side==0?".L":".R";
            const std::string upper=simple_?"wing_upper"+suffix:shrike_?"Wing"+suffix:
                legacy_?(side==0?"Humerus_L":"Humerus_R"):
                rig.findBoneIndex("LeftWing")>=0?(side==0?"LeftWing":"RightWing"):
                (side==0?"Upper_Arm_l":"Upper_Arm_r");
            const int bone=rig.findBoneIndex(upper);if(bone<0)continue;
            const int parent=rig.bones()[size_t(bone)].parentIndex;
            const auto origin=parent>=0?glm::vec3(rest_.globalMatrices[size_t(parent)][3]):glm::vec3(0);
            const float sign=glm::dot(glm::vec3(rest_.globalMatrices[size_t(bone)][3])-origin,right_)<0?-1.f:1.f;
            const auto outward=sign*right_;
            addWing(rig,spread,upper,outward+.08f*up_+.12f*forward_,sign,0,simple_||(!shrike_&&!legacy_&&rig.findBoneIndex("Upper_Arm_l")>=0));
            if(simple_)addWing(rig,spread,"wing_tip"+suffix,outward-.22f*forward_,sign,1,true);
            if(shrike_) {
                addWing(rig,spread,"Wing.001"+suffix,outward-.16f*forward_,sign,1,false);
                addWing(rig,spread,"Wing.002"+suffix,outward-.24f*forward_,sign,2,false);
            }
            if(!simple_&&!shrike_&&!legacy_) {
                const std::string arm=side==0?"Arm_l":"Arm_r";
                addWing(rig,spread,arm,outward-.22f*forward_,sign,1,true);
            }
        }
    }
    bool valid() const {return valid_;}
    void setFlight(bool flying,float flap,float frequency,float speed,uint64_t identity) {
        wanted_=flying?1.f:0.f;wantedFlap_=std::clamp(flap,0.f,1.f);
        frequency_=std::clamp(frequency,.1f,20.f);speed_=std::max(0.f,speed);
        if(!seeded_){clock_=double(identity%997)*.137;phase_=double(identity%997)/997.;seeded_=true;}
    }
    void stop() {stopped_=true;}
    void update(float dt) override {
        if(stopped_||dt<=0)return;
        clock_+=dt;
        // Integrate phase, rather than multiplying elapsed time by frequency:
        // changing cadence must never teleport the wings mid-stroke.
        phase_=std::fmod(phase_+double(dt)*frequency_,1.);
        const float blend=1.f-std::exp(-9.f*dt);
        flight_+=(wanted_-flight_)*blend;flap_+=(wantedFlap_-flap_)*blend;
    }
    bool active() const override {return valid_&&!stopped_;}
    void apply(saida::LocalPose& pose,const saida::Rig& rig) const override {
        if(simple_||shrike_)pose=restPose_; // Rig_Check is not locomotion.
        if(legacy_) {
            for(size_t i=0;i<rig.boneCount();++i) {
                auto& t=pose.localTransforms[i];const auto& rest=rig.bones()[i].restLocal;
                t.rotation=glm::slerp(rest.rotation,t.rotation,flight_);
                t.position=glm::mix(rest.position,t.position,flight_);
                t.scale=glm::mix(rest.scale,t.scale,flight_);
                // These source clips already articulate shoulder/elbow/digits.
                // Blend to their spread bind pose during glides, including
                // seagulls, which have no separate glide animation.
                const auto& name=rig.bones()[i].name;
                if(name.find("Humerus_")==0||name.find("Ulna_")==0||name.find("Digit_")==0)
                    t.rotation=glm::slerp(rest.rotation,t.rotation,flap_);
            }
        }
        const float recovery=returnStroke(float(phase_));
        for(const auto& wing:wings_) {
            auto& rotation=pose.localTransforms[size_t(wing.index)].rotation;
            if(legacy_) {
                rotation=glm::normalize(glm::angleAxis(-wing.sign*(1.f-flight_)*1.2f,wing.flapAxis)*rotation);
                continue;
            }
            float angle;
            if(wing.segment==0)angle=.12f+flap_*(.82f*stroke(float(phase_))-.12f);
            else {
                // Distal joints trail the shoulder, then fold/sweep back on
                // recovery. A fully spread, quiet wing carries the glide.
                const float lag=wing.segment==1?.10f:.16f;
                angle=flap_*.32f*(stroke(float(phase_)-lag)-stroke(float(phase_)));
            }
            auto flying=glm::angleAxis(wing.sign*angle,wing.flapAxis)*wing.spread;
            if(wing.segment>0) {
                flying=glm::angleAxis(wing.sign*flap_*recovery*.55f,wing.sweepAxis)*flying;
                flying=glm::angleAxis(flap_*recovery*.22f,wing.twistAxis)*flying;
            }
            rotation=glm::normalize(glm::slerp(rotation,flying,flight_));
            if(shrike_&&wing.segment==0)
                rotation=glm::normalize(glm::angleAxis(-wing.sign*(1.f-flight_)*1.1f,wing.flapAxis)*rotation);
        }
        auto rotate=[&](const std::string& name,glm::vec3 axis,float angle) {
            const int i=rig.findBoneIndex(name);if(i<0)return;
            const int parent=rig.bones()[size_t(i)].parentIndex;
            const glm::mat3 frame=parent>=0?glm::mat3(rest_.globalMatrices[size_t(parent)]):glm::mat3(1);
            axis=glm::normalize(glm::inverse(frame)*axis);
            pose.localTransforms[size_t(i)].rotation=glm::normalize(glm::angleAxis(angle,axis)*pose.localTransforms[size_t(i)].rotation);
        };
        if(simple_||shrike_) {
            const auto pitch=glm::cross(up_,forward_);
            const float effort=flight_*flap_;
            rotate("head",up_,(1.f-flight_)*.14f*std::sin(float(clock_)*1.3f));
            rotate("head",pitch,(1.f-flight_)*std::min(.15f,speed_*.1f)*std::sin(float(clock_)*9.f));
            rotate(simple_?"tail_1":"tail_base",pitch,flight_*.12f+effort*.045f*stroke(float(phase_)-.15f));
            if(simple_)rotate("tail_2",pitch,effort*.035f*stroke(float(phase_)-.22f));
            for(const char* side:{".L",".R"}) {
                const float sign=std::string(side)==".L"?1.f:-1.f;
                const float walk=(1.f-flight_)*std::min(1.f,speed_*3.f)*sign*.25f*std::sin(float(clock_)*speed_*15.f);
                rotate(std::string(simple_?"leg":"thigh")+side,pitch,flight_*.65f+walk);
                if(simple_)rotate(std::string("foot")+side,pitch,-flight_*.3f);
            }
        }
    }
private:
    // Smooth reversals, brisk power stroke (43%), longer recovery (57%).
    static float stroke(float phase) {
        phase-=std::floor(phase);
        constexpr float pi=3.14159265358979323846f;
        return phase<.43f?std::cos(pi*phase/.43f):-std::cos(pi*(phase-.43f)/.57f);
    }
    static float returnStroke(float phase) {
        if(phase<=.43f)return 0;
        const float s=std::sin(3.14159265358979323846f*(phase-.43f)/.57f);
        return s*s;
    }
    struct Wing {
        int index,segment;
        float sign;
        glm::quat spread;
        glm::vec3 flapAxis,sweepAxis,twistAxis;
    };
    void addWing(const saida::Rig& rig,saida::LocalPose& spread,const std::string& name,
                 glm::vec3 direction,float sign,int segment,bool unfold) {
        const int i=rig.findBoneIndex(name);if(i<0)return;
        saida::GlobalPose global;global.computeFrom(spread,rig);
        const int parent=rig.bones()[size_t(i)].parentIndex;
        const auto frame=parent>=0?glm::mat3(global.globalMatrices[size_t(parent)]):glm::mat3(1);
        const auto inverse=glm::inverse(frame);
        auto& rotation=spread.localTransforms[size_t(i)].rotation;
        if(unfold) {
            const auto from=glm::normalize(inverse*glm::vec3(global.globalMatrices[size_t(i)][1]));
            const auto to=glm::normalize(inverse*direction);
            rotation=glm::normalize(glm::quat(from,to)*rotation);
        } else if(name=="LeftWing"||name=="RightWing") {
            // Peacock bones run along the body, not along the wing span.
            rotation=glm::normalize(glm::angleAxis(sign*1.05f,glm::normalize(inverse*forward_))*rotation);
        }
        wings_.push_back({i,segment,sign,rotation,glm::normalize(inverse*forward_),
                          glm::normalize(inverse*up_),glm::normalize(inverse*right_)});
    }
    saida::LocalPose restPose_;
    saida::GlobalPose rest_;
    std::vector<Wing> wings_;
    glm::vec3 forward_,up_,right_;
    bool valid_=false,simple_=false,shrike_=false,legacy_=false,stopped_=false,seeded_=false;
    double clock_=0,phase_=0;
    float flight_=0,wanted_=0,flap_=1,wantedFlap_=1,frequency_=4,speed_=0;
};

} // namespace r1
