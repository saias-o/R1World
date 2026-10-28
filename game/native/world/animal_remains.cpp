#include "session.hpp"
#include <windows.h>

namespace r1::world {
std::string World::Impl::animalResidentId(uint64_t group,size_t member) {
    return "resident:"+std::to_string(group)+":"+std::to_string(member);
}

std::string World::Impl::indoorAnimalId(const LiveInterior& room,size_t animal,int member) {
    const auto& a=room.animals[animal];
    return "room:"+std::to_string(room.plan.id)+":"+a.species+":"+std::to_string(animal)+":"+std::to_string(member);
}

json World::Impl::animalTransformJson(const saida::Transform& t) {
    return r1::animalTransformJson(t);
}

saida::Transform World::Impl::animalReadTransform(const json& j) {
    return r1::animalReadTransform(j);
}

void World::Impl::animalSaveError(const std::string& why) {
    if(animalSaveErrorSaid)return;animalSaveErrorSaid=true;
    saida::Log::error("[World fauna] remains persistence: ",why);
    text("stream-status","Animal body error: "+why);
    if(smoke)testFailed=true;
}

void World::Impl::placeAnimalRemains(AnimalRemains& c,const Frame& observer,bool rebase) {
    const auto position=glm::vec3(observer.local(c.frame.origin));
    const auto rotation=glm::normalize(glm::quat_cast(glm::mat3(glm::transpose(observer.basis)*c.frame.basis)));
    if(rebase)engine.sceneTree().world().rebaseSubtreeTo(*c.anchor,position,rotation);
    else {c.anchor->transform().position=position;c.anchor->transform().rotation=rotation;}
}

void World::Impl::rebaseAnimalRemains(const Frame& next) {
    for(auto& c:animalRemains)if(c.anchor)placeAnimalRemains(c,next,true);
}

void World::Impl::charAnimal(Beast& b) {
    b.node->traverse([&](saida::Node& n,const glm::mat4&) {
        if(auto* mesh=dynamic_cast<saida::MeshNode*>(&n);mesh&&mesh->material())
            mesh->setMaterial(engine.resources().getMaterial(r1::charredAnimalMaterial(mesh->material()->desc())));
    });
}

json World::Impl::snapshotAnimal(const AnimalRemains& c) const {
    if(!c.beast.node)return c.saved;
    const auto& rig=*c.beast.animator->rig();
    const auto pose=r1::animalLocalPose(rig,c.beast.animator->globalPose());
    json bones=json::array(),locals=json::array(),basis=json::array();
    for(const auto& b:rig.bones())bones.push_back(b.name);
    for(const auto& t:pose.localTransforms)locals.push_back(animalTransformJson(t));
    for(int i=0;i<3;++i)basis.push_back({c.frame.basis[i].x,c.frame.basis[i].y,c.frame.basis[i].z});
    auto velocity=c.resumeVelocity,spin=c.resumeSpin;
    if(c.fall&&c.fall->simulating()) {
        const auto rotation=glm::mat3(glm::transpose(c.frame.basis)*origin.basis);
        auto* physics=engine.sceneTree().world().physics();
        velocity=rotation*physics->linearVelocity(c.fall->ragdoll()->body(0));
        spin=rotation*physics->angularVelocity(c.fall->ragdoll()->body(0));
    }
    return {{"resident",c.resident},{"model",c.beast.model},{"charred",c.charred},
        {"origin",{c.frame.origin.x,c.frame.origin.y,c.frame.origin.z}},{"basis",basis},
        {"root",animalTransformJson(c.beast.node->transform())},{"visual",animalTransformJson(c.beast.visual->transform())},
        {"bones",bones},{"pose",locals},{"settled",!c.distant&&(!c.fall||!c.fall->simulating())},
        {"velocity",{velocity.x,velocity.y,velocity.z}},
        {"spin",{spin.x,spin.y,spin.z}}};
}

void World::Impl::saveAnimalRemains() {
    if(animalSaveBlocked||animalRemains.empty())return;
    try {
        json all=json::array();for(const auto& c:animalRemains)all.push_back(snapshotAnimal(c));
        const fs::path path=game/"cache/fauna-remains.json",temporary=game/"cache/fauna-remains.json.tmp";
        fs::create_directories(path.parent_path());
        {std::ofstream out(temporary,std::ios::binary|std::ios::trunc);out<<json{{"schema",1},{"remains",all}}.dump();out.flush();
         if(!out)throw std::runtime_error("cannot write "+temporary.string());}
        if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace "+path.string()+" (Windows "+std::to_string(GetLastError())+")");
    } catch(const std::exception& e){animalSaveError(e.what());}
}

void World::Impl::loadAnimalRemains() {
    const fs::path path=game/"cache/fauna-remains.json";
    if(!fs::exists(path))return;
    try {
        std::ifstream input(path);const json data=json::parse(input);
        if(data.at("schema")!=1||!data.at("remains").is_array())throw std::runtime_error("unsupported remains schema");
        std::vector<AnimalRemains> loadedRemains;std::set<std::string> dead;
        for(const auto& j:data.at("remains")) {
            AnimalRemains c;c.saved=j;c.resident=j.at("resident");c.charred=j.at("charred");
            c.beast.model=j.at("model");
            if(!dead.insert(c.resident).second)throw std::runtime_error("duplicate resident "+c.resident);
            for(int i=0;i<3;++i){c.frame.origin[i]=j.at("origin").at(i);for(int k=0;k<3;++k)c.frame.basis[i][k]=j.at("basis").at(i).at(k);}
            if(!std::isfinite(glm::length(c.frame.origin))||glm::length(c.frame.origin)<6e6||
               !std::isfinite(glm::determinant(c.frame.basis))||std::abs(glm::determinant(c.frame.basis)-1.)>.001)
                throw std::runtime_error("invalid remains frame");
            if(j.at("bones").empty()||j.at("bones").size()!=j.at("pose").size())throw std::runtime_error("incomplete remains pose");
            animalReadTransform(j.at("root"));animalReadTransform(j.at("visual"));
            for(const auto& t:j.at("pose"))animalReadTransform(t);
            for(int i=0;i<3;++i) {
                c.resumeVelocity[i]=j.value("velocity",json::array({0,0,0})).at(i);
                c.resumeSpin[i]=j.value("spin",json::array({0,0,0})).at(i);
                if(!std::isfinite(c.resumeVelocity[i])||!std::isfinite(c.resumeSpin[i]))throw std::runtime_error("invalid remains velocity");
            }
            loadedRemains.push_back(std::move(c));
        }
        animalRemains=std::move(loadedRemains);animalDeaths=std::move(dead);
        saida::Log::info("[World fauna] restored ",animalDeaths.size()," persistent animal deaths");
    } catch(const std::exception& e) {
        // Preserve a damaged or newer save verbatim; never replace it with an
        // empty document and silently erase the player's durable changes.
        animalSaveBlocked=true;animalSaveError(path.string()+": "+e.what());
    }
}

void World::Impl::buildAnimalRestBody(AnimalRemains& c) {
    if(c.restBody)return;
    auto body=std::make_unique<saida::StaticBodyNode>();body->setName("Animal remains collision");
    const auto& rig=*c.beast.animator->rig();const auto& pose=c.beast.animator->globalPose().globalMatrices;
    for(const auto& part:c.profile.parts) {
        const int bone=rig.findBoneIndex(part.bone),end=rig.findBoneIndex(part.end);
        const auto& m=pose[size_t(bone)];const glm::vec3 p(m[3]);
        const auto rotation=r1::animalTransform(m).rotation;
        glm::vec3 reach;
        if(end>=0)reach=glm::vec3(pose[size_t(end)][3])-p;
        else {
            const auto bindRotation=r1::animalTransform(glm::inverse(rig.bones()[size_t(bone)].inverseBindMatrix)).rotation;
            reach=rotation*glm::inverse(bindRotation)*part.axis*part.length;
        }
        const float length=glm::length(reach);if(length<1e-6f)continue;
        auto shape=std::make_unique<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Capsule;
        shape->radius=part.radius;shape->height=std::max(length,part.radius*2.f);
        shape->transform().position=p+reach*.5f;
        shape->transform().rotation=glm::quat(glm::vec3(0,1,0),reach/length);
        body->addChild(std::move(shape));
    }
    c.restBody=static_cast<saida::StaticBodyNode*>(c.beast.visual->addChild(std::move(body)));
}

void World::Impl::updateAnimalRemains() {
    bool changed=false;
    for(auto& c:animalRemains) {
        if(!c.anchor&&!c.refused) {
            // Saved bodies are mounted on approach; mounted bodies have no
            // timeout or distance retirement. This is independent of wildlife.
            if(glm::distance(ecef(lon,lat,alt),c.frame.origin)>2000.)continue;
            if(!animalKinds.count(c.beast.model)) {c.refused=true;animalSaveError("missing model "+c.beast.model);continue;}
            const auto root=animalReadTransform(c.saved.at("root"));
            const auto point=c.frame.origin+c.frame.basis*glm::dvec3(root.position);
            const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
            if(!tile(geo.x,geo.y))continue;
            auto* kind=animalKind(c.beast.model);if(!kind)continue;
            if(c.saved.at("bones").size()!=kind->rig->boneCount()) {
                c.refused=true;animalSaveError("skeleton changed for "+c.beast.model);continue;
            }
            bool same=true;for(size_t i=0;i<kind->rig->boneCount();++i)same=same&&c.saved.at("bones").at(i)==kind->rig->bones()[i].name;
            if(!same){c.refused=true;animalSaveError("bone order changed for "+c.beast.model);continue;}
            c.anchor=engine.sceneTree().world().createChild<saida::Node>("Persistent animal remains");placeAnimalRemains(c,origin);
            if(!makeBeast(*c.anchor,c.beast.model,root.scale.x,c.beast))continue;
            c.beast.node->transform()=root;c.beast.visual->transform()=animalReadTransform(c.saved.at("visual"));
            c.beast.body->setEnabled(false);
            auto* rest=c.beast.animator->addModifier<r1::AnimalRestPose>();
            for(const auto& t:c.saved.at("pose"))rest->pose.localTransforms.push_back(animalReadTransform(t));
            if(rest->pose.localTransforms.size()!=kind->rig->boneCount())throw std::runtime_error("incomplete animal remains pose");
            c.beast.animator->onUpdate(0);
            c.profile=r1::animalRagdoll(*kind->rig,float(kind->length),float(kind->width),float(kind->height),float(kind->forward),root.scale.x);
            c.fall=c.beast.visual->addBehaviour<saida::RagdollBehaviour>(c.profile);
            c.anchor->updateTransforms(engine.sceneTree().world().worldTransform(),true);
            if(c.charred)charAnimal(c.beast);
            if(c.saved.value("settled",true)){buildAnimalRestBody(c);c.stored=true;c.beast.animator->setEnabled(false);}
            else {
                const auto rotation=glm::mat3(glm::transpose(origin.basis)*c.frame.basis);
                if(!c.fall->activate(rotation*c.resumeVelocity,rotation*c.resumeSpin)) {
                    animalSaveError("cannot resume animal fall: "+c.beast.model);buildAnimalRestBody(c);c.stored=true;
                }
            }
        }
        if(!c.anchor||!c.fall)continue;
        if(c.distant) {
            const auto geo=sceneToGeo(glm::vec3(c.beast.visual->worldTransform()[3]));
            if(tile(geo.x,geo.y)) {
                const auto rotation=glm::mat3(glm::transpose(origin.basis)*c.frame.basis);
                wakeAnimalRemains(c,rotation*c.resumeVelocity,rotation*c.resumeSpin);
            }
        }
        if(c.fall->state()==saida::RagdollBehaviour::State::Frozen&&!c.stored) {
            buildAnimalRestBody(c);c.stored=true;changed=true;
            // Keep the last pose without ticking a graph and recomposing its
            // skeleton each frame. Re-enable before another physical impact.
            c.beast.animator->setEnabled(false);
        }
    }
    if(changed)saveAnimalRemains();
}

bool World::Impl::wakeAnimalRemains(AnimalRemains& c,const glm::vec3& velocity,const glm::vec3& spin) {
    if(c.fall->simulating())return true;
    c.beast.animator->setEnabled(true);
    // RagdollBehaviour retains the visible pose while Frozen. A restored
    // snapshot modifier supplies it after a restart instead of a live clip.
    if(!(c.restBody?c.fall->activateReplacing(*c.restBody,velocity,spin):c.fall->activate(velocity,spin)))return false;
    if(c.restBody){c.restBody->setEnabled(false);c.restBody->queueFree();c.restBody=nullptr;}
    c.stored=false;c.distant=false;return true;
}

void World::Impl::pauseDepartingAnimalRemains(const std::set<std::string>& kept) {
    bool changed=false;
    for(auto& c:animalRemains)if(c.fall&&c.fall->simulating()) {
        auto* ragdoll=c.fall->ragdoll();const auto geo=sceneToGeo(ragdoll->rootPosition());
        if(kept.count(tileAt(geo.x,geo.y).key()))continue;
        auto* physics=engine.sceneTree().world().physics();
        const auto rotation=glm::mat3(glm::transpose(c.frame.basis)*origin.basis);
        c.resumeVelocity=rotation*physics->linearVelocity(ragdoll->body(0));
        c.resumeSpin=rotation*physics->angularVelocity(ragdoll->body(0));
        c.fall->freeze();c.beast.animator->onUpdate(0);c.distant=true;c.stored=false;changed=true;
    }
    if(changed)saveAnimalRemains();
}

bool World::Impl::leaveAnimalBody(Beast& beast,const Frame& frame,const std::string& resident,
                    const glm::vec3& velocity,const glm::vec3& impulse,const glm::vec3& point,bool charred) {
    if(!beast.node||animalDeaths.count(resident))return false;
    auto* oldParent=beast.node->parent();beast.node->updateTransforms(oldParent->worldTransform(),true);
    const auto before=beast.node->worldTransform();const auto oldLocal=beast.node->transform();
    AnimalRemains c;c.frame=frame;c.resident=resident;c.charred=charred;
    c.anchor=engine.sceneTree().world().createChild<saida::Node>("Persistent animal remains");placeAnimalRemains(c,origin);
    c.anchor->updateTransforms(engine.sceneTree().world().worldTransform(),true);
    c.beast=beast;
    c.anchor->addChild(oldParent->detachChild(beast.node));
    beast.node->transform()=r1::animalTransform(glm::inverse(c.anchor->worldTransform())*before);
    c.anchor->updateTransforms(engine.sceneTree().world().worldTransform(),true);
    const auto& kind=animalKinds.at(beast.model);
    c.profile=r1::animalRagdoll(*kind.rig,float(kind.length),float(kind.width),float(kind.height),float(kind.forward),beast.node->transform().scale.x);
    beast.animator->setPoseRate(0);beast.animator->onUpdate(0);
    c.fall=beast.visual->addBehaviour<saida::RagdollBehaviour>(c.profile);
    const auto spin=charred?glm::cross(glm::vec3(0,1,0),velocity)*.12f:glm::vec3(0);
    if(!c.fall->activateReplacing(*beast.body,velocity,spin)) {
        beast.visual->removeBehaviour(c.fall);
        oldParent->addChild(c.anchor->detachChild(beast.node));beast.node->transform()=oldLocal;c.anchor->queueFree();
        animalSaveError("physics refused "+beast.model);return false;
    }
    beast.body->setEnabled(false);
    if(beast.birdPose)beast.birdPose->stop();
    if(glm::dot(impulse,impulse)>0) {
        const size_t part=c.fall->ragdoll()->nearestPart(point);
        c.fall->applyImpulse(glm::normalize(impulse)*r1::animalShotMomentum(c.profile,part,glm::length(impulse)),point);
    }
    if(charred)charAnimal(beast);
    animalDeaths.insert(resident);animalRemains.push_back(std::move(c));beast=Beast{};
    return true;
}

bool World::Impl::hitAnimal(const saida::RaycastHit& hit,const glm::vec3& direction,float momentum) {
    auto matches=[&](const Beast& b){return b.body&&!hit.body.IsInvalid()&&b.body->bodyId()==hit.body;};
    for(auto& [key,t]:loaded) {
        for(size_t i=0;i<t.beasts.size()&&i<t.herds.animals().size();++i) {
            auto& b=t.beasts[i];const auto a=t.herds.animals()[i];
            if(!a.alive||!matches(b))continue;
            const auto velocity=glm::vec3(glm::transpose(origin.basis)*t.frame.basis*glm::dvec3(std::sin(a.heading)*a.speed,a.verticalSpeed,std::cos(a.heading)*a.speed));
            const auto& k=animalKinds.at(b.model);const float mass=r1::animalRagdoll(*k.rig,float(k.length),float(k.width),float(k.height),float(k.forward),b.node->transform().scale.x).mass;
            if(leaveAnimalBody(b,t.frame,animalResidentId(t.herds.groups()[a.group].identity,a.member),velocity,
                               direction*std::min(momentum,mass*1.2f),hit.point,false))t.herds.setMemberEnabled(a.group,a.member,false);
            saveAnimalRemains();effects.burst(r1::CombatEffects::Blood,hit.point);return true;
        }
        for(auto& room:t.interiors)for(auto& ib:room.beasts)if(!ib.gone&&matches(ib.beast)) {
            if(leaveAnimalBody(ib.beast,t.frame,indoorAnimalId(room,ib.animal,ib.member),{},direction*std::min(momentum,.4f),hit.point,false))ib.gone=true;
            saveAnimalRemains();
            return true;
        }
    }
    for(auto& c:animalRemains)if(c.anchor) {
        const bool rest=c.restBody&&c.restBody->bodyId()==hit.body;
        const bool part=c.fall&&c.fall->ragdoll()&&c.fall->ragdoll()->partOf(hit.body)>=0;
        if(!rest&&!part)continue;
        if(wakeAnimalRemains(c))c.fall->applyImpulse(direction*r1::animalShotMomentum(c.profile,c.fall->ragdoll()->nearestPart(hit.point),momentum),hit.point);
        return true;
    }
    return false;
}

void World::Impl::blastAnimals(glm::vec3 at,saida::CollisionObjectNode* direct) {
    // Snapshot pointers before converting living slots: the vector of remains
    // may grow while the explosion is resolved, but each victim is hit once.
    const size_t old=animalRemains.size();
    struct Target {Beast* beast;Frame frame;std::string resident;glm::vec3 velocity,point;Loaded* tile;uint32_t group,member;IndoorBeast* indoor;};
    std::vector<Target> targets;
    std::vector<std::pair<size_t,glm::vec3>> corpses;
    auto blast=[&](Beast& b,const Frame& frame,const std::string& resident,glm::vec3 inherited,
                   Loaded* owner,uint32_t group,uint32_t member,IndoorBeast* indoor) {
        const auto& k=animalKinds.at(b.model);
        const auto point=glm::vec3(b.node->worldTransform()*glm::vec4(0,float(k.ground+k.height*.45),0,1));
        const bool struck=direct&&direct==b.body;
        const float distance=struck?0.f:glm::distance(point,at);
        if(distance>=r1::rocket::kBlastRadius||(!struck&&!rocketExposed(at,point,b.body)))return;
        const auto profile=r1::animalRagdoll(*k.rig,float(k.length),float(k.width),float(k.height),float(k.forward),b.node->transform().scale.x);
        const auto push=r1::animalBlastVelocity(profile.mass,distance,point-at);
        targets.push_back({&b,frame,resident,inherited+push,point,owner,group,member,indoor});
    };
    for(auto& [key,t]:loaded) {
        for(size_t i=0;i<t.beasts.size()&&i<t.herds.animals().size();++i) {
            auto& b=t.beasts[i];const auto a=t.herds.animals()[i];if(!a.alive||!b.node||!b.node->enabled())continue;
            const auto v=glm::vec3(glm::transpose(origin.basis)*t.frame.basis*glm::dvec3(std::sin(a.heading)*a.speed,a.verticalSpeed,std::cos(a.heading)*a.speed));
            blast(b,t.frame,animalResidentId(t.herds.groups()[a.group].identity,a.member),v,&t,a.group,a.member,nullptr);
        }
        for(auto& room:t.interiors)for(auto& ib:room.beasts)if(!ib.gone&&ib.beast.node)
            blast(ib.beast,t.frame,indoorAnimalId(room,ib.animal,ib.member),{},nullptr,0,0,&ib);
    }
    for(size_t i=0;i<old;++i) {
        auto& c=animalRemains[i];if(!c.anchor)continue;
        const auto point=c.fall->simulating()?c.fall->ragdoll()->partPosition(0):glm::vec3(c.beast.visual->worldTransform()*glm::vec4(c.beast.animator->globalPose().globalMatrices[size_t(c.beast.animator->rig()->findBoneIndex(c.profile.parts[0].bone))][3]));
        const float distance=glm::distance(point,at);
        if(distance>=r1::rocket::kBlastRadius||!rocketExposed(at,point,c.restBody,c.fall->ragdoll()))continue;
        const auto velocity=r1::animalBlastVelocity(c.profile.mass,distance,point-at);
        corpses.push_back({i,velocity});
    }
    // Visibility was resolved against the pre-blast world. A newly created
    // ragdoll must not shield the next victim from this same explosion.
    for(const auto& target:targets)
        if(leaveAnimalBody(*target.beast,target.frame,target.resident,target.velocity,{},target.point,true)) {
            if(target.tile)target.tile->herds.setMemberEnabled(target.group,target.member,false);
            if(target.indoor)target.indoor->gone=true;
        }
    for(const auto& [index,velocity]:corpses) {
        auto& c=animalRemains[index];
        if(!c.charred){charAnimal(c.beast);c.charred=true;}
        const bool moving=c.fall->simulating();
        const auto spin=moving?glm::vec3(0):glm::cross(glm::vec3(0,1,0),velocity)*.12f;
        if(wakeAnimalRemains(c,moving?glm::vec3(0):velocity,spin)&&moving&&c.fall->ragdoll()) {
            // Already simulating: distribute the blast momentum over every
            // part instead of concentrating a rocket's energy in one knee.
            float shares=0;for(const auto& p:c.profile.parts)shares+=p.massShare;
            for(size_t p=0;p<c.profile.parts.size();++p)c.fall->ragdoll()->applyImpulse(p,
                velocity*(c.profile.mass*c.profile.parts[p].massShare/shares),c.fall->ragdoll()->partPosition(p));
        }
    }
    saveAnimalRemains();
}

} // namespace r1::world
