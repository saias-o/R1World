#include "session.hpp"
#include <windows.h>
#include <psapi.h>

namespace r1::world {
bool World::Impl::typing() const {
    if(cheatOpen)return true;
    auto* document=ui?ui->documentElement():nullptr;
    auto* focus=document&&document->GetContext()?document->GetContext()->GetFocusElement():nullptr;
    return focus&&focus->GetTagName()=="input"&&focus->IsVisible(true);
}

bool World::Impl::held(int key) const {return !typing()&&engine.window().keyDown(key);}

bool World::Impl::clicked(int button) const {return !cheatOpen&&engine.window().mouseButtonDown(button);}

double World::Impl::privateMegabytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))
        ?double(counters.PrivateUsage)/1048576.:0.;
}

void World::Impl::openCheat() {
    if(!smokeCheatCycles)saida::Log::info("[World cheat] line opened, private memory ",int(privateMegabytes())," MB");
    cheatOpen=true;
    field("cheat-input",std::string());
    style("cheat-said","display","none");
    style("cheat","display","block");
    if(auto* input=ui->findElementById("cheat-input")){input->Focus(true);ui->notifyJsMutation();}
    engine.uiInteraction().focusWebCanvas(ui);
}

void World::Impl::closeCheat() {
    cheatOpen=false;
    style("cheat","display","none");
    if(auto* input=ui->findElementById("cheat-input")){input->Blur();ui->notifyJsMutation();}
    engine.uiInteraction().focusWebCanvas(nullptr);
}

void World::Impl::cheatSay(const std::string& said) {text("cheat-said",said);cheatSaidAt=clock;}

void World::Impl::updateCheats(double dt) {
    if(smokeCheatCycles){cycleSmokeCheat(dt);return;}
    auto& w=engine.window();
    const bool key=w.keyDown(GLFW_KEY_T),escape=w.keyDown(GLFW_KEY_ESCAPE),
               enter=w.keyDown(GLFW_KEY_ENTER)||w.keyDown(GLFW_KEY_KP_ENTER);
    if(cheatOpen) {
        if(enter&&!wasCheatEnter){const std::string typed=value("cheat-input");closeCheat();runCheat(typed);}
        else if(escape&&!wasCheatEscape)closeCheat();
        // A click with the cursor captured lands on no control and takes
        // the canvas's focus away: the line keeps it while it is open.
        else if(engine.uiInteraction().focusedWebCanvas()!=ui)engine.uiInteraction().focusWebCanvas(ui);
    }
    // With the debug panel open, T is its trees (ui/performance.html).
    else if(key&&!wasCheatKey&&!performanceDebug&&!wasted&&!smoke)openCheat();
    wasCheatKey=key;wasCheatEnter=enter;wasCheatEscape=escape;
    style("cheat-said","display",!cheatOpen&&clock-cheatSaidAt<4.?"block":"none");
}

void World::Impl::runCheat(const std::string& typed) {
    using r1::cheats::Code;
    const Code code=r1::cheats::parse(typed);
    if(code==Code::Unknown&&typed.find_first_not_of(" \t\r\n")==std::string::npos)return;
    std::string refused;
    switch(code) {
    case Code::Helicopter: refused=spawnAircraft("helicopter");break;
    case Code::Plane: refused=spawnAircraft("bizjet");break;
    case Code::SportsCar: refused=spawnSportsCar();break;
    case Code::Moto: refused=spawnParkedVehicle("moto_city");break;
    case Code::Rocket: refused=spawnParkedVehicle("moto_cross");break;
    case Code::Invincible: health.protect(r1::cheats::kInvincibleSeconds);combatHud();break;
    case Code::Unknown: break;
    }
    cheatSay(code==Code::Unknown?"Unknown code: "+typed:refused.empty()?r1::cheats::said(code):refused);
    saida::Log::info("[World cheat] \"",typed,"\": ",code==Code::Unknown?"unknown":refused.empty()?"done":"refused");
}

bool World::Impl::roomBeside(double length,double width,bool ahead,bool wings,double& outLon,double& outLat) {
    const double s=std::sin(yaw*rad),c=std::cos(yaw*rad);
    auto at=[&](double x,double y,double along,double across){return onward(x,y,s*along+c*across,c*along-s*across);};
    auto clear=[&](double x,double y){
        if(!tile(x,y))return false;
        const double level=height(x,y,alt+1.);
        return std::abs(level-alt)<2.*kStepUp&&!blocked(x,y,level);
    };
    const double margin=driving()?2.5:1.;
    std::vector<glm::dvec2> spots;  // (along, across) from the player
    if(ahead)spots.push_back({length*.5+3.,0.});
    spots.push_back({0.,width*.5+2.5});spots.push_back({0.,-width*.5-2.5});
    if(!ahead)spots.push_back({length*.5+3.,0.});
    spots.push_back({-length*.5-3.,0.});
    const double inner=std::max(length,width)*.5+2.;
    for(int i=1;i<=160;++i) {
        const double a=i*2.39996323,d=inner+(40.-inner)*std::sqrt(i/160.);
        spots.push_back({d*std::cos(a),d*std::sin(a)});
    }
    for(const bool whole:{true,false}) {
        if(!whole&&!wings)break;
        for(const auto& spot:spots) {
            if(std::abs(spot.x)<length*.5+margin&&std::abs(spot.y)<width*.5+margin)continue;  // on him
            const auto centre=at(lon,lat,spot.x,spot.y);
            bool fits=true;
            for(double along:{-length*.5,0.,length*.5})
                for(double across:{-width*.5,0.,width*.5}) {
                    if(!fits)break;
                    const auto q=at(centre.x,centre.y,along,across);
                    fits=across!=0.&&!whole?tile(q.x,q.y)!=nullptr:clear(q.x,q.y);
                }
            // Nor on an aircraft standing there already: they have no body.
            for(const auto& a:leftAircraft)
                if(fits&&a.type&&metresBetween(a.lon,a.lat,centre.x,centre.y)<(std::max(a.type->length,a.type->span)+std::max(length,width))*.5)fits=false;
            if(!fits)continue;
            outLon=centre.x;outLat=centre.y;
            return true;
        }
    }
    return false;
}

std::string World::Impl::spawnAircraft(const std::string& name) {
    const auto* type=r1::aircraftType(name);
    const auto prototype=aircraftPrototypes.find(name);
    const bool rotor=type&&type->klass=="helicopter";
    if(!type||prototype==aircraftPrototypes.end()) {
        saida::Log::error("[World cheat] the aircraft fleet has no ",name);
        return "This aircraft is not in the fleet.";
    }
    if(piloting()&&plane.airborne) {
        saida::Log::info("[World cheat] no ",name," in flight");
        return "Not while flying — land first.";
    }
    double x=0,y=0;
    parkingCar=true;  // a shop a walker may enter is a wall to it
    const bool found=roomBeside(type->length,type->span,true,true,x,y);
    parkingCar=false;
    if(!found) {
        saida::Log::warn("[World cheat] no room for a ",name," (",type->length," x ",type->span,
                         " m) within 40 m of ",lon,", ",lat);
        return std::string("Not enough room here for ")+(rotor?"a helicopter":"a plane")+" — find an open spot.";
    }
    auto node=clonePlant(*prototype->second);
    node->setName("cheat-"+name);
    aircraftLod(*node);
    Aircraft a;
    a.node=engine.sceneTree().world().addChild(std::move(node));
    a.type=type;a.lon=x;a.lat=y;a.yaw=yaw;a.alt=height(x,y,alt+1.);
    collectAircraftParts(a);
    keepAircraft(std::move(a));
    saida::Log::info("[World cheat] a ",name," at ",x,", ",y," beside the player");
    return {};
}

std::string World::Impl::spawnSportsCar() {
    const auto spec=std::find_if(fleet.begin(),fleet.end(),[](const VehicleModel& m){return m.name=="sport";});
    if(spec==fleet.end()) {
        saida::Log::error("[World cheat] the road vehicle fleet has no sport car");
        return "No sports car in the fleet.";
    }
    if(piloting()&&plane.airborne) {
        saida::Log::info("[World cheat] no car in flight");
        return "Not while flying — land first.";
    }
    double x=0,y=0;
    parkingCar=true;
    const bool found=roomBeside(spec->length,spec->width,false,false,x,y);
    parkingCar=false;
    if(!found) {
        saida::Log::warn("[World cheat] no room for a car within 40 m of ",lon,", ",lat);
        return "Not enough room here for a car.";
    }
    auto node=clonePlant(*spec->prototype);
    vehicleLod(*node);vehicleCollider(*node,*spec);
    const auto red=std::max_element(paints.begin(),paints.end(),[](glm::vec3 a,glm::vec3 b){
        return a.x/(a.y+a.z+.01f)<b.x/(b.y+b.z+.01f);});
    if(red!=paints.end())paintCar(*node,*red);
    auto* root=engine.sceneTree().world().addChild(std::move(node));
    if(auto* drive=r1::VehicleRuntime::driver(*root))drive->parked=true;
    keepParked({root,x,y,groundAt(x,y,alt),yaw});
    placeCar();
    saida::Log::info("[World cheat] a sport car at ",x,", ",y," beside the player");
    return {};
}

std::string World::Impl::spawnParkedVehicle(const std::string& name) {
    const auto spec=std::find_if(fleet.begin(),fleet.end(),[&](const VehicleModel& m){return m.name==name;});
    if(spec==fleet.end()) {
        saida::Log::error("[World cheat] the road vehicle fleet has no ",name);
        return name=="moto_cross"?"No dirt bike in the fleet.":"No city motorcycle in the fleet.";
    }
    if(piloting()&&plane.airborne) {
        saida::Log::info("[World cheat] no ",name," while in flight");
        return "Not while flying — land first.";
    }
    double x=0,y=0;
    parkingCar=true;
    const bool found=roomBeside(spec->length,spec->width,false,false,x,y);
    parkingCar=false;
    if(!found) {
        saida::Log::warn("[World cheat] no room for a ",name," within 40 m of ",lon,", ",lat);
        return name=="moto_cross"?"Not enough room here for a dirt bike.":"Not enough room here for a motorcycle.";
    }
    auto node=clonePlant(*spec->prototype);
    vehicleCollider(*node,*spec);
    auto* root=engine.sceneTree().world().addChild(std::move(node));
    if(auto* drive=r1::VehicleRuntime::driver(*root))drive->parked=true;
    keepParked({root,x,y,groundAt(x,y,alt),yaw});
    placeCar();
    saida::Log::info("[World cheat] a ",name," at ",x,", ",y," beside the player");
    return {};
}

void World::Impl::startSmokeFight() {
    if(!smokeFought) {
        smokeFought=smokeFighting=true;smokeFightPhase=-1;smokeFightTime=0;
        hurtPlayer(r1::combat::kFists.damage*.65,{0,0,1},"a passer-by (smoke)");
        if(!playerDown||wasted||!playerFall.ragdoll()->simulating()||!feet->physicsSuspended()) {
            saida::Log::error("[World E2E] FAIL fight: a nonlethal hit did not replace the player's capsule with physics");
            testFailed=true;engine.sceneTree().quit();
        }
        return;
    }
    // People appear out of sight (r1::Crowd::kSpawnNear and beyond): the
    // nearest anywhere in the neighbourhood will do for the physics.
    const glm::vec3 me=player->transform().position;
    const double reach=r1::Crowd::kDespawn;
    double best=reach;
    for(auto& [key,t]:loaded) {
        const auto& walkers=t.crowd.walkers();
        for(size_t i=0;i<t.people.size()&&i<walkers.size();++i) {
            if(!t.people[i].node||!t.people[i].node->enabled()||!walkers[i].alive)continue;
            const double d=glm::length(nodePosition(*t.people[i].node)-me);
            if(d<best){best=d;smokeFightTile=key;smokeFightSlot=i;}
        }
    }
    if(best>=reach) {
        saida::Log::info("[World E2E] fight: nobody in the neighbourhood to knock down; skipped");
        if(!smokeDroveOnce&&!onSeaIce(lon,lat))startSmokeDrive();else smokeFinishPhase();
        return;
    }
    Loaded* t=tileByKey(smokeFightTile);
    const glm::vec3 d=glm::normalize(nodePosition(*t->people[smokeFightSlot].node)-me+glm::vec3(0,.001f,0));
    if(!knockDown(*t,smokeFightSlot,d*3.f+glm::vec3(0,1.5f,0),glm::cross(glm::vec3(0,1,0),d)*3.f)) {
        saida::Log::error("[World E2E] FAIL fight: the knocked-down body did not enter the physics");
        testFailed=true;engine.sceneTree().quit();return;
    }
    selectWeapon(r1::combat::Weapon::Pistol);
    smokeFighting=true;smokeFightTime=0;smokeFightPhase=0;
    saida::Log::info("[World E2E] fight: knocked down a passer-by ",best," m away");
}

void World::Impl::runSmokeFight(double dt) {
    smokeFightTime+=dt;
    if(smokeFightPhase==-1) {
        if(playerDown&&smokeFightTime<12.)return;
        if(playerDown||wasted||feet->innerBodyId().IsInvalid()||feet->physicsSuspended()) {
            saida::Log::error("[World E2E] FAIL fight: the player did not recover his capsule after a nonlethal hit");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS player hit: nonlethal ragdoll, recovery and restored capsule");
        smokeFighting=false;startSmokeFight();return;
    }
    Loaded* t=tileByKey(smokeFightTile);
    if(!t||smokeFightSlot>=t->people.size()||!t->people[smokeFightSlot].fall.ragdoll()) {
        saida::Log::error("[World E2E] FAIL fight: the knocked-down body's tile left before it came to rest");
        testFailed=true;engine.sceneTree().quit();return;
    }
    auto* ragdoll=t->people[smokeFightSlot].fall.ragdoll();
    if(smokeFightPhase==0) {
        if(ragdoll->state()==saida::RagdollBehaviour::State::Simulated&&smokeFightTime<8.)return;
        const auto state=ragdoll->state();
        if(state==saida::RagdollBehaviour::State::Simulated) {
            saida::Log::error("[World E2E] FAIL fight: the body still tumbles after 8 s (fastest part ",
                              ragdoll->ragdoll()->fastestSpeed()," m/s)");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] fight: the body came to rest in ",smokeFightTime," s (",
                         state==saida::RagdollBehaviour::State::Frozen?"frozen":"getting up",")");
        const size_t magazine=size_t(arsenal.loaded());
        fire();
        if(size_t(arsenal.loaded())+1!=magazine) {
            saida::Log::error("[World E2E] FAIL fight: the pistol did not fire (",arsenal.loaded()," of ",magazine,")");
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeFightPhase=1;smokeFightTime=0;
        return;
    }
    if(smokeFightTime<1.)return;
    selectWeapon(r1::combat::Weapon::Fists);
    smokeFighting=false;
    saida::Log::info("[World E2E] PASS fight: knock-down, rest and a pistol round");
    if(!smokeDroveOnce&&!onSeaIce(lon,lat)){startSmokeDrive();return;}
    smokeFinishPhase();
}

} // namespace r1::world
