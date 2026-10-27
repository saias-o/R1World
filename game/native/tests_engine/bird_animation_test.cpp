#include "bird_animation.hpp"
#include "bird_habitat.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/AnimationClip.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <set>

static void require(bool ok,const std::string& what){if(!ok){std::fprintf(stderr,"FAIL bird animation: %s\n",what.c_str());std::exit(1);}}
int main() {
    const auto game=std::filesystem::path(__FILE__).parent_path()/"../..";
    const auto catalog=nlohmann::json::parse(std::ifstream(game/"assets/animals/fauna.json"));
    const auto measured=nlohmann::json::parse(std::ifstream(game/"assets/animals/animals.json")).at("models");
    size_t models=0,rigs=0,procedural=0;
    for(const auto& s:catalog.at("species"))if(s.value("class",std::string())=="bird")for(const auto& m:s.at("models")) {
        const auto id=m.at("model").get<std::string>();const auto& info=measured.at(id);
        require(info.at("sameRig").get<bool>(),id+": shared LOD rig");++models;
        for(const auto* level:{"lod0","lod1"}) {
            saida::GltfAnimationData data;std::string error;
            require(saida::GLTFLoader::loadAnimationData((game/info.at(level).get<std::string>()).string(),data,&error),id+": "+error);
            require(!data.rigs.empty(),id+": skeleton");const auto& rig=*data.rigs.front();++rigs;
            r1::BirdPose motion(rig,info.at("forward").get<float>());
            if(!s.at("bird").value("flies",true)) {
                require(info.at("roles").contains("walk")&&info.at("roles").contains("idle"),id+": grounded source animation");continue;
            }
            require(motion.valid(),id+": supported flight rig");++procedural;
            motion.setFlight(true,1,s.at("bird").value("flapHz",4.f),8,1234);
            saida::LocalPose rest;
            for(const auto& b:rig.bones())rest.localTransforms.push_back(b.restLocal);
            saida::LocalPose first,last;
            double movement=0;
            for(int frame=0;frame<180;++frame) {
                motion.update(1.f/60);auto pose=rest;motion.apply(pose,rig);
                for(size_t b=0;b<rig.boneCount();++b) {
                    const auto& t=pose.localTransforms[b];
                    require(std::isfinite(t.rotation.w)&&std::abs(glm::length(t.rotation)-1)<.0001f,id+": normalized finite rotation");
                    require(glm::distance(t.position,rig.bones()[b].restLocal.position)<1e-6f,id+": bone lengths preserved");
                    require(glm::distance(t.scale,rig.bones()[b].restLocal.scale)<1e-6f,id+": scale preserved");
                    if(frame>60&&!last.localTransforms.empty())movement+=1.-std::abs(glm::dot(t.rotation,last.localTransforms[b].rotation));
                }
                last=pose;if(frame==0)first=pose;
            }
            if(rig.findBoneIndex("Humerus_L")<0)require(movement>.001,id+": flight really articulates bones");
            motion.setFlight(false,0,4,0,1234);
            for(int frame=0;frame<180;++frame)motion.update(1.f/60);
            auto perch=rest;motion.apply(perch,rig);
            saida::GlobalPose global;global.computeFrom(perch,rig);
            require(global.globalMatrices.size()==rig.boneCount(),id+": composed perch pose");
            motion.stop();require(!motion.active(),id+": ragdoll stops live wing motion");
        }
    }
    require(models==14&&rigs==28,"all shipped bird variants and both LODs");
    require(r1::birdBranchPoint({.6f,2,0},{.6f,2,.2f},{.8f,2,0},{}).has_value(),"real upward branch accepted");
    require(!r1::birdBranchPoint({.6f,2,0},{.8f,2,0},{.6f,2,.2f},{}),"down-facing bark rejected");
    require(!r1::birdBranchPoint({0,0,0},{0,0,1},{1,0,0},{}),"ground triangle rejected");
    std::printf("PASS bird animation: %zu models, %zu imported LOD rigs, %zu flight poses; branch support and ragdoll handoff (no window)\n",models,rigs,procedural);
}
