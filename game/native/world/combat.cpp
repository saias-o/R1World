#include "session.hpp"

namespace r1::world {
void World::Impl::unifyPlayerBody() {
    if(animators.empty()||!animators.front()->rig())return;
    saida::Animator* source=animators.front();
    saida::Node* model=nullptr;
    for(auto& c:player->children())
        for(saida::Node* n=source->node();n&&!model;n=n->parent())if(n==c.get())model=c.get();
    if(!model)return;
    auto body=std::make_unique<saida::Node>("player-body");
    body->transform()=model->transform();
    auto* animator=body->addBehaviour<saida::Animator>();
    animator->setRig(const_cast<saida::Rig*>(source->rig()));
    for(const auto& [name,clip]:source->clips())animator->addClip(name,clip);
    std::function<void(saida::Node&,const saida::Transform&)> walk=[&](saida::Node& n,const saida::Transform& above) {
        saida::Transform here;
        here.rotation=above.rotation*n.transform().rotation;
        here.scale=above.scale*n.transform().scale;
        here.position=above.position+above.rotation*(above.scale*n.transform().position);
        if(n.mesh()) {
            auto mesh=std::make_unique<saida::MeshNode>(n.name(),n.mesh(),n.material());
            mesh->transform()=here;
            if(auto* shown=dynamic_cast<saida::MeshNode*>(&n))mesh->castShadows()=shown->castShadows();
            body->addChild(std::move(mesh));
        }
        for(auto& c:n.children())walk(*c,here);
    };
    for(auto& c:model->children())walk(*c,saida::Transform{});
    model->setEnabled(false);
    const size_t before=animators.size();
    playerBody=player->addChild(std::move(body));
    playerBodyRest=playerBody->transform();
    animators={animator};
    saida::Log::info("[World player] one body: one Animator over ",playerBody->children().size(),
                     " meshes (the scan imported ",before,")");
}

void World::Impl::setupCombat() {
    if(!playerBody){playerBody=animators.front()->node();playerBodyRest=playerBody->transform();}
    auto* animator=animators.front();
    const saida::Rig& rig=*animator->rig();
    playerBikeLean=animator->addModifier<saida::GazeModifier>(rig,r1::rocketboxRidingLean());
    playerBikeAim=animator->addModifier<r1::RocketboxRidingAim>(rig);
    playerBikeLook=animator->addModifier<saida::GazeModifier>(rig,r1::rocketboxRidingLook());
    playerArms[0]=animator->addModifier<saida::TwoBoneIKModifier>(rig,r1::rocketboxArm(false));
    playerArms[1]=animator->addModifier<saida::TwoBoneIKModifier>(rig,r1::rocketboxArm(true));
    if(!playerArms[0]->valid()||!playerArms[1]->valid())
        throw std::runtime_error("Player skeleton lacks the arm chains a punch reaches with");
    for(int h=0;h<2;++h)playerBikeLegs[h]=animator->addModifier<saida::TwoBoneIKModifier>(rig,
        saida::TwoBoneIKModifier::Settings{h?"Bip01 R Thigh":"Bip01 L Thigh",h?"Bip01 R Calf":"Bip01 L Calf",h?"Bip01 R Foot":"Bip01 L Foot",22.f});
    playerBikeHands=animator->addModifier<r1::RocketboxRidingHands>(rig);
    playerReach=r1::armLength(rig,true);
    playerFall.bind(*playerBody->addBehaviour<saida::RagdollBehaviour>(ragdollProfile),
                    *feet,r1::combat::kPlayerLieSeconds);
    if(!r1::pistolGrip(rig,pistolGrip))throw std::runtime_error("Player skeleton lacks the hand and fingers a pistol is held in");
    if(!r1::pistolGrip(rig,launcherSupportGrip,false))throw std::runtime_error("Player skeleton lacks the launcher support hand");
    if(auto built=r1::buildPistol(engine.resources())) {
        built->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 R Hand",pistolGrip);
        pistol=playerBody->addChild(std::move(built));
        pistol->setVisible(false);
    }
    effects.build(engine.sceneTree().world());
    sounds.load();
    for(const char* path:{kWindSound,kFlutterSound,kOpeningSound})
        if(!saida::AudioManager::get().preload(path))
            saida::Log::error("[World arrival] missing sound ",path," (python -m r1.arrival_sounds): the fall will be silent");
    buildRockets();
    showHeldPistol();
    saida::Log::info("[World combat] ready: ",ragdollProfile.parts.size()," bodies per ragdoll, ",kLiveRagdolls,
                     " tumbling at once; arm ",playerReach," m");
}

double World::Impl::groundUnder(const Loaded& tile,double x,double z,double fallback) {
    const glm::dvec3 local(x,fallback,z);
    const auto point=tile.frame.origin+tile.frame.basis*local;
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    if(!this->tile(geo.x,geo.y))return fallback;
    const double h=height(geo.x,geo.y,geo.z+.5);
    if(!std::isfinite(h))return fallback;
    return tile.frame.local(ecef(geo.x,geo.y,h)).y;
}

glm::dvec3 World::Impl::sceneToGeo(const glm::vec3& p) const {
    const auto point=origin.origin+origin.basis*glm::dvec3(p);
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    return {geo.x,geo.y,geo.z};
}

float World::Impl::sceneGround(const glm::vec3& p) {
    const auto geo=sceneToGeo(p);
    if(!tile(geo.x,geo.y))return p.y;
    const double h=height(geo.x,geo.y,geo.z+.5);
    return std::isfinite(h)?float(origin.local(ecef(geo.x,geo.y,h)).y):p.y;
}

Loaded* World::Impl::tileByKey(const std::string& key) {
    auto found=loaded.find(key);
    return found==loaded.end()?nullptr:&found->second;
}

glm::vec3 World::Impl::nodePosition(const saida::Node& n) { return glm::vec3(n.worldTransform()[3]); }

glm::dvec3 World::Impl::toTile(const Loaded& tile,const glm::vec3& v) const {
    return glm::transpose(tile.frame.basis)*origin.basis*glm::dvec3(v);
}

bool World::Impl::beginFall(r1::Knockdown& fall,const glm::vec3& velocity,const glm::vec3& spin,const char* who) {
    if(fall.activate(velocity,spin))return true;
    if(ragdollRefused++==0)
        saida::Log::error("[World combat] ",who," ragdoll refused (reserved combat bodies ",kCombatBodies,
                          " of ",kPhysicsBodies,")");
    text("stream-status","The fall could not be simulated.");
    return false;
}

bool World::Impl::knockDown(Loaded& tile,size_t slot,const glm::vec3& velocity,const glm::vec3& spin) {
    if(slot>=tile.people.size())return false;
    Person& p=tile.people[slot];
    if(!p.node||!p.fall.ragdoll())return false;
    if(p.fall.ragdoll()->simulating())return true;
    limitRagdolls();
    if(!beginFall(p.fall,velocity,spin,"passer-by"))return false;
    tile.crowd.knockDown(slot);
    p.punch=-1;
    for(auto* arm:p.arms)if(arm)arm->release();
    if(p.gaze)p.gaze->release();
    return true;
}

void World::Impl::limitRagdolls() {
    size_t live=0;saida::RagdollBehaviour* oldest=nullptr;
    for(auto tile:ring) {
        auto* t=tileByKey(tile.key());if(!t)continue;
        for(auto& p:t->people) {
            if(!p.fall.ragdoll()||!p.fall.ragdoll()->simulating())continue;
            ++live;
            if(!oldest||p.fall.ragdoll()->simulatedSeconds()>oldest->simulatedSeconds())oldest=p.fall.ragdoll();
        }
    }
    for(auto& unit:policeUnits)for(auto& officer:unit.officers)if(auto* ragdoll=officer.person.fall.ragdoll();ragdoll&&ragdoll->simulating()) {
        ++live;if(!oldest||ragdoll->simulatedSeconds()>oldest->simulatedSeconds())oldest=ragdoll;
    }
    if(live>=kLiveRagdolls&&oldest)oldest->freeze();
    SAIDA_PROFILE_COUNTER("World/RagdollsTumbling",double(live));
}

bool World::Impl::personDown(Loaded& tile,size_t slot,Person& p,const r1::Walker& w,double dt) {
    if(w.activity!=r1::Activity::Down) {
        // Back on their feet and blending out: the crowd walks them again.
        return false;
    }
    if(!p.fall.ragdoll())return true;
    const auto state=p.fall.ragdoll()->state();
    if(state==saida::RagdollBehaviour::State::Animated) {
        // Down without a body to show (a ragdoll the world refused, or
        // released by a disable): stood up where they are.
        const glm::vec3 at=p.node->transform().position;
        tile.crowd.getUp(slot,at.x,at.y,at.z,p.yaw);
        return false;
    }
    if(p.fall.update(dt,!w.dead,[&](const glm::vec3& at){return sceneGround(at);})) {
        const glm::vec3 local=p.node->transform().position;
        const glm::vec3 front=p.node->transform().rotation*glm::vec3(0,0,1);
        p.yaw=std::atan2(front.x,-front.z);p.shown=true;
        tile.crowd.getUp(slot,local.x,local.y,local.z,p.yaw);
        p.clip="";
        return false;
    }
    return true;
}

void World::Impl::personAttack(Loaded& tile,size_t slot,Person& p,const r1::Walker& w,double dt) {
    (void)slot;
    if(w.attacks!=p.attacks) {
        if(w.attacks>p.attacks&&p.punch<0){p.punch=0;p.punchHand^=1;p.struck=false;}
        p.attacks=w.attacks;
    }
    if(p.punch<0)return;
    p.punch+=dt;
    auto* arm=p.arms[p.punchHand];
    const double reach=r1::combat::Punch::extensionAt(p.punch);
    if(arm&&player->enabled()) {
        const glm::mat4 toObject=glm::inverse(p.node->worldTransform());
        const glm::vec3 head=player->transform().position+glm::vec3(0.f,float(playerHeight*.9),0.f);
        const glm::vec3 target=glm::vec3(toObject*glm::vec4(head,1.f));
        arm->reach(target,float(reach));arm->setWeightNow(float(reach));
        arm->setPole({p.punchHand?-.5f:.5f,.9f,-.3f});
    }
    if(!p.struck&&p.punch>=r1::combat::Punch::kStrike) {
        p.struck=true;
        const glm::vec3 from=nodePosition(*p.node),to=player->transform().position;
        const bool onFoot=!driving()&&!sailing()&&!piloting()&&!swimming()&&!playerDown;
        if(onFoot&&glm::length(glm::vec2(to.x-from.x,to.z-from.z))<r1::Crowd::kFightReach+.45) {
            sounds.play(r1::CombatSounds::Punch,glm::length(camera->transform().position-to));
            hurtPlayer(r1::combat::kFists.damage*.65,glm::normalize(glm::vec3(to.x-from.x,0.f,to.z-from.z)+glm::vec3(0,.001f,0)),"a passer-by");
        } else sounds.play(r1::CombatSounds::Swing,glm::length(camera->transform().position-from),.6f);
    }
    if(p.punch>=r1::combat::Punch::kRecover){p.punch=-1;if(arm)arm->release();}
    (void)tile;
}

void World::Impl::hurtPlayer(double damage,const glm::vec3& push,const char* by,bool knockDown) {
    if(wasted||(!player->enabled()&&!ridingBike())||health.invincible()||health.dead()||!(damage>0))return;
    const bool killed=health.take(damage);
    for(size_t i=0;i<animators.size()&&i<playerImpacts.size();++i) {
        glm::mat3 body(animators[i]->node()->worldTransform());
        for(int c=0;c<3;++c)body[c]=glm::normalize(body[c]);
        playerImpacts[i]->push(glm::transpose(body)*push,float(std::min(9.,2.+damage*.25)));
    }
    hurtShown=fightClock;
    saida::Log::info("[World combat] player hurt by ",by,": ",damage," (health ",health.hp,")");
    if(killed||knockDown)knockPlayerDown(push*3.f+glm::vec3(0,1.f,0),
                                       glm::cross(glm::vec3(0,1,0),push)*2.f,killed);
}

void World::Impl::knockPlayerDown(const glm::vec3& velocity,const glm::vec3& spin,bool killed) {
    glm::vec3 ridingVelocity(0);
    if(ridingBike()) {
        if(auto* body=r1::VehicleRuntime::body(*car);body&&body->physicsWorld())
            ridingVelocity=body->physicsWorld()->linearVelocity(body->bodyId());
        // Restore the avatar's normal parent and release the handlebar/leg
        // pose before activating its ragdoll. Keep the bike's momentum.
        leaveCar();
    }
    if(playerDown) {
        if(killed){wasted=true;downClock=0;showWasted(true);}
        if(playerFall.activate())playerFall.ragdoll()->applyImpulse(velocity*ragdollProfile.mass,downAnchor());
        return;
    }
    if(aiming)setAiming(false);
    for(auto* arm:playerArms)if(arm)arm->release();
    if(!beginFall(playerFall,velocity+ridingVelocity,spin,"player")) {
        if(killed){wasted=true;playerDown=true;downClock=0;showWasted(true);}
        return;
    }
    playerDown=true;wasted=killed;downClock=0;
    sounds.play(r1::CombatSounds::Fall,followDistance,.8f);
    if(killed)showWasted(true);
}

void World::Impl::landed(double speed) {
    if(fightClock-arrivalClock<4.)return;  // a spawn settling on the relief is not a fall
    const double damage=r1::combat::fallDamage(speed);
    if(damage<=0||health.invincible())return;
    sounds.play(r1::CombatSounds::Fall,followDistance,std::min(1.,speed/12.));
    hurtPlayer(damage,glm::vec3(0,-1,0),"the fall",damage>=35);
}

bool World::Impl::hitByVehicle(const saida::CharacterBodyNode::Contact& contact) {
    if(contact.node->name()!=r1::VehicleRuntime::kBodyName||!contact.node->physicsWorld())return false;
    // Only the car's own motion toward him counts: running into a parked
    // car is his speed, not a blow.
    const glm::vec3 velocity=contact.node->physicsWorld()->linearVelocity(contact.node->bodyId());
    const float closing=glm::dot(velocity,glm::normalize(contact.normal));
    const auto hit=r1::combat::carImpact(closing);
    if(!hit.knockDown||playerDown)return false;
    const float speed=glm::length(velocity);
    const glm::vec3 along=speed>.1f?velocity/speed:glm::normalize(contact.normal);
    sounds.play(r1::CombatSounds::CarHit,followDistance);
    const bool killed=health.take(hit.damage);
    hurtShown=fightClock;
    saida::Log::info("[World combat] player hit by a car at ",closing," m/s: ",hit.damage," (health ",health.hp,")");
    knockPlayerDown(velocity*.8f+glm::vec3(0,1.f+.12f*speed,0),glm::cross(along,glm::vec3(0,1,0))*std::clamp(speed*.5f,2.f,8.f),killed);
    return true;
}

glm::vec3 World::Impl::downAnchor() const {
    if(playerFall.ragdoll()&&playerFall.ragdoll()->ragdoll()&&playerFall.ragdoll()->simulating())return playerFall.ragdoll()->ragdoll()->rootPosition();
    return nodePosition(*playerBody);
}

void World::Impl::updatePlayerDown(double dt) {
    downClock+=dt;
    const bool recovering=playerFall.update(dt,!wasted,
        [&](const glm::vec3& at){return sceneGround(at);},&playerBodyRest);
    if(wasted) {
        if(downClock>=r1::combat::kWastedSeconds)respawn();
        return;
    }
    if(recovering) {
        const glm::vec3 at=player->transform().position;
        const auto geo=sceneToGeo(at);
        lon=geo.x;lat=geo.y;alt=height(lon,lat,geo.z+.5);if(!std::isfinite(alt))alt=geo.z;
        jumpOffset=jumpVelocity=0;
        player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
        const glm::vec3 front=player->transform().rotation*glm::vec3(0,0,1);
        yaw=wrap(std::atan2(front.x,-front.z)/rad);
        return;
    }
    if(playerFall.ragdoll()->state()==saida::RagdollBehaviour::State::Animated){playerDown=false;downClock=0;}
}

void World::Impl::respawn() {
    const glm::dvec3 here=ecef(lon,lat,alt);
    double best=1e30;glm::dvec2 at(lon,lat);std::string name;
    for(auto& [key,t]:loaded)for(const auto& room:t.interiors) {
        if(room.plan.recipe!="clinic")continue;
        const r1::P2 out{room.plan.door.x-room.plan.inward.x*2.5,room.plan.door.y-room.plan.inward.y*2.5};
        const glm::dvec3 local(out.x,room.plan.floor,out.y);
        const auto point=t.frame.origin+t.frame.basis*local;
        const double d=glm::length(point-here);
        if(d<best) {
            best=d;const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
            at={geo.x,geo.y};name=room.plan.name.empty()?"the hospital":room.plan.name;
        }
    }
    clearPolice();
    playerFall.release();
    playerBody->transform()=playerBodyRest;
    playerDown=wasted=false;downClock=0;
    health.restore();
    lon=at.x;lat=at.y;alt=height(lon,lat);jumpOffset=jumpVelocity=0;
    player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
    arrivalClock=fightClock;
    request(lon,lat);
    respawnSaid=best<1e29?"Waking up outside "+name+".":"No mapped hospital nearby: waking up where you fell.";
    saida::Log::info("[World combat] respawn ",best<1e29?"at "+name:std::string("where he fell")," (",lon,", ",lat,")");
    showWasted(false);
    text("stream-status",respawnSaid);
}

void World::Impl::showWasted(bool show) {
    style("wasted","display",show?"block":"none");
    if(show)text("wasted-detail","Waking up at the nearest hospital…");
}

void World::Impl::showHeldPistol() {
    if(pistol)pistol->setVisible(arsenal.held()==r1::combat::Weapon::Pistol&&(!playerBikeRider||aiming));
    if(launcher)launcher->setVisible(arsenal.held()==r1::combat::Weapon::RocketLauncher&&!playerBikeRider);
}

void World::Impl::setAiming(bool on) {
    aiming=on;
    if(!on&&playerBikeAim)playerBikeAim->release();
    showHeldPistol();
    style("crosshair","display",on?"block":"none");
    if(!on)for(auto* arm:playerArms)if(arm){arm->release();arm->clearTipRotation();}
}

void World::Impl::selectWeapon(r1::combat::Weapon wanted) {
    if(arsenal.held()==wanted)return;
    arsenal.select(wanted);
    showHeldPistol();
    if(wanted==r1::combat::Weapon::Fists&&aiming)setAiming(false);
}

saida::QueryFilter World::Impl::combatFilter() const {
    saida::QueryFilter filter;filter.ignoreInner=feet->innerBodyId();
    if(ridingBike())if(auto* body=r1::VehicleRuntime::body(*car))filter.ignore=body->bodyId();
    return filter;
}

bool World::Impl::personByBody(const saida::RaycastHit& hit,Loaded*& outTile,size_t& outSlot) {
    auto* physics=engine.sceneTree().world().physics();if(!physics)return false;
    auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
    for(auto tile:ring) {
        auto* t=tileByKey(tile.key());if(!t)continue;
        for(size_t i=0;i<t->people.size();++i) {
            const Person& p=t->people[i];
            if(!p.node||!p.node->enabled())continue;
            if((node&&p.body==node)||(p.fall.ragdoll()&&p.fall.ragdoll()->ragdoll()&&p.fall.ragdoll()->ragdoll()->partOf(hit.body)>=0)) {
                outTile=t;outSlot=i;return true;
            }
        }
    }
    return false;
}

void World::Impl::alarm(const glm::vec3& at,double radius,double seconds) {
    for(auto tile:ring) {
        auto* t=tileByKey(tile.key());if(!t)continue;
        const glm::dvec3 local=t->frame.local(origin.origin+origin.basis*glm::dvec3(at));
        t->crowd.scare(local.x,local.z,radius,seconds);
    }
}

void World::Impl::strikePerson(Loaded& tile,size_t slot,double damage,const glm::vec3& direction,const glm::vec3& point,
                      float impulse,bool always) {
    Person& p=tile.people[slot];
    const auto& walkers=tile.crowd.walkers();
    if(slot>=walkers.size()||!walkers[slot].alive)return;
    const bool wasDown=walkers[slot].activity==r1::Activity::Down;
    const bool wasDead=walkers[slot].dead;
    tile.crowd.damage(slot,damage);
    const r1::Walker& w=walkers[slot];
    if(!wasDead)reportCrime(w.dead?r1::police::Crime::Killing:r1::police::Crime::Assault,point);
    effects.burst(r1::CombatEffects::Blood,point);
    if(wasDown) {
        // Already on the ground: a round moves the body again.
        limitRagdolls();
        if(!beginFall(p.fall,{}, {},"passer-by"))return;
        if(p.fall.ragdoll())p.fall.ragdoll()->applyImpulse(direction*impulse,point);
        return;
    }
    if(always||w.dead||w.health<100.*r1::combat::kKnockDownBelow) {
        const glm::vec3 spin=glm::cross(glm::vec3(0,1,0),direction)*2.5f;
        if(knockDown(tile,slot,direction*2.2f+glm::vec3(0,.6f,0),spin)) {
            p.fall.ragdoll()->applyImpulse(direction*impulse,point);
            sounds.play(r1::CombatSounds::Fall,glm::length(camera->transform().position-point),.7f);
        }
        return;
    }
    if(p.impact) {
        const glm::quat facing=p.node->transform().rotation;
        const glm::dvec3 d=toTile(tile,direction);
        p.impact->push(glm::inverse(facing)*glm::vec3(float(d.x),0.f,float(d.z)),7.f);
    }
    tile.crowd.provoke(slot);
}

void World::Impl::throwPunch() {
    if(!punch.throwAt(fightClock))return;
    sounds.play(r1::CombatSounds::Swing,followDistance,.7f);
    // The nearest person in front within reach, their head the target.
    const glm::vec3 me=player->transform().position;
    const glm::vec3 ahead(float(std::sin(yaw*rad)),0.f,float(-std::cos(yaw*rad)));
    double best=r1::combat::kFists.range*kPeopleVehicleResize+.25;punchAimed=false;punchPolice=-1;
    for(auto tile:ring) {
        auto* t=tileByKey(tile.key());if(!t)continue;
        const auto& walkers=t->crowd.walkers();
        for(size_t i=0;i<t->people.size()&&i<walkers.size();++i) {
            const Person& p=t->people[i];
            if(!p.node||!p.node->enabled()||!walkers[i].alive||walkers[i].activity==r1::Activity::Down)continue;
            const glm::vec3 d=nodePosition(*p.node)-me;
            const double flat=std::hypot(d.x,d.z);
            if(flat>=best||flat<1e-3)continue;
            if((d.x*ahead.x+d.z*ahead.z)/flat<.45)continue;
            best=flat;punchTileKey=tile.key();punchSlot=i;punchAimed=true;
        }
    }
    for(size_t u=0;u<policeUnits.size();++u)if(policeUnits[u].dismounted)for(size_t i=0;i<2;++i) {
        auto& officer=policeUnits[u].officers[i];
        if(!officer.person.node||officer.health<=0||officer.person.fall.ragdoll()->state()!=saida::RagdollBehaviour::State::Animated)continue;
        auto d=nodePosition(*officer.person.node)-me;d.y=0;const float length=glm::length(d);
        if(length>.01f&&length<best&&glm::dot(d/length,ahead)>.45f){best=length;punchPolice=int(u*2+i);punchAimed=false;}
    }
    // Squared up to whom he hits.
    if(punchAimed)if(auto* t=tileByKey(punchTileKey)) {
        const glm::vec3 d=nodePosition(*t->people[punchSlot].node)-me;
        yaw=wrap(std::atan2(d.x,-d.z)/rad);
    }
}

void World::Impl::landPunch() {
    if(punchPolice>=0) {
        auto& officer=policeUnits[size_t(punchPolice)/2].officers[size_t(punchPolice)%2];
        if(!officer.person.node||!officer.person.node->enabled())return;
        const auto at=nodePosition(*officer.person.node);const auto delta=at-policePlayerPosition();
        if(glm::length(delta)>r1::combat::kFists.range*kPeopleVehicleResize+.4)return;
        saida::RaycastHit hit;hit.hit=true;hit.body=officer.person.body->innerBodyId();hit.point=at+glm::vec3(0,float(playerHeight*.75),0);
        hitPolice(hit,punch.damage(),glm::normalize(delta),false,float(r1::combat::kFists.impulse));
        sounds.play(r1::CombatSounds::Punch,followDistance);return;
    }
    Loaded* t=punchAimed?tileByKey(punchTileKey):nullptr;
    if(!t||punchSlot>=t->people.size()||!t->people[punchSlot].node)return;
    const Person& p=t->people[punchSlot];
    const glm::vec3 me=player->transform().position,them=nodePosition(*p.node);
    glm::vec3 d=them-me;d.y=0;
    if(glm::length(d)>r1::combat::kFists.range*kPeopleVehicleResize+.4)return;  // they stepped out of reach
    d=glm::normalize(d);
    const glm::vec3 point=them+glm::vec3(0.f,float(playerHeight*(punch.hook()?.82:.88)),0.f);
    sounds.play(r1::CombatSounds::Punch,followDistance);
    alarm(them,r1::combat::kFightSeen,6.);
    strikePerson(*t,punchSlot,punch.damage(),d,point,float(r1::combat::kFists.impulse*(punch.hook()?1.6:1.)),punch.hook());
}

bool World::Impl::evacuateVehicle(saida::CollisionObjectNode& hitBody) {
    auto* root=hitBody.parent();
    if(!root||(root==car&&driving()))return false;
    for(const auto& unit:policeUnits)if(unit.car==root)return false;
    const auto& spec=vehicleSpec(*root);
    if(r1::motorcycle(spec.name))return false;
    auto* occupant=root->getBehaviour<VehicleOccupant>();
    if(occupant&&occupant->escaped)return false;
    size_t escaped=0;
    for(const auto& [key,t]:loaded)for(const auto& w:t.crowd.walkers())escaped+=w.alive&&w.escapedVehicle;
    if(escaped>=r1::Crowd::kVehicleEscapeSlots) {
        saida::Log::warn("[World combat] vehicle exit deferred: all eight fleeing occupant slots are occupied");
        return false;
    }
    auto* physics=engine.sceneTree().world().physics();if(!physics)return false;
    r1::VehicleRuntime::synchronize(*root);
    root->updateTransforms(root->parent()->worldTransform(),true);
    const auto center=glm::vec3(root->worldTransform()[3]);
    const auto rotation=glm::normalize(glm::quat_cast(glm::mat3(root->worldTransform())));
    const auto threat=policePlayerPosition();
    const r1::VehicleDimensions dimensions{float(spec.length),float(spec.width),float(spec.height),float(spec.wheelbase),float(spec.wheelRadius)};
    auto side=rotation*glm::vec3(1,0,0);side.y=0;
    if(glm::length(side)<.01f)side={1,0,0};else side=glm::normalize(side);
    if(glm::dot(side,threat-center)>0)side=-side;
    for(float sign:{1.f,-1.f}) {
        auto at=center+r1::VehicleRuntime::exitOffset(dimensions,rotation,side*sign);
        at.y=sceneGround(at)+.05f;
        bool clear=true;
        for(float height:{.25f,float(playerHeight*.5),float(playerHeight-.2)})
            if(!physics->overlapSphere(at+glm::vec3(0,height,0),float(r1::Crowd::kBodyRadius)).empty())clear=false;
        if(!clear)continue;
        const auto point=origin.origin+origin.basis*glm::dvec3(at);
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        auto* t=tile(geo.x,geo.y);if(!t||t->humanKinds.empty())continue;
        const auto local=t->frame.local(point);
        const auto fear=t->frame.local(origin.origin+origin.basis*glm::dvec3(threat));
        const int slot=t->crowd.escapeVehicle({local.x,local.y,local.z},{fear.x,fear.z});
        if(slot<0)continue;
        if(t->people.size()<t->crowd.walkers().size())t->people.resize(t->crowd.walkers().size());
        auto& person=t->people[size_t(slot)];
        person.fall.release();person.shown=false;person.clip="";person.punch=-1;
        syncPeople(*t,cameraTileLocal(*t),0,nullptr);
        if(person.node)person.node->updateTransforms(t->node->worldTransform(),true);
        if(!occupant)occupant=root->addBehaviour<VehicleOccupant>();
        occupant->escaped=true;
        if(auto* drive=r1::VehicleRuntime::driver(*root))drive->parked=true;
        for(auto& [key,source]:loaded)for(size_t i=0;i<source.cars.size();++i)
            if(source.cars[i]==root&&trafficSlotLive(source,i))preserveTrafficCar(source,i);
        saida::Log::info("[World combat] occupant fled from ",spec.name);
        return true;
    }
    saida::Log::warn("[World combat] vehicle exit deferred: neither door has clear ground");
    return false;
}

void World::Impl::runVehicleEscapeSmoke(double dt) {
    escapeSmokeTime+=dt;
    auto fail=[&](const char* why){saida::Log::error("[World E2E] FAIL vehicle escape: ",why);testFailed=true;engine.sceneTree().quit();};
    if(escapeSmokePhase==0) {
        if(!spawnSportsCar().empty()){fail("no space for the test car");return;}
        escapeSmokePhase=1;escapeSmokeTime=0;return;
    }
    if(escapeSmokePhase==1) {
        if(escapeSmokeTime<1)return;
        auto* body=r1::VehicleRuntime::body(*parked.back().node);
        if(!body||!evacuateVehicle(*body)){fail("hit did not release an occupant");return;}
        if(evacuateVehicle(*body)){fail("a second hit released another occupant");return;}
        for(const auto& [key,t]:loaded)for(size_t i=0;i<t.crowd.walkers().size();++i) {
            const auto& w=t.crowd.walkers()[i];if(!w.alive||!w.escapedVehicle)continue;
            const auto& p=t.people[i];
            if(!p.node||!dynamic_cast<saida::CharacterBodyNode*>(p.body)||std::strcmp(p.clip,"run")) {
                fail("occupant has no running character body");return;
            }
            escapeSmokeTile=key;escapeSmokeSlot=i;
            escapeSmokeDistance=glm::distance(nodePosition(*p.node),policePlayerPosition());
        }
        if(escapeSmokeTile.empty()){fail("no live occupant");return;}
        escapeSmokePhase=2;escapeSmokeTime=0;return;
    }
    if(escapeSmokeTime<2)return;
    auto* t=tileByKey(escapeSmokeTile);
    if(!t||escapeSmokeSlot>=t->people.size()){fail("occupant disappeared");return;}
    const auto& p=t->people[escapeSmokeSlot];
    const double distance=glm::distance(nodePosition(*p.node),policePlayerPosition());
    if(distance<escapeSmokeDistance+1){fail("occupant did not run away from the player");return;}
    saida::Log::info("[World E2E] PASS vehicle escape: single occupant, running character, distance ",escapeSmokeDistance," -> ",distance);
    engine.sceneTree().quit();
}

void World::Impl::fire() {
    const auto result=arsenal.pull();
    if(result==r1::combat::Arsenal::Trigger::Empty){sounds.play(r1::CombatSounds::DryFire);if(arsenal.reloading())sounds.play(r1::CombatSounds::Reload,0,.8f);return;}
    if(result!=r1::combat::Arsenal::Trigger::Fired)return;
    if(arsenal.reloading())sounds.play(r1::CombatSounds::Reload,0,.8f);
    auto* physics=engine.sceneTree().world().physics();
    const glm::vec3 eye=camera->transform().position;
    // Aim error grows with movement and with the last shot's recoil.
    static uint32_t spreadSeed=0x9e3779b9u;
    auto random=[&]{spreadSeed^=spreadSeed<<13;spreadSeed^=spreadSeed>>17;spreadSeed^=spreadSeed<<5;return double(spreadSeed)/4294967296.;};
    const double spread=r1::combat::kPistol.spread*(1+2.5*std::min(1.,(ridingBike()?std::abs(carSpeed)/18.:std::hypot(footEast,footNorth)/3.))+recoil*4);
    const double a=random()*6.283185307179586,rr=std::sqrt(random())*spread;
    const glm::quat jitter=glm::angleAxis(float(rr*std::cos(a)),glm::vec3(0,1,0))*glm::angleAxis(float(rr*std::sin(a)),glm::vec3(1,0,0));
    const glm::vec3 look=glm::normalize(camera->transform().rotation*(jitter*glm::vec3(0,0,-1)));
    const float range=float(r1::combat::kPistol.range);
    glm::vec3 aimPoint=eye+look*range;
    if(physics){const auto hit=physics->raycast(eye+look*float(followDistance*.8),look,range,combatFilter());if(hit.hit)aimPoint=hit.point;}
    // The round leaves the muzzle for whatever the camera put under the
    // crosshair; a wall between the two takes it instead.
    const glm::vec3 muzzle=pistol?glm::vec3(pistol->worldTransform()*glm::vec4(r1::kPistolMuzzle,1.f)):eye;
    const glm::vec3 path=aimPoint-muzzle;
    const float length=glm::length(path);
    effects.muzzle(muzzle);
    sounds.play(r1::CombatSounds::Shot);
    recoil=std::min(1.,recoil+.55);
    pitch=std::clamp(pitch+1.1,-65.,25.);
    alarm(ridingBike()?car->transform().position:player->transform().position,r1::combat::kGunshotHeard,10.);
    reportCrime(r1::police::Crime::Gunshot,policePlayerPosition());
    if(!physics||length<1e-3)return;
    const glm::vec3 dir=path/length;
    const auto hit=physics->raycast(muzzle,dir,length+.05f,combatFilter());
    if(!hit.hit)return;
    const double heard=glm::length(eye-hit.point);
    if(hitAnimal(hit,dir,float(r1::combat::kPistol.impulse))) {
        sounds.play(r1::CombatSounds::Impact,heard,.5f);return;
    }
    if(hitPolice(hit,r1::combat::kPistol.damage,dir))return;
    Loaded* t=nullptr;size_t slot=0;
    if(personByBody(hit,t,slot)) {
        const float feet=nodePosition(*t->people[slot].node).y;
        const bool head=hit.point.y-feet>float(playerHeight*.84);
        strikePerson(*t,slot,r1::combat::kPistol.damage*(head?r1::combat::kPistol.headMultiplier:1.),dir,hit.point,
                     float(r1::combat::kPistol.impulse),true);
        sounds.play(r1::CombatSounds::Impact,heard,.5f);
        return;
    }
    auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
    if(node&&node->name()==r1::VehicleRuntime::kBodyName) {
        evacuateVehicle(*node);
        effects.burst(r1::CombatEffects::Sparks,hit.point);
        if(auto* dent=node->getBehaviour<saida::DeformableMeshBehaviour>()) {
            const glm::mat4 toBody=glm::inverse(node->worldTransform());
            dent->applyImpact(glm::vec3(toBody*glm::vec4(hit.point,1.f)),glm::normalize(glm::vec3(toBody*glm::vec4(dir,0.f))),
                              dent->yieldEnergy*4.f);
        }
        physics->applyImpulse(hit.body,dir*float(r1::combat::kPistol.impulse),hit.point);
        sounds.play(r1::CombatSounds::Impact,heard);
        return;
    }
    effects.burst(r1::CombatEffects::Dust,hit.point+hit.normal*.03f);
    // A loose body is pushed; a static one absorbs it (PhysicsWorld).
    physics->applyImpulse(hit.body,dir*float(r1::combat::kPistol.impulse),hit.point);
    sounds.play(r1::CombatSounds::Impact,heard);
}

void World::Impl::poseArms(const glm::vec3* inspectionLook) {
    auto* animator=animators.front();
    if(!animator||!playerBody)return;
    const glm::mat4 toObject=glm::inverse(playerBody->worldTransform());
    const auto& pose=animator->globalPose().globalMatrices;
    const saida::Rig& rig=*animator->rig();
    const int shoulderBone[2]={rig.findBoneIndex("Bip01 L UpperArm"),rig.findBoneIndex("Bip01 R UpperArm")};
    if(shoulderBone[0]<0||shoulderBone[1]<0||pose.size()<rig.boneCount())return;
    const glm::vec3 shoulder[2]={glm::vec3(pose[size_t(shoulderBone[0])][3]),glm::vec3(pose[size_t(shoulderBone[1])][3])};
    if(playerBikeHands)playerBikeHands->hold(ridingBike()||arsenal.held()==r1::combat::Weapon::RocketLauncher);
    if((aiming||arsenal.held()==r1::combat::Weapon::RocketLauncher)&&arsenal.held()!=r1::combat::Weapon::Fists) {
        const glm::vec3 look=inspectionLook?*inspectionLook:camera->transform().rotation*glm::vec3(0,0,-1);
        glm::vec3 aim=glm::normalize(glm::vec3(toObject*glm::vec4(look,0.f)));
        if(ridingBike()){playerBikeAim->lookAt(aim);playerBikeLook->lookAt(shoulder[1]+aim*100.f);}
        const glm::vec3 up(0,1,0);
        // Out in front at eye height, the arm nearly straight.
        const float overShoulder=ridingBike()?.18f*std::clamp(-aim.z*2.f,0.f,1.f):0.f;
        const glm::vec3 grip=shoulder[1]+aim*(playerReach*.9f)+glm::vec3(-.05f,overShoulder,0.f);
        glm::vec3 side=glm::cross(up,aim);if(glm::dot(side,side)<1e-6f)side=glm::vec3(1,0,0);
        side=glm::normalize(side);
        const glm::vec3 top=glm::cross(aim,side);
        glm::mat3 gun;gun[0]=side;gun[1]=top;gun[2]=aim;
        const glm::quat hand=glm::normalize(glm::quat_cast(gun)*glm::inverse(pistolGrip.rotation));
        if(arsenal.held()==r1::combat::Weapon::RocketLauncher) {
            // Shoulder-fired stance: solve palm contacts, not wrist points.
            // Keeping both hands within reach prevents IK clamping from
            // pulling the foregrip out of the support hand when aiming down.
            glm::vec3 origin=shoulder[1]+aim*(playerReach*.38f)-top*.12f-side*.025f;
            const auto supportRotation=glm::normalize(glm::quat_cast(gun)*glm::inverse(launcherSupportGrip.rotation));
            if(!ridingBike()) {
                const auto left=origin+gun*r1::kLauncherSupport-supportRotation*launcherSupportGrip.position;
                const auto delta=left-shoulder[0];
                const float reach=playerReach*.94f,distance=glm::length(delta);
                if(distance>reach)origin-=delta*((distance-reach)/distance);
            }
            playerArms[1]->reach(origin-hand*pistolGrip.position);
            playerArms[1]->setTipRotation(hand);
            playerArms[1]->setPole(shoulder[1]-side*.35f-top*.55f);
            if(ridingBike())return;
            playerArms[0]->reach(origin+gun*r1::kLauncherSupport-supportRotation*launcherSupportGrip.position);
            playerArms[0]->setTipRotation(supportRotation);
            playerArms[0]->setPole(shoulder[0]+side*.25f-top*.6f);
            return;
        }
        playerArms[1]->reach(grip);playerArms[1]->setTipRotation(hand);
        playerArms[1]->setPole(shoulder[1]-side*.4f-top*.5f+aim*.12f);
        // Riders keep their left hand on the handlebar.
        if(ridingBike())return;
        playerArms[0]->reach(grip+side*.035f-top*.045f-aim*.02f);playerArms[0]->clearTipRotation();
        playerArms[0]->setPole(shoulder[0]+glm::vec3(.4f,-.6f,-.1f));
        return;
    }
    for(int h=0;h<2;++h) {
        auto* arm=playerArms[h];
        const bool throwing=punch.active()&&(punch.rightHand()==(h==1));
        if(!throwing){if(arm->weight()>0)arm->release();continue;}
        glm::vec3 target=shoulder[h]+glm::vec3(h?-.08f:.08f,.05f,playerReach*.95f);
        if(Loaded* t=punchAimed?tileByKey(punchTileKey):nullptr)if(punchSlot<t->people.size()&&t->people[punchSlot].node) {
            const glm::vec3 head=nodePosition(*t->people[punchSlot].node)+glm::vec3(0.f,float(playerHeight*(punch.hook()?.8:.88)),0.f);
            target=glm::vec3(toObject*glm::vec4(head,1.f));
        }
        if(punchPolice>=0)if(auto* node=policeUnits[size_t(punchPolice)/2].officers[size_t(punchPolice)%2].person.node)
            target=glm::vec3(toObject*glm::vec4(nodePosition(*node)+glm::vec3(0,float(playerHeight*.85),0),1));
        const float w=float(punch.extension());
        arm->clearTipRotation();arm->reach(target,w);arm->setWeightNow(w);
        arm->setPole(shoulder[h]+glm::vec3(h?-.45f:.45f,-.35f,-.25f));
    }
}

void World::Impl::runOverPeople(double dt) {
    if(!driving()||!car||std::abs(carSpeed)<2.5)return;
    auto* body=r1::VehicleRuntime::body(*car);
    if(!body||!body->physicsWorld())return;
    const glm::vec3 velocity=body->physicsWorld()->linearVelocity(body->bodyId());
    const float speed=glm::length(velocity);
    if(speed<2.5f)return;
    const auto& spec=vehicleSpec(*car);
    const glm::mat4 world=car->worldTransform();
    const glm::mat4 toCar=glm::inverse(world);
    const float halfWidth=float(spec.width*.5+.25),halfLength=float(spec.length*.5),reach=float(speed*dt*1.6+.35);
    const glm::vec3 dir=velocity/speed;
    for(auto tile:ring) {
        auto* t=tileByKey(tile.key());if(!t)continue;
        const auto& walkers=t->crowd.walkers();
        for(size_t i=0;i<t->people.size()&&i<walkers.size();++i) {
            Person& p=t->people[i];
            if(!p.node||!p.node->enabled()||!walkers[i].alive||walkers[i].activity==r1::Activity::Down)continue;
            const glm::vec3 at=nodePosition(*p.node);
            const glm::vec3 local=glm::vec3(toCar*glm::vec4(at,1.f));
            if(std::abs(local.x)>halfWidth||local.y<-1.f||local.y>float(spec.height+.5))continue;
            if(local.z<-halfLength-reach||local.z>halfLength+reach)continue;
            if(glm::dot(at-glm::vec3(world[3]),dir)<0.f)continue;  // behind the car as it goes
            const auto hit=r1::combat::carImpact(speed);
            if(!hit.knockDown)continue;
            t->crowd.damage(i,hit.damage);
            reportCrime(walkers[i].dead?r1::police::Crime::Killing:r1::police::Crime::Assault,at);
            // Legs taken forward, the body over the bonnet.
            const glm::vec3 spin=glm::cross(dir,glm::vec3(0,1,0))*std::clamp(speed*.55f,2.f,9.f);
            if(knockDown(*t,i,velocity*.85f+glm::vec3(0,1.2f+.1f*speed,0),spin)) {
                sounds.play(r1::CombatSounds::CarHit,followDistance);
                effects.burst(r1::CombatEffects::Blood,at+glm::vec3(0.f,float(playerHeight*.5),0.f));
                alarm(at,r1::combat::kFightSeen*1.5,8.);
                saida::Log::info("[World combat] a passer-by run over at ",speed*3.6," km/h, damage ",hit.damage);
            }
        }
    }
}

void World::Impl::updateCombat(double dt) {
    SAIDA_PROFILE_SCOPE("World/Combat");
    fightClock+=dt;
    if(health.invincible()) {
        health.tick(dt);
        if(!health.invincible()){cheatSay("Invincibility over.");saida::Log::info("[World cheat] invincibility over");}
    }
    recoil=std::max(0.,recoil-dt*2.2);
    arsenal.update(dt);
    effects.update(float(dt));
    updateRockets(dt);
    runOverPeople(dt);
    if(playerDown)updatePlayerDown(dt);
    const bool onFoot=!driving()&&!sailing()&&!piloting()&&!swimming()&&!playerDown&&player->enabled();
    const bool onBike=ridingBike()&&playerBikeRider&&!playerDown;
    const bool inHelicopter=piloting()&&plane.helicopter()&&!playerDown;
    // In the air his arms are the arrival's (World::Impl::poseDrop): no fight.
    if((!onFoot&&!onBike&&!inHelicopter)||menu||playerMenu||photoMode||smoke||dropFlying()) {
        if(aiming)setAiming(false);
        wasFire=clicked(GLFW_MOUSE_BUTTON_LEFT);
        if(!onFoot)for(auto* arm:playerArms)if(arm&&arm->weight()>0&&!playerDown)arm->release();
        if(onFoot&&smoke&&!dropFlying())poseArms();
        if(onBike)bikeHandlePose();
        return;
    }
    const bool keys[3]={held(GLFW_KEY_1),held(GLFW_KEY_2),held(GLFW_KEY_3)};
    if(keys[0]&&!wasWeaponKey[0])selectWeapon(r1::combat::Weapon::Fists);
    if(keys[1]&&!wasWeaponKey[1])selectWeapon(r1::combat::Weapon::Pistol);
    if(keys[2]&&!wasWeaponKey[2])selectWeapon(r1::combat::Weapon::RocketLauncher);
    for(int i=0;i<3;++i)wasWeaponKey[i]=keys[i];
    const bool reloadKey=held(GLFW_KEY_E);
    if(reloadKey&&!wasReloadKey&&arsenal.reload())sounds.play(r1::CombatSounds::Reload,0,.8f);
    wasReloadKey=reloadKey;
    const bool wantAim=(inHelicopter&&arsenal.held()==r1::combat::Weapon::RocketLauncher)||
        (arsenal.held()!=r1::combat::Weapon::Fists&&clicked(GLFW_MOUSE_BUTTON_RIGHT));
    if(wantAim!=aiming)setAiming(wantAim);
    const bool fireDown=clicked(GLFW_MOUSE_BUTTON_LEFT);
    if(fireDown&&!wasFire) {
        if(arsenal.held()==r1::combat::Weapon::RocketLauncher){if(onFoot||inHelicopter)fireRocket();}
        else if(inHelicopter){} // helicopter weapons are rockets
        else if(arsenal.held()==r1::combat::Weapon::Fists){if(onFoot&&arsenal.pull()==r1::combat::Arsenal::Trigger::Fired)throwPunch();}
        else if(onFoot||aiming)fire();
    }
    wasFire=fireDown;
    if(inHelicopter)return;
    if(punch.update(dt))landPunch();
    if(onBike){poseArms();bikeHandlePose();return;}
    // Aiming, he faces where the camera looks.
    if(aiming)player->transform().rotation=glm::slerp(player->transform().rotation,
        glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0)),float(1-std::exp(-18*dt)));
    else if(punch.active())player->transform().rotation=glm::slerp(player->transform().rotation,
        glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0)),float(1-std::exp(-20*dt)));
    poseArms();
}

void World::Impl::combatHud() {
    const int hp=int(std::ceil(health.hp));
    style("health-fill","width",std::to_string(int(std::round(health.fraction()*196)))+"px");
    style("health-fill","background-color",health.invincible()?"#f2d16b":hp>30?"#d5f5a4":"#e8664f");
    style("shield","display",health.invincible()?"block":"none");
    if(health.invincible())text("shield","Invincible · "+r1::cheats::clock(health.shield));
    text("health-value",std::to_string(hp));
    const bool pistolHeld=arsenal.held()!=r1::combat::Weapon::Fists;
    text("weapon-name",r1::combat::spec(arsenal.held()).name);
    text("weapon-ammo",pistolHeld?(arsenal.reloading()?"Reloading…":std::to_string(arsenal.loaded())+" / "+std::to_string(arsenal.reserve())):"");
    style("hurt","display",fightClock-hurtShown<.45?"block":"none");
}

} // namespace r1::world
