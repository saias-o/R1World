#include "session.hpp"
#include "photo_detection.hpp"
#include "scene/SceneMeasure.hpp"

namespace r1::world {
std::vector<PhotoAnimal> World::Impl::photographedAnimals() {
    auto* physics=engine.sceneTree().world().physics();
    if(!physics)return {};
    int width=0,height=0;engine.window().framebufferSize(width,height);
    if(width<=0||height<=0)return {};
    const auto eye=camera->transform().position;
    saida::Camera view;view.position=eye;
    view.lookAt(eye+camera->transform().rotation*glm::vec3(0,0,-1));
    view.setPerspective(glm::radians(camera->fovDegrees),float(width)/height,camera->nearZ,camera->farZ);
    const auto clip=view.projection()*view.view();
    const auto filter=combatFilter();
    std::vector<PhotoSubject> subjects;
    const auto add=[&](const Beast& beast,const r1::Species* species) {
        if(!species||!beast.visual||!beast.visual->isActiveInHierarchy()||!beast.visual->isVisibleInHierarchy())return;
        const auto body=beast.body?beast.body->bodyId():JPH::BodyID();
        const auto visible=[physics,eye,filter,body](glm::vec3 point) {
            const auto delta=point-eye;const float distance=glm::length(delta);
            if(distance<.001f)return false;
            const auto hit=physics->raycast(eye,delta/distance,distance,filter);
            return !hit.hit||hit.body==body||hit.distance>=distance-.01f;
        };
        std::function<void(const saida::Node&)> collect=[&](const saida::Node& node) {
            if(!node.isActiveInHierarchy()||!node.isVisibleInHierarchy())return;
            if(const auto* mesh=dynamic_cast<const saida::MeshNode*>(&node);mesh&&mesh->meshEnabled()&&mesh->mesh()&&mesh->mesh()->loaded())
                subjects.push_back({{species->id,species->name},mesh->mesh()->bounds(),node.worldTransform(),visible});
            for(const auto& child:node.children())collect(*child);
        };
        collect(*beast.visual);
    };
    for(const auto& [key,tile]:loaded) {
        const auto& animals=tile.herds.animals();
        for(size_t i=0;i<animals.size()&&i<tile.beasts.size();++i) {
            const auto& animal=animals[i];const auto& beast=tile.beasts[i];
            if(!animal.alive||animal.kind>=tile.faunaKinds.size()||!beast.shown||beast.identity!=animal.identity)continue;
            add(beast,tile.faunaKinds[animal.kind].species);
        }
        for(const auto& room:tile.interiors)for(const auto& indoor:room.beasts)
            if(!indoor.gone&&indoor.animal<room.animals.size())add(indoor.beast,r1::speciesById(room.animals[indoor.animal].species));
    }
    return detectPhotoAnimals(clip,width,height,subjects);
}

bool World::Impl::framePhotoSmokeAnimal() {
    auto* physics=engine.sceneTree().world().physics();if(!physics)return false;
    for(const auto& [key,tile]:loaded) {
        const auto& animals=tile.herds.animals();
        for(size_t i=0;i<animals.size()&&i<tile.beasts.size();++i) {
            const auto& beast=tile.beasts[i];
            if(!animals[i].alive||animals[i].kind>=tile.faunaKinds.size()||!beast.shown||!beast.visual||!beast.body||!beast.visual->isVisibleInHierarchy())continue;
            const auto bounds=saida::measure::measureSubtree(*beast.visual);
            if(!bounds.valid)continue;
            const auto target=bounds.center();
            const float distance=std::max(3.f,bounds.radius()*4);
            auto filter=combatFilter();filter.ignore=beast.body->bodyId();
            for(const auto offset:{glm::vec3(1,.4f,0),glm::vec3(-1,.4f,0),glm::vec3(0,.4f,1),glm::vec3(0,.4f,-1)}) {
                const auto direction=glm::normalize(offset);
                const auto hit=physics->raycast(target,direction,distance+.25f,filter);
                if(hit.hit)continue;
                camera->transform().position=target+direction*distance;
                camera->transform().rotation=glm::quatLookAt(-direction,glm::vec3(0,1,0));
                const auto detected=photographedAnimals();
                const auto* species=tile.faunaKinds[animals[i].kind].species;
                if(std::any_of(detected.begin(),detected.end(),[&](const auto& a){return a.species==species->id;})) {
                    saida::Log::info("[World photo E2E] framing live ",species->id," with the production visibility detector");return true;
                }
            }
        }
    }
    return false;
}
} // namespace r1::world
