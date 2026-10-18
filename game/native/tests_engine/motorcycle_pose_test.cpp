#include "../combat_kit.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/Node.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/AnimationClip.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    const std::filesystem::path game=argv[1];
    std::ifstream file(game/"assets/models/humans/humans.json");const auto humans=nlohmann::json::parse(file);
    saida::GltfAnimationData player,seated;std::string error;
    if(!saida::GLTFLoader::loadAnimationData((game/"assets/models/humans/player.glb").string(),player,&error)||
       !saida::GLTFLoader::loadAnimationData((game/humans.at("crowd")[0].at("model").get<std::string>()).string(),seated,&error)) {
        std::fprintf(stderr,"%s\n",error.c_str());return 1;
    }
    const saida::AnimationClip* sit=nullptr;
    for(const auto& clip:seated.clips)if(clip->name()=="sit")sit=clip.get();
    if(!sit)return 1;
    const float scale=humans.at("scale").get<float>()*.85f;
    const float drop=(humans.at("crowd")[0].at("seat").at("pelvisHeight").get<float>()-.14f)*scale;
    bool passed=true;
    auto check=[&](saida::Rig& rig,const saida::AnimationClip* sit,float drop,const std::string& name,bool cross,int yaw) {
        saida::Node node;auto* animator=node.addBehaviour<saida::Animator>();
        animator->setRig(&rig);animator->addClip("sit",sit);animator->play("sit",true,0.f);
        r1::holdRocketboxRidingBase(*animator);
        auto* lean=animator->addModifier<saida::GazeModifier>(rig,r1::rocketboxRidingLean());lean->lookAt({0,-.4f,1.2f});
        auto* torso=animator->addModifier<r1::RocketboxRidingAim>(rig);
        auto* head=animator->addModifier<saida::GazeModifier>(rig,r1::rocketboxRidingLook());
        const bool aiming=yaw!=999;const float radians=glm::radians(float(yaw));
        const glm::vec3 direction(std::sin(radians),0,std::cos(radians));
        if(aiming){torso->lookAt(direction);head->lookAt(direction*100.f+glm::vec3(0,1.5f,0));}
        else head->lookAt({0,1.5f,100});
        saida::Transform rider;rider.position=r1::rocketboxRidingSeat(rig,cross,.68f,scale,drop);
        rider.rotation=glm::quat(0,0,1,0);rider.scale=glm::vec3(scale);
        glm::vec3 contacts[2];saida::Transform palms[2];saida::TwoBoneIKModifier* arms[2]{};
        for(int h=0;h<2;++h) {
            const float halfBar=cross?.361f:.285f;
            contacts[h]=glm::vec3(glm::inverse(rider.matrix())*glm::vec4(glm::vec3(h?halfBar:-halfBar,cross?1.20f:1.116f,cross?-.344f:-.390f)*.68f,1));
            palms[h]=r1::rocketboxBarGrip(rig,h==1);
            if(h==0)torso->keepLeftHandAt(contacts[h]-palms[h].rotation*palms[h].position);
            auto* arm=animator->addModifier<saida::TwoBoneIKModifier>(rig,r1::rocketboxArm(h==1));
            arms[h]=arm;
            arm->setTipRotation(palms[h].rotation);arm->reach(contacts[h]-palms[h].rotation*palms[h].position);arm->setWeightNow(1);
            arm->setPole({contacts[h].x*1.7f,contacts[h].y-.15f,contacts[h].z-.25f});
        }
        animator->addModifier<r1::RocketboxRidingHands>(rig)->hold(true);
        saida::Transform pistol;r1::pistolGrip(rig,pistol);
        for(int tick=0;tick<180;++tick) {
            animator->onUpdate(1.f/60);
            if(aiming) {
                const auto shoulder=glm::vec3(animator->globalPose().globalMatrices.at(size_t(rig.findBoneIndex("Bip01 R UpperArm")))[3]);
                const auto side=glm::normalize(glm::cross(glm::vec3(0,1,0),direction)),top=glm::cross(direction,side);
                arms[1]->reach(shoulder+direction*(r1::armLength(rig,true)*.9f)+glm::vec3(-.05f,.18f*std::clamp(-direction.z*2.f,0.f,1.f),0));
                arms[1]->setTipRotation(glm::normalize(glm::quat_cast(glm::mat3(side,top,direction))*glm::inverse(pistol.rotation)));
                arms[1]->setPole(shoulder-side*.4f-top*.5f+direction*.12f);
            }
        }
        passed=passed&&std::abs(animator->activeClipNode()->time()-3.f)<.0001f;
        const auto& pose=animator->globalPose().globalMatrices;
        for(int h=0;h<2;++h) {
            const auto& hand=pose.at(size_t(rig.findBoneIndex(h?"Bip01 R Hand":"Bip01 L Hand")));
            if(h==1&&aiming) {
                const auto forward=glm::normalize(glm::mat3(hand)*(pistol.rotation*glm::vec3(0,0,1)));
                passed=passed&&glm::dot(forward,direction)>.995f;
                continue;
            }
            const auto wrist=contacts[h]-palms[h].rotation*palms[h].position;
            const float distance=glm::distance(glm::vec3(hand*glm::vec4(palms[h].position,1)),contacts[h])*scale;
            const float wristError=glm::distance(glm::vec3(hand[3]),wrist)*scale;
            const float angle=glm::angle(glm::inverse(palms[h].rotation)*glm::normalize(glm::quat_cast(glm::mat3(hand))));
            const auto root=glm::vec3(pose.at(size_t(rig.findBoneIndex(h?"Bip01 R UpperArm":"Bip01 L UpperArm")))[3]);
            const auto mid=glm::vec3(pose.at(size_t(rig.findBoneIndex(h?"Bip01 R Forearm":"Bip01 L Forearm")))[3]);
            std::printf("%s %s yaw=%d %s palm=%.5fm wrist=%.5fm rotation=%.3fdeg requested=%.5f reach=%.5f\n",name.c_str(),cross?"cross":"city",yaw,h?"right":"left",distance,wristError,glm::degrees(angle),glm::distance(root,wrist),glm::distance(root,mid)+glm::distance(mid,glm::vec3(hand[3])));
            passed=passed&&distance<.01f;
        }
    };
    for(bool cross:{false,true})for(int yaw:{999,-180,-135,-90,-45,0,45,90,135,180})
        check(*player.rigs.front(),sit,drop,"player",cross,yaw);
    int females=0;
    for(const auto& entry:humans.at("crowd")) {
        if(entry.at("sex")=="f"&&females++>=2)continue;
        saida::GltfAnimationData data;
        if(!saida::GLTFLoader::loadAnimationData((game/entry.at("model").get<std::string>()).string(),data,&error))return 1;
        const saida::AnimationClip* seatedClip=nullptr;
        for(const auto& clip:data.clips)if(clip->name()=="sit")seatedClip=clip.get();
        if(!seatedClip)return 1;
        const float seatDrop=(entry.at("seat").at("pelvisHeight").get<float>()-.14f)*scale;
        for(bool cross:{false,true})check(*data.rigs.front(),seatedClip,seatDrop,entry.at("name"),cross,999);
    }
    if(passed)std::puts("PASS motorcycle solved palms: player driving/9 aim directions and every preloaded male/female traffic rig");
    return passed?0:1;
}
