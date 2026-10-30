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
int main(int argc,char** argv) {
    nlohmann::json previews=nlohmann::json::object();
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
            saida::LocalPose last;
            double movement=0;
            double wristMovement=0;
            for(int frame=0;frame<180;++frame) {
                motion.update(1.f/60);auto pose=rest;motion.apply(pose,rig);
                for(size_t b=0;b<rig.boneCount();++b) {
                    const auto& t=pose.localTransforms[b];
                    require(std::isfinite(t.rotation.w)&&std::abs(glm::length(t.rotation)-1)<.0001f,id+": normalized finite rotation");
                    require(glm::distance(t.position,rig.bones()[b].restLocal.position)<1e-6f,id+": bone lengths preserved");
                    require(glm::distance(t.scale,rig.bones()[b].restLocal.scale)<1e-6f,id+": scale preserved");
                    if(frame>60&&!last.localTransforms.empty()) {
                        const double change=1.-std::abs(glm::dot(t.rotation,last.localTransforms[b].rotation));
                        movement+=change;
                        if(rig.bones()[b].name=="wing_tip.L"||rig.bones()[b].name=="Wing.001.L"||rig.bones()[b].name=="Arm_l")wristMovement+=change;
                    }
                }
                if(frame>60&&rig.findBoneIndex("wing_tip.L")>=0) {
                    saida::GlobalPose flight;flight.computeFrom(pose,rig,rig.rootParent());
                    const auto left=glm::normalize(glm::vec3(flight.globalMatrices[size_t(rig.findBoneIndex("wing_tip.L"))][1]));
                    const auto right=glm::normalize(glm::vec3(flight.globalMatrices[size_t(rig.findBoneIndex("wing_tip.R"))][1]));
                    require(glm::distance(left,glm::vec3(-right.x,right.y,right.z))<.02f,id+": mirrored wing-tip pitch throughout the stroke");
                }
                last=pose;
            }
            if(rig.findBoneIndex("Humerus_L")<0)require(movement>.001,id+": flight really articulates bones");
            if(rig.findBoneIndex("wing_tip.L")>=0||rig.findBoneIndex("Wing.001.L")>=0||rig.findBoneIndex("Arm_l")>=0)
                require(wristMovement>.001,id+": wrist articulates independently of shoulder");
            motion.setFlight(true,0,4,8,1234);
            for(int frame=0;frame<180;++frame)motion.update(1.f/60);
            auto glide=rest;motion.apply(glide,rig);
            saida::GlobalPose spread;spread.computeFrom(glide,rig,rig.rootParent());
            // A flapping joint is not proof of an open wing: pigeons used to
            // roll their folded wings while both tips still pointed aft.
            if(rig.findBoneIndex("wing_upper.L")>=0)for(const char* side:{".L",".R"}) {
                const int shoulder=rig.findBoneIndex(std::string("wing_upper")+side);
                const int wrist=rig.findBoneIndex(std::string("wing_tip")+side);
                const auto upper=glm::normalize(glm::vec3(spread.globalMatrices[size_t(wrist)][3]-spread.globalMatrices[size_t(shoulder)][3]));
                const auto tip=glm::normalize(glm::vec3(spread.globalMatrices[size_t(wrist)][1]));
                const float sign=std::string(side)==".L"?-1.f:1.f;
                require(sign*upper.x>.9f,id+": shoulder opens sideways in glide");
                require(sign*tip.x>.85f,id+": wrist opens outward instead of staying folded aft");
            }
            motion.update(.137f);auto still=rest;motion.apply(still,rig);
            for(size_t b=0;b<rig.boneCount();++b)if(rig.bones()[b].name.find("wing")!=std::string::npos||rig.bones()[b].name.find("Wing")!=std::string::npos)
                require(std::abs(glm::dot(glide.localTransforms[b].rotation,still.localTransforms[b].rotation))>1.f-1e-5f,id+": glide stops flapping");
            if(rig.findBoneIndex("Humerus_L")>=0&&info.at("roles").contains("fly")) {
                const auto name=info.at("roles").at("fly").front().get<std::string>();
                for(size_t c=0;c<data.clips.size();++c)if(data.clipNames[c]==name)for(int frame=0;frame<16;++frame) {
                    auto source=rest;
                    for(size_t b=0;b<rig.boneCount();++b)if(const auto* tracks=data.clips[c]->getTracks(rig.bones()[b].name))
                        for(const auto& track:*tracks)track->evaluate(data.clips[c]->duration()*float(frame)/16.f,source.localTransforms[b]);
                    motion.apply(source,rig);
                    for(size_t b=0;b<rig.boneCount();++b) {
                        const auto& bone=rig.bones()[b].name;
                        if(bone.find("Humerus_")==0||bone.find("Ulna_")==0||bone.find("Digit_")==0)
                            require(std::abs(glm::dot(source.localTransforms[b].rotation,glide.localTransforms[b].rotation))>1.f-1e-5f,id+": source flight clip cannot flap during glide");
                    }
                }
            }
            // Cadence changes preserve the phase even after a long session.
            motion.setFlight(true,1,4,8,1234);motion.update(3600);
            auto beforeRate=rest;motion.apply(beforeRate,rig);
            motion.setFlight(true,1,7,8,1234);auto afterRate=rest;motion.apply(afterRate,rig);
            for(size_t b=0;b<rig.boneCount();++b)
                require(std::abs(glm::dot(beforeRate.localTransforms[b].rotation,afterRate.localTransforms[b].rotation))>1.f-1e-5f,id+": cadence change is continuous");
            if(argc>1&&std::string(level)=="lod0") {
                auto& preview=previews[id];preview["asset"]=info.at(level);
                for(const auto& bone:rig.bones())preview["bones"].push_back(bone.name);
                const float hz=s.at("bird").value("flapHz",4.f);
                r1::BirdPose sample(rig,info.at("forward").get<float>());
                sample.setFlight(true,1,hz,8,0);sample.update(4.f/hz+3.f);
                for(int frame=0;frame<17;++frame) {
                    if(frame==16){sample.setFlight(true,0,hz,8,0);sample.update(3);}
                    auto pose=rest;
                    if(info.at("roles").contains("fly")) {
                        const auto clipName=info.at("roles").at("fly").front().get<std::string>();
                        for(size_t c=0;c<data.clips.size();++c)if(data.clipNames[c]==clipName)
                            for(size_t b=0;b<rig.boneCount();++b)if(const auto* tracks=data.clips[c]->getTracks(rig.bones()[b].name))
                                for(const auto& track:*tracks)track->evaluate(data.clips[c]->duration()*float(frame%16)/16.f,pose.localTransforms[b]);
                    }
                    sample.apply(pose,rig);saida::GlobalPose out;out.computeFrom(pose,rig,rig.rootParent());
                    nlohmann::json matrices=nlohmann::json::array();
                    for(const auto& matrix:out.skinningMatrices) {
                        nlohmann::json values=nlohmann::json::array();
                        for(int row=0;row<4;++row)for(int col=0;col<4;++col)values.push_back(matrix[col][row]);
                        matrices.push_back(values);
                    }
                    preview["frames"].push_back(matrices);sample.update(1.f/(16.f*hz));
                }
            }
            motion.setFlight(false,0,4,0,1234);
            for(int frame=0;frame<180;++frame)motion.update(1.f/60);
            auto perch=rest;motion.apply(perch,rig);
            if(rig.findBoneIndex("wing_upper.L")>=0)for(size_t b=0;b<rig.boneCount();++b)
                if(rig.bones()[b].name.find("wing_")==0)
                    require(std::abs(glm::dot(perch.localTransforms[b].rotation,rest.localTransforms[b].rotation))>1.f-1e-5f,id+": landing restores folded wings");
            saida::GlobalPose global;global.computeFrom(perch,rig);
            require(global.globalMatrices.size()==rig.boneCount(),id+": composed perch pose");
            motion.stop();require(!motion.active(),id+": ragdoll stops live wing motion");
        }
    }
    require(models==14&&rigs==28,"all shipped bird variants and both LODs");
    if(argc>1)std::ofstream(argv[1])<<previews.dump();
    require(r1::birdBranchPoint({.6f,2,0},{.6f,2,.2f},{.8f,2,0},{}).has_value(),"real upward branch accepted");
    require(!r1::birdBranchPoint({.6f,2,0},{.8f,2,0},{.6f,2,.2f},{}),"down-facing bark rejected");
    require(!r1::birdBranchPoint({0,0,0},{0,0,1},{1,0,0},{}),"ground triangle rejected");
    std::printf("PASS bird animation: %zu models, %zu imported LOD rigs, %zu flight poses; branch support and ragdoll handoff (no window)\n",models,rigs,procedural);
}
