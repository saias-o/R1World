#pragma once

#include "physics/Ragdoll.hpp"
#include "graphics/Material.hpp"
#include "scene/animation/PoseModifiers.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

namespace r1 {

inline saida::Transform animalTransform(const glm::mat4& matrix) {
    saida::Transform out;out.position=glm::vec3(matrix[3]);
    glm::mat3 rotation(matrix);
    for(int c=0;c<3;++c){out.scale[c]=glm::length(rotation[c]);rotation[c]/=std::max(out.scale[c],1e-8f);}
    out.rotation=glm::normalize(glm::quat_cast(rotation));return out;
}

inline nlohmann::json animalTransformJson(const saida::Transform& t) {
    return {{"p",{t.position.x,t.position.y,t.position.z}},
            {"q",{t.rotation.w,t.rotation.x,t.rotation.y,t.rotation.z}},
            {"s",{t.scale.x,t.scale.y,t.scale.z}}};
}
inline saida::Transform animalReadTransform(const nlohmann::json& j) {
    if(j.at("p").size()!=3||j.at("q").size()!=4||j.at("s").size()!=3)
        throw std::runtime_error("incomplete animal transform");
    saida::Transform t;
    t.position={j.at("p").at(0),j.at("p").at(1),j.at("p").at(2)};
    t.rotation=glm::quat(j.at("q").at(0).get<float>(),j.at("q").at(1).get<float>(),j.at("q").at(2).get<float>(),j.at("q").at(3).get<float>());
    t.scale={j.at("s").at(0),j.at("s").at(1),j.at("s").at(2)};
    for(int i=0;i<3;++i)if(!std::isfinite(t.position[i])||!std::isfinite(t.scale[i])||t.scale[i]<=0)
        throw std::runtime_error("invalid animal transform");
    if(!std::isfinite(glm::length(t.rotation))||glm::length(t.rotation)<1e-5f)
        throw std::runtime_error("invalid animal rotation");
    t.rotation=glm::normalize(t.rotation);return t;
}

inline saida::LocalPose animalLocalPose(const saida::Rig& rig,const saida::GlobalPose& pose) {
    saida::LocalPose local;local.resize(rig.boneCount());
    for(size_t i=0;i<rig.boneCount();++i) {
        const int p=rig.bones()[i].parentIndex;
        local.localTransforms[i]=animalTransform(p<0?pose.globalMatrices[i]:
            glm::inverse(pose.globalMatrices[size_t(p)])*pose.globalMatrices[i]);
    }
    return local;
}

// A saved game-specific corpse pose, applied through the engine's modifier
// contract. A subsequent RagdollBehaviour overrides it when hit again.
class AnimalRestPose final : public saida::PoseModifier {
public:
    saida::LocalPose pose;
    void update(float) override {}
    bool active() const override {return !pose.localTransforms.empty();}
    void apply(saida::LocalPose& out,const saida::Rig&) const override {
        if(out.localTransforms.size()==pose.localTransforms.size())out=pose;
    }
};

// Content fitting for the shipped animal skeletons. Shapes and constraints
// remain Saida's; dimensions here are in the rig's frame, not GLB metres.
inline saida::RagdollProfile animalRagdoll(const saida::Rig& rig, float length,
                                         float width, float height, float facing,
                                         float scale = 1.f) {
    const float units = glm::length(glm::vec3(rig.rootParent()[0]));
    const float unit = std::max(units, 1e-6f);
    const glm::vec3 forward = glm::normalize(glm::inverse(glm::mat3(rig.rootParent())) *
        glm::vec3(std::sin(facing), 0, std::cos(facing)));
    std::map<std::string, std::string> names;
    auto lower=[](std::string s){for(char& c:s)c=char(std::tolower(static_cast<unsigned char>(c)));return s;};
    for(const auto& b:rig.bones())names[lower(b.name)]=b.name;
    auto has=[&](const std::string& n){return names.count(lower(n))!=0;};
    std::vector<std::vector<std::string>> torso,legs;
    auto chain=[&](std::initializer_list<const char*> n){std::vector<std::string> out;for(auto s:n)out.push_back(s);return out;};
    const bool bird=has("wing_upper.L")||has("Humerus_L")||has("Wing.L")||has("LeftWing")||has("Upper_Arm_l")||has("Finger3_L");
    if(bird)width=std::min(width,std::max(.012f,length*.3f));
    if(has("wing_upper.L")) {
        torso={chain({"root","neck","head"})};
        for(auto side:{".L",".R"}) {
            legs.push_back({"wing_upper"+std::string(side),"wing_tip"+std::string(side)});
            legs.push_back({"leg"+std::string(side),"foot"+std::string(side)});
        }
    } else if(has("Humerus_L")) {
        torso={chain({"Body","Head"})};
        for(auto side:{"_L","_R"})legs.push_back({"Humerus"+std::string(side),"Ulna"+std::string(side),"Digit"+std::string(side)});
    } else if(has("Wing.L")) {
        torso={chain({"root","pelvis_control","spine","ribcage","neck","head"})};
        for(auto side:{".L",".R"}) {
            legs.push_back({"Wing"+std::string(side),"Wing.001"+std::string(side),"Wing.002"+std::string(side)});
            legs.push_back({"thigh"+std::string(side),"shin"+std::string(side)});
        }
    } else if(has("LeftWing")) {
        torso={chain({"Body","Neck1","Neck2","Neck3","Head"})};
        legs={chain({"LeftWing"}),chain({"RightWing"}),chain({"LeftLeg","LeftShin","LeftFoot"}),chain({"RightLeg","RightShin","RightFoot"})};
    } else if(has("Upper_Arm_l")) {
        torso={chain({"Hip","SpineA","SpineB","NeckA","NeckB","NeckC","Head"})};
        for(auto side:{"_l","_r"}) {
            legs.push_back({"Upper_Arm"+std::string(side),"Arm"+std::string(side)});
            legs.push_back({"Upperleg"+std::string(side),"Leg"+std::string(side),"Feet"+std::string(side)});
        }
    } else if(has("Finger3_L")||has("Skull")) {
        torso={has("Skull")?chain({"Hip","Backbone","Backbone.001","Backbone.002","Skull"}):chain({"Hip","Backbone","Bone.003","Bone.004","Head"})};
        for(auto side:{"_L","_R"})legs.push_back({"Leg"+std::string(side)+".001","Leg"+std::string(side)+".002","Finger3"+std::string(side)});
    } else if(has("fore_upper.L")) {
        torso={chain({"root","spine","neck","head"})};
        for(auto side:{".L",".R"})for(auto pair:{std::pair{"fore_upper","fore_lower"},std::pair{"hind_upper","hind_lower"}})
            legs.push_back({std::string(pair.first)+side,std::string(pair.second)+side,std::string(pair.first).substr(0,4)+"_foot"+side});
    } else if(has("Bovidae_pelvis")) {
        torso={chain({"Bovidae_pelvis","Bovidae_spine","Bovidae_spine1","Bovidae_Neck","Bovidae_Head"})};
        for(auto side:{"left","right"})for(auto end:{"Front","Rear"}) {
            const std::string lr=std::string(side)=="left"?"L":"R",prefix=std::string("Bovidae_")+end;
            legs.push_back({prefix+"Thigh_"+lr,prefix+"Calf_"+side,prefix+"Foot_"+side});
        }
    } else if(has("Hip")&&has("Backbone")) {
        torso={chain({"Hip","Backbone","Backbone.001","Backbone.002","Head"})};
        for(auto side:{"_L","_R"})for(auto end:{"Frontleg","Backleg"})
            legs.push_back({std::string(end)+side,std::string(end)+side+".001",std::string(end)+side+".002"});
    } else if(has("Hips")&&has("FrontUpLeg_L")) {
        torso={chain({"Hips","Spine","Spine.001","Neck","Head"})};
        for(auto side:{"_L","_R"})for(auto end:{"Front","Back"})
            legs.push_back({std::string(end)+"UpLeg"+side,std::string(end)+"Leg"+side,std::string(end)+"Paw"+side});
    } else if(has("Fox_pelvis")||has("FrontLeg1_L")) {
        const std::string p=has("Fox_pelvis")?"Fox_":"";
        torso={{p.empty()?"Bone":"Fox_pelvis",p.empty()?"Bone.005":"Fox_spine1",p.empty()?"Bone.001":"Fox_spine2",p+"Neck1",p+"Neck2",p+"Head"}};
        for(auto side:{"_L","_R"})for(auto end:{"Front","Back"}) {
            std::string upper=p+end+"Leg1"+side;
            if(!has(upper)&&has("Fox_"+upper))upper="Fox_"+upper;
            legs.push_back({upper,p+end+"Leg2"+side,p+end+"Leg3"+side,p+end+"Toe"+side});
        }
    } else if(has("Deer01_Pelvis")||has("Horse_Pelvis")) {
        const std::string p=has("Deer01_Pelvis")?"Deer01_":"Horse_";
        torso={{p+"Pelvis",p+"Spine",p+"Spine1",p+"Neck",p+"Neck1",p+"Head"}};
        for(auto side:{"L_","R_"}) {
            legs.push_back({p+side+"UpperArm",p+side+"Forearm",p+side+"Hand"});
            legs.push_back({p+side+"Thigh",p+side+"Calf",p+side+"Foot"});
        }
    } else if(has("main_pelvis")) {
        torso={chain({"main_pelvis","spine_01","spine_02","spine_neck","neck_01","neck_02","neck_03","head"})};
        for(auto side:{"left_","right_"})for(auto end:{"f_","r_"}) {
            const std::string p=std::string(side)+end;
            legs.push_back({p+"leg_top",p+"leg_mid",p+"leg_bottom",p+"foot"});
        }
    } else if(has("Elephantidae_Vertebrae_Dorsal")) {
        torso={chain({"Elephantidae_Vertebrae_Dorsal","Elephantidae_Vertebrae_Lumbar","Elephantidae_Pelvis"}),
               chain({"Elephantidae_Vertebrae_Cervical","Elephantidae_Head","Elephantidae_Trunk_1","Elephantidae_Trunk_5","Elephantidae_Trunk_9"})};
        for(auto side:{"_L","_R"}) {
            legs.push_back({"Elephantidae_Arm"+std::string(side),"Elephantidae_Forearm"+std::string(side),"Elephantidae_Hand"+std::string(side)});
            legs.push_back({"Elephantidae_Thigh"+std::string(side),"Elephantidae_Shin"+std::string(side),"Elephantidae_Foot"+std::string(side)});
        }
    } else if(has("Ursidae_Pelvis")||has("Hippopotamidae_Pelvis")||has("Camelus_Pelvis")||has("Horse_Spine")) {
        const std::string p=has("Ursidae_Pelvis")?"Ursidae_":has("Hippopotamidae_Pelvis")?"Hippopotamidae_":has("Camelus_Pelvis")?"Camelus_":"Horse_";
        const bool camel=p=="Camelus_", horse=p=="Horse_";
        torso={{p+"Pelvis",p+"Spine",p+"Spine_1",p+"Spine1",p+"Spine_2",p+"Spine2",p+"Neck",p+"Neck_01",p+"Neck_02",p+"Neck_03",p+"Neck_04",p+"Head"}};
        for(auto side:{"_L","_R"}) {
            const std::string s=side;
            legs.push_back({p+(camel||horse?"upperarm":"Upperarm")+s,p+"Forearm"+s,p+(camel||horse?"Hand":"Paw")+s});
            legs.push_back({p+(camel?"Thigh":"Leg")+s,p+"Shin"+s,p+"Foot"+s});
        }
    } else if(has("Boar_Body")) {
        torso={chain({"Boar_Body","Boar_Neck","Boar_Head"}),chain({"Boar_Rear"})};
        for(auto side:{"Left","Right"}) {
            legs.push_back({"Boar_Front_Leg_"+std::string(side),"Boar_Forearm_"+std::string(side),"Boar_Front_Foot_"+std::string(side)});
            legs.push_back({"Boar_Back_Leg_"+std::string(side),"Boar_Shin_"+std::string(side),"Boar_Back_Foot_"+std::string(side)});
        }
    } else if(has("leg_front_top.L")) {
        torso={chain({"pelvis","spine.1","spine.2","chest","neck.1","neck.2","head"})};
        for(auto side:{".L",".R"})for(auto end:{"front","back"}) {
            const std::string p=std::string("leg_")+end;
            legs.push_back({p+"_top"+side,p+"_mid"+side,p+"_bot"+side,p+"_foot"+side});
        }
    } else if(has("ThreeBagsFull")) {
        torso={chain({"ThreeBagsFull","Body_Front","Neck","Head"}),chain({"Body_Rear"})};
        for(auto side:{"Left","Right"}) {
            legs.push_back({"Arm_"+std::string(side),"Forearm_"+std::string(side),"Hand_"+std::string(side)});
            legs.push_back({"Leg_"+std::string(side),"Shin_"+std::string(side),"Foot_"+std::string(side)});
        }
    } else if(has("thigh_front_r")) {
        torso={chain({"Base","spine","neck","head"}),chain({"spine_back","back"})};
        for(auto side:{"_l","_r"})for(auto end:{"front","back"})
            legs.push_back({"thigh_"+std::string(end)+side,"calf_"+std::string(end)+side,"foot_"+std::string(end)+side});
    } else if(has("rhino")) {
        torso={chain({"rhino","spine1","spine2","neck","head"})};
        for(auto side:{"_L","_R"}) {
            legs.push_back({"leg_upper"+std::string(side),"leg"+std::string(side),"foot_front"+std::string(side)});
            legs.push_back({"thigh"+std::string(side),"leg_back"+std::string(side),"foot_back"+std::string(side)});
        }
    } else if(has("ZebraArmature")) {
        torso={chain({"body","rear_body"}),chain({"neck1","neck2","head"})};
        for(auto side:{"left","right"}) {
            legs.push_back({std::string(side)+"_leg",std::string(side)+"_shin",std::string(side)+"_foot"});
            legs.push_back({"rear_"+std::string(side)+"_leg","rear_"+std::string(side)+"_shin","rear_"+std::string(side)+"_foot"});
        }
    } else if(has("Bone001_00")) {
        torso={chain({"Bone001_00","Bone002_01","Bone003_02","Bone004_03"})};
        legs={chain({"Bone017_06","Bone018_07"}),chain({"Bone019_08","Bone020_09"}),
              chain({"Bone023_010","Bone024_011"}),chain({"Bone021_012","Bone022_013"})};
    } else if(has("leg.back.R.001")) {
        torso={chain({"Bone","Bone.003","Bone.009","Bone.004"})};
        for(auto side:{".L",".R"})for(auto end:{"front","back"})
            legs.push_back({"leg."+std::string(end)+side+".001","leg."+std::string(end)+side+".002","leg."+std::string(end)+side+".003"});
    } else if(has("Spine1")&&has("FrontLKnee")) {
        torso={chain({"Spine1","Spine2","Tail1","Tail2"}),chain({"Neck","TopJaw"})};
        for(auto side:{"L","R"})for(auto end:{"Front","Back"})
            legs.push_back({std::string(end)+side+"Knee",std::string(end)+side+"Leg",std::string(end)+side+"Foot"});
    }
    saida::RagdollProfile out;
    // Inferred mass from a filled fraction of the measured bounding volume.
    // An adult elephant resists the same blast far more than a rabbit.
    out.mass=std::clamp(length*width*height*(bird?35.f:150.f)*scale*scale*scale,.015f,6000.f);
    out.friction=.85f;out.restitution=.02f;out.angularDamping=.55f;out.linearDamping=.12f;
    out.jointFriction=std::clamp(out.mass*.012f,.0001f,8.f);
    out.settleSpeed=.12f;out.settleTime=1.2f;
    std::map<int,saida::RagdollPart> selected;
    auto fit=[&](const std::vector<std::string>& raw,bool limb) {
        std::vector<int> ids;
        for(const auto& n:raw)if(has(n))ids.push_back(rig.findBoneIndex(names.at(lower(n))));
        for(size_t i=0;i<ids.size();++i) {
            if(limb&&i+1==ids.size())break;
            const int b=ids[i];saida::RagdollPart p;p.bone=rig.bones()[size_t(b)].name;
            p.radius=std::max(.001f/unit,(limb?std::min(width*.105f,height*.055f):width*.32f)/unit);
            p.massShare=limb?.025f:.16f;p.swing=limb?.9f:.35f;p.twist=limb?.3f:.25f;
            p.length=std::max(.003f,height*(limb?.15f:.12f))/unit;p.axis=forward;
            if(i+1<ids.size()) {
                const float d=glm::distance(glm::vec3(glm::inverse(rig.bones()[size_t(b)].inverseBindMatrix)[3]),
                    glm::vec3(glm::inverse(rig.bones()[size_t(ids[i+1])].inverseBindMatrix)[3]));
                if(d>1e-4f)p.end=rig.bones()[size_t(ids[i+1])].name;
            }
            if(limb&&i>0){p.joint=saida::RagdollJoint::Hinge;p.bendToward=forward;p.bendMin=-.12f;p.bendMax=2.35f;}
            selected[b]=p;
        }
    };
    for(const auto& c:torso)fit(c,false);
    for(const auto& c:legs)fit(c,true);
    // Compact/unnamed rigs still fall as a body; their authored pose stays
    // intact. Never invent humanoid joints on an insect or an unknown rig.
    int root=selected.empty()?int(rig.evaluationOrder().front()):selected.begin()->first;
    const int first=root;
    auto ancestor=[&](int a,int b){for(int x=b;x>=0;x=rig.bones()[size_t(x)].parentIndex)if(x==a)return true;return false;};
    for(const auto& [b,p]:selected)while(root>=0&&!ancestor(root,b))root=rig.bones()[size_t(root)].parentIndex;
    if(root<0)root=first;
    if(!selected.count(root)) {
        saida::RagdollPart p;p.bone=rig.bones()[size_t(root)].name;
        p.length=std::max(.006f,length*.45f)/unit;p.axis=forward;
        p.radius=std::max(.001f,width*.3f)/unit;p.massShare=.25f;selected[root]=p;
    }
    out.parts.push_back(selected.at(root));
    for(auto b:rig.evaluationOrder())if(int(b)!=root&&selected.count(int(b))) {
        auto p=selected.at(int(b));
        bool linked=false;
        for(int a=rig.bones()[b].parentIndex;a>=0;a=rig.bones()[size_t(a)].parentIndex)linked=linked||selected.count(a);
        if(!linked)p.parent=selected.at(root).bone;
        out.parts.push_back(std::move(p));
    }
    return out;
}

inline glm::vec3 animalBlastVelocity(float mass,float distance,const glm::vec3& away) {
    const float falloff=std::clamp(1.f-distance/14.f,0.f,1.f);
    // Bounded momentum and a vented upward component. Small bodies travel;
    // massive bodies tumble close to the blast instead of sharing one speed.
    const float speed=std::min(22.f,1800.f/std::max(mass,1.f))*falloff*falloff;
    const glm::vec3 direction=glm::dot(away,away)>1e-6f?glm::normalize(away):glm::vec3(0,1,0);
    return (direction+glm::vec3(0,.65f,0))*speed;
}

inline float animalShotMomentum(const saida::RagdollProfile& profile,size_t part,float momentum) {
    float shares=0;for(const auto& p:profile.parts)shares+=p.massShare;
    const float partMass=profile.mass*profile.parts.at(part).massShare/std::max(shares,.001f);
    return std::min({momentum,4.f,partMass*3.f});
}

inline saida::MaterialDesc charredAnimalMaterial(saida::MaterialDesc material) {
    // Retain alpha, authored texture detail and normal maps, including fur
    // cards; only this instance receives the cached material variant.
    material.baseColor*=glm::vec4(.035f,.032f,.028f,1.f);
    material.metallic=0;material.roughness=.98f;material.emissiveColor=glm::vec4(0);
    material.emissiveId=saida::kAssetInvalid;material.environmentReflection=0;
    return material;
}

} // namespace r1
