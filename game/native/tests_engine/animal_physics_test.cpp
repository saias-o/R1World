#include "animal_physics.hpp"
#include "bird_animation.hpp"
#include "behaviours/RagdollBehaviour.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/Scene.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "physics/StaticBodyNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <set>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool pass,const std::string& message) {
    if(!pass){std::fprintf(stderr,"FAIL animal-physics: %s\n",message.c_str());std::exit(1);}
}
void step(saida::Scene& scene,int frames){for(int i=0;i<frames;++i)scene.update(1.f/60.f);}
void ground(saida::Scene& scene) {
    auto* floor=scene.createChild<saida::StaticBodyNode>();floor->transform().position={0,-.5f,0};
    auto* shape=floor->createChild<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Box;shape->halfExtents={100,.5f,100};
}
}
int main() {
    const auto game=std::filesystem::path(__FILE__).parent_path()/"../..";
    const auto measured=nlohmann::json::parse(std::ifstream(game/"assets/animals/animals.json"));
    const auto fauna=nlohmann::json::parse(std::ifstream(game/"assets/animals/fauna.json"));
    std::set<std::string> used;
    std::map<std::string,float> flying;
    for(const auto& s:fauna.at("species"))if(s.contains("bird")&&s.at("bird").value("flies",true))
        for(const auto& m:s.at("models"))flying[m.at("model").get<std::string>()]=s.at("bird").value("flapHz",4.f);
    for(const auto& s:fauna.at("species"))for(const char* field:{"models","indoorModels"})
        if(s.contains(field))for(const auto& m:s.at(field))used.insert(m.at("model").get<std::string>());
    size_t count=0,articulated=0;
    for(const auto& model:used) {
        const auto& m=measured.at("models").at(model);
        saida::GltfAnimationData assets;std::string error;
        require(saida::GLTFLoader::loadAnimationData((game/m.at("lod0").get<std::string>()).string(),assets,&error),model+": "+error);
        require(!assets.rigs.empty(),model+": has a rig");auto& rig=*assets.rigs.front();
        saida::Scene scene;scene.ensurePhysics();ground(scene);
        const auto profile=r1::animalRagdoll(rig,m.at("length"),m.at("width"),m.at("height"),m.at("forward"));
        saida::Ragdoll fitted(rig,profile);
        require(fitted.valid(),model+": "+fitted.error());
        require(profile.parts.size()<=32,model+": bounded body count");
        if(profile.parts.size()>1)++articulated;
        auto* root=scene.createChild<saida::Node>();root->transform().position.y=-m.at("ground").get<float>()+.05f;
        auto* visual=root->createChild<saida::Node>();visual->transform()=r1::animalTransform(rig.rootParent());
        auto* animator=visual->addBehaviour<saida::Animator>();animator->setRig(&rig);
        r1::BirdPose* birdPose=nullptr;
        if(flying.count(model)) {
            root->transform().position.y+=12.f;
            birdPose=animator->addModifier<r1::BirdPose>(rig,m.at("forward").get<float>());
            birdPose->setFlight(true,1,flying.at(model),8,42);
        }
        for(size_t i=0;i<assets.clips.size();++i)animator->addClip(assets.clipNames[i],assets.clips[i].get());
        for(const char* role:{"walk","idle","eat","fly"})if(m.at("roles").contains(role)&&!m.at("roles").at(role).empty()) {
            animator->play(m.at("roles").at(role).front().get<std::string>());break;
        }
        auto* collider=root->createChild<saida::RigidBodyNode>();collider->kinematic=true;
        auto* box=collider->createChild<saida::CollisionShapeNode>();box->shapeType=saida::CollisionShapeType::Box;
        box->halfExtents={.01f,.01f,.01f};
        auto* fall=visual->addBehaviour<saida::RagdollBehaviour>(profile);
        step(scene,31);
        const auto before=animator->globalPose();
        const auto object=visual->worldTransform();
        require(fall->activateReplacing(*collider),model+": activates from the displayed skeleton frame");
        if(birdPose)birdPose->stop();
        require(collider->bodyId().IsInvalid(),model+": living collider is suspended");
        for(size_t p=0;p<profile.parts.size();++p) {
            const int bone=rig.findBoneIndex(profile.parts[p].bone);
            const auto expected=glm::vec3(object*before.globalMatrices[size_t(bone)][3]);
            require(glm::distance(expected,fall->ragdoll()->bonePosition(p))<.003f,model+": physics matches GLB units at every joint");
        }
        step(scene,360);
        if(fall->simulating())step(scene,360);
        require(fall->state()==saida::RagdollBehaviour::State::Frozen,model+": corpse settles automatically within twelve seconds");
        const auto rootAt=glm::vec3(visual->worldTransform()[3]);
        require(std::isfinite(rootAt.y)&&rootAt.y>-float(m.at("height").get<double>())-1.f,model+": remains above the floor after falling");
        require(fall->state()!=saida::RagdollBehaviour::State::Animated,model+": death never returns to locomotion");
        // Freeze, then simulate an off-centre follow-up shot and a blast.
        fall->freeze();step(scene,1);
        const auto held=animator->globalPose().globalMatrices;
        step(scene,120);
        require(held==animator->globalPose().globalMatrices,model+": resting corpse keeps exactly the same pose");
        nlohmann::json saved=nlohmann::json::array();
        for(const auto& t:r1::animalLocalPose(rig,animator->globalPose()).localTransforms)saved.push_back(r1::animalTransformJson(t));
        const auto reloaded=nlohmann::json::parse(saved.dump());
        auto* restored=scene.createChild<saida::Node>();restored->transform()=r1::animalReadTransform(r1::animalTransformJson(visual->transform()));
        auto* savedAnimator=restored->addBehaviour<saida::Animator>();savedAnimator->setRig(&rig);
        auto* rest=savedAnimator->addModifier<r1::AnimalRestPose>();
        for(const auto& t:reloaded)rest->pose.localTransforms.push_back(r1::animalReadTransform(t));
        savedAnimator->onUpdate(0);
        for(size_t b=0;b<rig.boneCount();++b)require(glm::distance(glm::vec3(object*held[b][3]),
            glm::vec3(object*savedAnimator->globalPose().globalMatrices[b][3]))<.004f,model+": saved local pose restores the same fallen skeleton");
        auto* savedFall=restored->addBehaviour<saida::RagdollBehaviour>(profile);
        scene.update(0);
        require(savedFall->activate(),model+": restored corpse can return to physics without a standing animation");
        savedFall->freeze();scene.removeChild(restored);
        const auto launch=r1::animalBlastVelocity(profile.mass,1.f,{1,0,0});
        require(fall->activateReplacing(*collider,launch),model+": frozen corpse can be blasted again");
        const auto start=fall->ragdoll()->rootPosition();
        step(scene,10);
        if(profile.mass<150.f)require(fall->ragdoll()->rootPosition().y>start.y,model+": nearby blast launches a light body upward");
        const auto preRebase=fall->ragdoll()->rootPosition();
        const glm::vec3 translation(35,2,-70);const auto rotation=glm::angleAxis(.35f,glm::vec3(0,1,0));
        scene.rebaseSubtree(*root,translation,rotation);
        require(glm::distance(fall->ragdoll()->rootPosition(),rotation*preRebase+translation)<.004f,model+": geographic rebase carries corpse physics");
        step(scene,1);fall->freeze();step(scene,1);
        // Removing a wildlife/tile sibling cannot destroy an independently
        // owned corpse. No renderer, window or GPU is created in this test.
        auto* tile=scene.createChild<saida::Node>();scene.removeChild(tile);
        require(root->parent()==&scene&&fall->state()==saida::RagdollBehaviour::State::Frozen,model+": corpse ownership survives tile removal");
        ++count;
    }
    const auto light=r1::animalBlastVelocity(2.f,1.f,{1,0,0});
    const auto heavy=r1::animalBlastVelocity(4000.f,1.f,{1,0,0});
    require(glm::length(light)>glm::length(heavy)*20,"blast respects body mass");
    require(glm::length(r1::animalBlastVelocity(2,14,{1,0,0}))==0,"blast ends at the gameplay radius");
    bool invalid=false;try{auto bad=r1::animalTransformJson(saida::Transform{});bad["q"]={0,0,0,0};r1::animalReadTransform(bad);}catch(...){invalid=true;}
    require(invalid,"corrupted snapshot rotation is refused");
    saida::MaterialDesc living;living.baseColor={.7f,.5f,.3f,.65f};living.alphaCutoff=.4f;living.normalId=42;living.albedoId=17;
    const auto burned=r1::charredAnimalMaterial(living);
    require(living.baseColor.r==.7f&&living.roughness==.5f,"charring leaves the shared living material unchanged");
    require(burned.baseColor.r<.03f&&burned.baseColor.a==living.baseColor.a&&burned.alphaCutoff==living.alphaCutoff&&burned.normalId==42&&burned.albedoId==17,"charred variant preserves fur cutouts and texture detail");
    std::printf("PASS animal-physics: %zu shipped models, %zu articulated profiles; animated falls, automatic rest, JSON pose restore, blast, rebase and materials (no window)\n",count,articulated);
}
