#include "session.hpp"

namespace r1::world {
saida::CollisionObjectNode* World::Impl::aircraftCollider(saida::Node* node) {
    return node?dynamic_cast<saida::CollisionObjectNode*>(node->findByPath("Aircraft hull")):nullptr;
}

void World::Impl::armAircraft(saida::Node& node,const r1::AircraftType& type) {
    if(aircraftCollider(&node))return;
    auto body=std::make_unique<saida::RigidBodyNode>();body->setName("Aircraft hull");body->kinematic=true;
    auto shape=std::make_unique<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Box;
    shape->halfExtents={float(fuselageHalf(type)),float(type.height*.32),float(type.length*.36)};
    shape->offset={0,float(type.height*.45),0};body->addChild(std::move(shape));node.addChild(std::move(body));
}

void World::Impl::buildRockets() {
    auto held=r1::buildRocketModel(engine.resources(),true);
    held->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 R Hand",pistolGrip);
    launcher=playerBody->addChild(std::move(held));launcher->setVisible(false);
    auto& world=engine.sceneTree().world();rocketEffects.build(world);
    for(auto& r:rockets) {
        r.mesh=world.addChild(r1::buildRocketModel(engine.resources(),false));r.mesh->setEnabled(false);
        r.trail=world.createChild<saida::ParticleSystemNode>();auto& e=*r.trail;
        e.setName("SaidaFX rocket exhaust");e.effectClass=saida::ParticleSystemNode::EffectClass::Smoke;
        e.maxParticles=100;e.spawnRate=65;e.lifetime=1.25f;e.startSize=.18f;e.endSizeScale=7;
        e.startColor={.45f,.30f,.12f,.65f};e.endColor={.16f,.16f,.15f,0};
        e.blendMode=saida::ParticleSystemNode::BlendMode::Alpha;e.emissive=.7f;
        e.gravity={0,.4f,0};e.drag=2;e.radius=.08f;e.useInitialVelocity=true;e.playing=false;
        r.flame=world.createChild<saida::ParticleSystemNode>();auto& f=*r.flame;
        f.setName("SaidaFX rocket motor");f.effectClass=saida::ParticleSystemNode::EffectClass::Fire;
        f.maxParticles=24;f.spawnRate=100;f.lifetime=.15f;f.startSize=.28f;f.endSizeScale=.15f;
        f.startColor={1,.8f,.32f,1};f.endColor={1,.12f,.015f,0};f.emissive=9;f.gravity={0,0,0};
        f.radius=.06f;f.useInitialVelocity=true;f.playing=false;
    }
}

void World::Impl::clearRockets() {
    for(auto& r:rockets){r.life=r.rest=0;if(r.mesh)r.mesh->setEnabled(false);if(r.trail)r.trail->stop();if(r.flame)r.flame->stop();}
    rocketEffects.clear();blastShake=0;
}

bool World::Impl::rocketRoom()const {
    return std::any_of(rockets.begin(),rockets.end(),[](const auto& r){return r.life<=0&&r.rest<=0;});
}

bool World::Impl::launchRocket(glm::vec3 muzzle,glm::vec3 target,saida::QueryFilter filter,bool police) {
    if(glm::length(target-muzzle)<.01f)return false;
    for(auto& r:rockets)if(r.life<=0&&r.rest<=0) {
        r.position=muzzle;r.direction=glm::normalize(target-muzzle);r.filter=filter;r.police=police;r.life=r1::rocket::kLifetime;
        r.mesh->transform().position=muzzle;r.mesh->transform().rotation=glm::quat(glm::vec3(0,0,1),r.direction);
        r.mesh->setEnabled(true);r.trail->transform().position=muzzle;r.trail->initialVelocity=-r.direction*5.f;r.trail->play();
        r.flame->transform().position=muzzle-r.direction*.7f;r.flame->initialVelocity=-r.direction*14.f;r.flame->play();
        effects.muzzle(muzzle);sounds.play(r1::CombatSounds::RocketLaunch,glm::distance(muzzle,camera->transform().position));
        ++rocketLaunches;return true;
    }
    return false;
}

void World::Impl::fireRocket() {
    if(!rocketRoom()){text("stream-status","Trop de roquettes en vol — patientez.");return;}
    const auto result=arsenal.pull();
    if(result==r1::combat::Arsenal::Trigger::Empty){sounds.play(r1::CombatSounds::DryFire);return;}
    if(result!=r1::combat::Arsenal::Trigger::Fired)return;
    const auto look=camera->transform().rotation*glm::vec3(0,0,-1);
    const auto eye=camera->transform().position;
    auto filter=combatFilter();
    const auto chest=player->transform().position+glm::vec3(0,float(playerHeight*.8),0);
    auto muzzle=launcher?glm::vec3(launcher->worldTransform()*glm::vec4(r1::kLauncherMuzzle,1)):chest+look*.7f;
    if(piloting()&&plane.node) {
        // Belly launch avoids the rotor and keeps the launch visible from the chase camera.
        muzzle=nodePosition(*plane.node)+plane.node->transform().rotation*glm::vec3(1.6f,.1f,-1.5f);
        if(auto* hull=aircraftCollider(plane.node))filter.ignore=hull->bodyId();
    }
    auto target=eye+look*float(r1::combat::kRocket.range);
    if(auto* physics=engine.sceneTree().world().physics()) {
        const auto start=piloting()?nodePosition(*plane.node):chest;
        const auto barrel=muzzle-start;const float reach=glm::length(barrel);
        if(reach>.01f) {
            const auto obstruction=physics->raycast(start,barrel/reach,reach,filter);
            if(obstruction.hit)muzzle=obstruction.point-barrel/reach*.08f;
        }
        const auto hit=physics->raycast(eye,look,float(r1::combat::kRocket.range),filter);if(hit.hit)target=hit.point;
    }
    // Do not send a shot backwards when the chase camera is behind cover.
    if(glm::dot(target-muzzle,look)<1)target=muzzle+look*2.f;
    launchRocket(muzzle,target,filter,false);recoil=.6;
    alarm(muzzle,120,12);reportCrime(r1::police::Crime::Gunshot,policePlayerPosition());
}

glm::vec3 World::Impl::rocketTarget()const {
    glm::vec3 velocity(0);
    if(driving()&&car)if(auto* body=r1::VehicleRuntime::body(*car);body&&body->physicsWorld())velocity=body->physicsWorld()->linearVelocity(body->bodyId());
    if(piloting()&&plane.node)velocity=plane.node->transform().rotation*glm::vec3(0,0,float(-plane.speed))+glm::vec3(0,float(plane.climb),0);
    return velocity;
}

bool World::Impl::policeRocket(glm::vec3 muzzle,saida::CollisionObjectNode* shooter,bool aerial) {
    // Stagger the whole response, so independently ready patrols cannot
    // deliver a simultaneous salvo. At 100% every attack is still a rocket.
    if(policeRocketSpacing>0)return false;
    const auto target=policePlayerPosition()+glm::vec3(0,.9f,0),delta=target-muzzle,velocity=rocketTarget();
    const float lead=float(r1::rocket::intercept(glm::dot(delta,delta),glm::dot(delta,velocity),glm::dot(velocity,velocity)));
    const auto aim=target+velocity*lead;
    // Recheck the actual predicted path just before firing, including friendly units.
    if(!policeSight(muzzle,aim,shooter,aerial?250:130))return false;
    for(const auto& unit:policeUnits)if(unit.car) {
        if(glm::distance(nodePosition(*unit.car),aim)<r1::rocket::kBlastRadius)return false;
        for(const auto& o:unit.officers)if(unit.dismounted&&o.health>0&&o.person.node&&glm::distance(nodePosition(*o.person.node),aim)<r1::rocket::kBlastRadius)return false;
    }
    saida::QueryFilter filter;if(shooter){filter.ignore=shooter->bodyId();filter.ignoreInner=shooter->innerBodyId();}
    if(!launchRocket(muzzle,aim,filter,true))return false;
    policeRocketSpacing=wanted.percent>=100?1.5:3.;
    return true;
}

void World::Impl::retireRocketVehicle(saida::Node* node) {
    if(!node)return;
    node->traverse([](saida::Node& n,const glm::mat4&){if(auto* body=n.asCollisionObject())body->detachFromPhysics();});
    node->setEnabled(false);node->queueFree();++rocketDestroyed;
}

void World::Impl::destroyRocketCar(saida::Node* node) {
    if(node==car) {
        if(driving())leaveCar();
        policeCarImpact.disconnect();policeImpactBody=nullptr;
        car=nullptr;carParked=carSinking=false;carSinkDepth=0;carSpeed=0;frontWheels.clear();rearWheels.clear();
    }
    parked.erase(std::remove_if(parked.begin(),parked.end(),[&](const auto& p){return p.node==node;}),parked.end());
    for(auto& [key,t]:loaded)for(size_t i=0;i<t.cars.size();++i)if(t.cars[i]==node){t.flow.retire(i);t.cars[i]=nullptr;t.carIdentities[i]=0;}
    for(auto& unit:policeUnits)if(unit.car==node) {
        if(!rocketPoliceDamage)reportCrime(r1::police::Crime::OfficerAssault,nodePosition(*node),true);
        node->traverse([](saida::Node& n,const glm::mat4&){if(auto* body=n.asCollisionObject())body->detachFromPhysics();});
        policeRemove(unit);++rocketDestroyed;return;
    }
    retireRocketVehicle(node);
}

void World::Impl::destroyRocketAircraft(saida::Node* node) {
    if(piloting()&&plane.node==node) {
        // Forced ejection cannot fail because the ground is not streamed.
        const auto at=nodePosition(*node)+glm::vec3(0,2,0);
        plane=Aircraft{};
        const auto geo=sceneToGeo(at);lon=geo.x;lat=geo.y;
        const double ground=height(lon,lat);alt=std::isfinite(ground)?ground:geo.z;
        jumpOffset=std::max(0.,geo.z-alt);jumpVelocity=0;lookYaw=lookIdle=0;
        transitionTravel(TravelMode::Foot,false);
    }
    leftAircraft.erase(std::remove_if(leftAircraft.begin(),leftAircraft.end(),[&](const auto& a){return a.node==node;}),leftAircraft.end());
    if(node==policeHelicopter) {
        if(!rocketPoliceDamage)reportCrime(r1::police::Crime::OfficerAssault,nodePosition(*node),true);
        policeHelicopter=nullptr;policeSearchlight=nullptr;policeMainRotors.clear();policeTailRotors.clear();
        saida::AudioManager::get().stop(policeRotorSound);policeRotorSound=saida::kInvalidAudioID;
        policeAirRespawn=20;
    }
    for(auto& [key,t]:loaded)for(auto& a:t.aircraft)if(a.node==node){a.node=nullptr;a.taken=true;}
    retireRocketVehicle(node);
}

bool World::Impl::rocketExposed(glm::vec3 at,glm::vec3 point,saida::CollisionObjectNode* body,const saida::Ragdoll* ragdoll) {
    const auto delta=point-at;const float d=glm::length(delta);if(d<.15f)return true;
    auto* physics=engine.sceneTree().world().physics();if(!physics)return false;
    // Keep the struck wall in the query: its far side remains sheltered.
    const auto hit=physics->raycast(at+delta/d*.02f,delta/d,d-.02f);
    return !hit.hit||hit.distance>=d-.05f||(body&&(hit.body==body->bodyId()||hit.body==body->innerBodyId()))||
        (ragdoll&&ragdoll->partOf(hit.body)>=0);
}

void World::Impl::explodeRocket(glm::vec3 at,saida::CollisionObjectNode* direct,bool police) {
    ++rocketImpacts;rocketPoliceDamage=police;rocketEffects.explode(at);
    sounds.play(r1::CombatSounds::RocketExplosion,glm::distance(at,camera->transform().position),1.4f);
    blastShake=std::max(blastShake,.55*std::max(0.,1.-glm::distance(at,camera->transform().position)/65.));
    alarm(at,140,14);
    blastAnimals(at,direct);
    auto exposed=[&](glm::vec3 point,saida::CollisionObjectNode* body) {
        return rocketExposed(at,point,body);
    };
    std::vector<saida::Node*> cars,aircraft;
    auto candidate=[&](saida::Node* n,bool air) {
        if(!n||!n->enabled())return;
        auto* body=air?aircraftCollider(n):r1::VehicleRuntime::body(*n);
        const auto p=nodePosition(*n)+glm::vec3(0,.8f,0);
        if((direct&&body==direct)||(glm::distance(p,at)<r1::rocket::kDestroyRadius&&exposed(p,body)))
            (air?aircraft:cars).push_back(n);
    };
    candidate(car,false);for(auto& p:parked)candidate(p.node,false);
    for(auto& u:policeUnits)candidate(u.car,false);
    for(auto& [key,t]:loaded){for(auto* c:t.cars)candidate(c,false);for(auto& a:t.aircraft)candidate(a.node,true);}
    candidate(plane.node,true);candidate(policeHelicopter,true);for(auto& a:leftAircraft)candidate(a.node,true);
    // Resolve people before freeing any body referenced by the visibility query.
    for(auto& [key,t]:loaded)for(size_t i=0;i<t.people.size()&&i<t.crowd.walkers().size();++i) {
        auto& p=t.people[i];if(!p.node||!p.node->enabled()||t.crowd.walkers()[i].dead)continue;
        const auto point=nodePosition(*p.node)+glm::vec3(0,.8f,0);const double damage=r1::rocket::damage(glm::distance(point,at));
        if(damage>0&&exposed(point,p.body))strikePerson(t,i,damage,glm::normalize(point-at+glm::vec3(0,.1f,0)),point,float(damage*5),true);
    }
    for(auto& u:policeUnits)if(u.dismounted)for(auto& o:u.officers)if(o.health>0&&o.person.node) {
        const auto point=nodePosition(*o.person.node)+glm::vec3(0,.8f,0);const double damage=r1::rocket::damage(glm::distance(point,at));
        if(damage>0&&exposed(point,o.person.body)) {
            saida::RaycastHit hit;hit.hit=true;hit.body=o.person.body->innerBodyId();hit.point=point;
            hitPolice(hit,damage,glm::normalize(point-at+glm::vec3(0,.1f,0)),false,float(damage*5));
        }
    }
    const auto playerAt=policePlayerPosition()+glm::vec3(0,.8f,0);
    const bool ejected=(driving()&&std::find(cars.begin(),cars.end(),car)!=cars.end())||
        (piloting()&&std::find(aircraft.begin(),aircraft.end(),plane.node)!=aircraft.end());
    const double playerDamage=r1::rocket::damage(glm::distance(playerAt,at));
    const bool playerExposed=ejected||exposed(playerAt,feet);
    std::sort(cars.begin(),cars.end(),std::less<saida::Node*>());cars.erase(std::unique(cars.begin(),cars.end()),cars.end());
    for(auto* c:cars)destroyRocketCar(c);
    std::sort(aircraft.begin(),aircraft.end(),std::less<saida::Node*>());aircraft.erase(std::unique(aircraft.begin(),aircraft.end()),aircraft.end());
    for(auto* a:aircraft)destroyRocketAircraft(a);
    if(playerExposed&&(playerDamage>0||ejected)) {
        const auto push=glm::normalize(playerAt-at+glm::vec3(0,.3f,0));
        // An occupied vehicle absorbs part of the blast, leaving a recoverable ejection.
        hurtPlayer(ejected?55:playerDamage,push,police?"a police rocket":"a rocket explosion");
        if(ejected&&!playerDown)knockPlayerDown(push*7.f+glm::vec3(0,4,0),{0,2,0},false);
        if(ejected)text("stream-status","Véhicule détruit — vous avez été éjecté !");
    }
    rocketPoliceDamage=false;
    saida::Log::info("[World rocket] explosion #",rocketImpacts," destroyed=",rocketDestroyed," ejected=",ejected);
}

void World::Impl::updateRockets(double elapsed) {
    const float dt=float(std::clamp(elapsed,0.,.1));rocketEffects.update(dt);blastShake=std::max(0.,blastShake-dt);
    auto* physics=engine.sceneTree().world().physics();if(!physics)return;
    for(auto& r:rockets) {
        r.rest=std::max(0.f,r.rest-dt);if(r.life<=0)continue;
        // Sweep the whole frame's path, never just the endpoint: thin walls
        // cannot be skipped at low frame rates. Collision resolves exactly once.
        const float distance=r1::rocket::kSpeed*std::min(dt,r.life);
        const auto hit=physics->raycast(r.position,r.direction,distance,r.filter);
        r.life-=dt;
        if(hit.hit||r.life<=0) {
            r.life=0;r.rest=1.3f;r.mesh->setEnabled(false);r.trail->stop();r.flame->stop();
            if(hit.hit)explodeRocket(hit.point+hit.normal*.15f,static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body)),r.police);
            continue; // expiry in the sky is harmless
        }
        r.position+=r.direction*distance;r.mesh->transform().position=r.position;
        r.trail->transform().position=r.position-r.direction*.7f;
        r.flame->transform().position=r.position-r.direction*.7f;
    }
}

} // namespace r1::world
