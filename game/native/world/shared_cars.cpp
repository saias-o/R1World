#include "session.hpp"
#include "parking_rules.hpp"

// Multiplayer on, the world is the same for everyone in it (session.hpp,
// sharedWorld): no traffic, passers-by or police, which each machine would
// invent for itself, and parked cars at the same places for everyone
// (parking_rules.hpp). Whoever takes one owns it and says where it is
// (online.cpp, o::Vehicle); the others draw it there and leave its place empty.

namespace r1::world {

void World::Impl::applySharedWorld() {
    const bool shared=sharedWorld();
    if(shared==sharedApplied)return;
    sharedApplied=shared;
    saida::Log::info("[World online] ",shared?"multiplayer on: no traffic, passers-by or police; the parked cars are the same for everyone"
                                              :"multiplayer off: traffic and passers-by are back");
    for(auto* tiles:{&loaded,&incoming})for(auto& [key,tile]:*tiles) {
        // The traffic leaves at once, not when it is out of sight: a car in
        // view that nobody else sees is a car another player drives through.
        for(size_t i=0;i<tile.flow.agents().size();++i)if(tile.flow.agents()[i].alive)tile.flow.retire(i);
        for(auto* node:tile.cars)if(node)node->setEnabled(false);
        // The passers-by too; the crowd fills the streets again on its own.
        std::vector<r1::CrowdPace> paces;
        for(size_t kind:tile.humanKinds)paces.push_back(crowdKinds[kind].pace);
        if(!paces.empty())tile.crowd.reset(&tile.walks,uint32_t(std::hash<std::string>{}(tile.data.at("key").get<std::string>()))|1u,std::move(paces));
        for(auto& person:tile.people)if(person.node){person.node->setEnabled(false);person.shown=false;person.punch=-1;}
        if(!shared)clearParking(tile);
    }
    if(shared) {
        clearPolice();
        wanted=r1::police::Wanted{};
        // His own world's cars are not anyone else's: the one beside him and
        // the ones he left go; the one he drives, he steps out of first.
        if(driving()&&car&&!ownedCars.count(car)) {
            leaveCar();
            text("stream-status","Multiplayer on: that car was your own world's. The parked cars are everyone's.");
        }
        if(car&&!ownedCars.count(car)&&!driving()){car->setEnabled(false);carParked=false;}
        for(auto it=parked.begin();it!=parked.end();)
            if(ownedCars.count(it->node))++it;
            else{it->node->queueFree();it=parked.erase(it);}
    } else {
        ownedCars.clear();takenCars.clear();
        for(auto& [model,nodes]:parkedSpares)for(auto* node:nodes)node->queueFree();
        parkedSpares.clear();
    }
}

void World::Impl::readParking(Loaded& tile) {
    tile.parkingRead=true;
    if(tile.graph.lanes.empty())return;
    const std::string key=tile.data.at("key").get<std::string>();
    const bool leftHand=tile.data.value("traffic",json::object()).value("leftHand",false);
    size_t outside=0;
    for(const auto& spot:r1::parking::kerbSpots(tile.graph,leftHand,-1.f,key)) {
        const auto& lane=tile.graph.lanes[spot.lane];
        const float span=tile.graph.laneLength(spot.lane);
        const float t=span>0?spot.s/span:0.f;
        // On the ground itself: the street graph's heights are the cook's,
        // before the tiles' ground was joined at the edges (gen/seams), and
        // stood a parked car 1.9 m above a Paris quay.
        const float road=tile.groundUp[lane.from]*(1-t)+tile.groundUp[lane.to]*t;
        const float up=float(groundUnder(tile,spot.x,spot.y,road));
        // A street's graph runs into the neighbours: each place belongs to
        // the tile it stands in, or two tiles would park two cars on it.
        const auto point=tile.frame.origin+tile.frame.basis*glm::dvec3(spot.x,up,spot.y);
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        if(tileAt(geo.x,geo.y).key()!=key){++outside;continue;}
        size_t model=0;
        for(size_t i=0;i<fleet.size();++i)if(fleet[i].name==r1::parking::modelFor(spot.seed))model=i;
        tile.kerbCars.push_back({spot,model,up,nullptr});
    }
    if(!tile.kerbCars.empty())
        saida::Log::info("[World parking] ",key,": ",tile.kerbCars.size()," parked cars (",outside," places in the neighbours' streets)");
}

// A parked car out of reach waits under a node of its own, never under its
// tile: a tile that leaves frees its children, and a spare kept there was
// a dangling pointer the next tile picked up (both smoke instances crashed
// on it the first time).
void World::Impl::releaseParked(Loaded& tile,Loaded::ParkedSpot& p) {
    if(!p.node)return;
    if(r1::VehicleRuntime::physical(*p.node))r1::VehicleRuntime::deactivate(*p.node);
    if(!parkingPool) {
        auto pool=std::make_unique<saida::Node>("Parked cars out of reach");
        pool->setEnabled(false);
        parkingPool=engine.sceneTree().world().addChild(std::move(pool));
    }
    if(auto owned=tile.node->detachChild(p.node))parkedSpares[p.model].push_back(parkingPool->addChild(std::move(owned)));
    p.node=nullptr;
}

void World::Impl::clearParking(Loaded& tile) {
    for(auto& p:tile.kerbCars)releaseParked(tile,p);
}

saida::Node* World::Impl::parkedNode(Loaded& tile,size_t model,uint32_t seed) {
    saida::Node* node=nullptr;
    auto& spares=parkedSpares[model];
    if(!spares.empty()) {
        auto owned=parkingPool->detachChild(spares.back());
        spares.pop_back();
        if(owned)node=tile.node->addChild(std::move(owned));
    }
    if(!node) {
        auto made=clonePlant(*fleet[model].prototype);
        vehicleCollider(*made,fleet[model]);
        node=tile.node->addChild(std::move(made));
    }
    r1::VehicleRuntime::reset(*node);
    paintCar(*node,paintFor(seed));
    node->setEnabled(true);
    return node;
}

void World::Impl::updateParking() {
    applySharedWorld();
    if(!sharedWorld()||fleet.empty())return;
    for(auto& [key,tile]:loaded)if(!tile.parkingRead)readParking(tile);
    const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if(now<parkingAt)return;
    parkingAt=now+.5;
    // The nearest places get a car, up to the budget; a place someone took
    // stays empty for the session, wherever its car went.
    struct Near {double d;Loaded* tile;size_t i;};
    std::vector<Near> near;
    for(auto& [key,tile]:loaded) {
        const glm::dvec3 eye=cameraTileLocal(tile);
        for(size_t i=0;i<tile.kerbCars.size();++i) {
            const auto& p=tile.kerbCars[i];
            const double d=std::hypot(p.spot.x-eye.x,p.spot.y-eye.z);
            if(d<kParkedRadius&&!takenCars.count(p.spot.id))near.push_back({d,&tile,i});
        }
    }
    std::sort(near.begin(),near.end(),[](const Near& a,const Near& b){return a.d<b.d;});
    if(near.size()>kParkedShown)near.resize(kParkedShown);
    std::set<std::pair<Loaded*,size_t>> keep;
    for(const auto& n:near)keep.insert({n.tile,n.i});
    for(auto& [key,tile]:loaded)for(size_t i=0;i<tile.kerbCars.size();++i) {
        auto& p=tile.kerbCars[i];
        if(p.node&&!keep.count({&tile,i}))releaseParked(tile,p);
    }
    for(const auto& n:near) {
        auto& p=n.tile->kerbCars[n.i];
        if(p.node)continue;
        p.node=parkedNode(*n.tile,p.model,p.spot.seed);
        const saida::traffic::Vec2 dir{std::sin(p.spot.yaw),std::cos(p.spot.yaw)};
        const double bearing=std::atan2(double(dir.x),-double(dir.y));  // a tile's z points south
        const auto& lane=n.tile->graph.lanes[p.spot.lane];
        const float rise=(n.tile->groundUp[lane.to]-n.tile->groundUp[lane.from])/std::max(.05f,n.tile->graph.laneLength(p.spot.lane));
        p.node->transform().position=glm::vec3(p.spot.x,p.up+r1::VehicleRuntime::kBodyClearance,p.spot.y);
        p.node->transform().rotation=glm::angleAxis(float(-bearing),glm::vec3(0,1,0))*glm::angleAxis(std::atan(rise),glm::vec3(1,0,0));
    }
    // Solid within reach, as a parked car of his own is (updateTraffic).
    for(const auto& n:near) {
        auto* node=n.tile->kerbCars[n.i].node;
        if(n.d<r1::VehicleRuntime::kPromotionDistance&&!r1::VehicleRuntime::physical(*node))
            if(auto* physics=engine.sceneTree().world().physics()) {
                node->updateTransforms(n.tile->node->worldTransform(),true);
                r1::VehicleRuntime::activate(*node,*physics,glm::vec3(0));
            }
    }
}

bool World::Impl::takeParkedCar(const Reach& target) {
    Loaded& tile=*target.tile;
    auto& p=tile.kerbCars[target.agent];
    saida::Node* node=p.node;
    r1::VehicleRuntime::synchronize(*node);
    node->updateTransforms(tile.node->worldTransform(),true);
    const glm::mat4 previousWorld=node->worldTransform();
    auto owned=tile.node->detachChild(node);
    if(!owned) {
        saida::Log::warn("[World parking] the parked car could not be detached from ",tile.data.value("key",std::string()));
        return false;
    }
    p.node=nullptr;
    takenCars.insert(p.spot.id);
    abandonCurrentCar();
    car=engine.sceneTree().world().addChild(std::move(owned));
    car->transform().position=glm::vec3(previousWorld[3]);
    car->transform().rotation=glm::quat_cast(glm::mat3(previousWorld));
    const glm::vec3 world=car->transform().position;
    const auto point=origin.origin+origin.basis*glm::dvec3(world);
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    carParked=true;carLon=geo.x;carLat=geo.y;carAlt=groundAt(carLon,carLat,alt);
    const glm::vec3 forward=car->transform().rotation*glm::vec3(0,0,-1);
    carYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
    carSpeed=0;wheelSpin=0;steerShown=0;
    collectWheels(*car);
    // The first take of this place's car; another player who took it too
    // is settled by o::outranks.
    ownedCars[car]={p.spot.id,p.spot.seed,uint8_t(p.model),1,0};
    saida::Log::info("[World parking] took the parked car ",p.spot.id," at ",carLon,", ",carLat);
    return true;
}

r1::online::Car World::Impl::carState(saida::Node& node) const {
    r1::online::Car c;
    const bool mine=&node==car;
    const glm::quat q=node.transform().rotation;
    const glm::vec3 forward=q*glm::vec3(0,0,-1),right=q*glm::vec3(1,0,0);
    const auto point=origin.origin+origin.basis*glm::dvec3(node.transform().position);
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    c.lon=geo.x;c.lat=geo.y;c.alt=float(geo.z-r1::VehicleRuntime::kBodyClearance);
    c.yaw=float(std::atan2(double(forward.x),-double(forward.z))/rad);
    c.pitch=float(std::asin(std::clamp(double(forward.y),-1.,1.))/rad);
    c.roll=float(std::asin(std::clamp(double(-right.y),-1.,1.))/rad);
    c.driven=mine&&driving();
    c.speed=c.driven?float(carSpeed):0.f;
    return c;
}

void World::Impl::forgetOwnedCar(saida::Node* node) {
    ownedCars.erase(node);
}

} // namespace r1::world
