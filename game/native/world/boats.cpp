#include "session.hpp"

namespace r1::world {
double World::Impl::segmentDistance(glm::dvec2 p,glm::dvec2 a,glm::dvec2 b) {
    const glm::dvec2 d=b-a;const double len=glm::dot(d,d);
    const double f=len>0?std::clamp(glm::dot(p-a,d)/len,0.,1.):0.;
    return glm::length(p-(a+f*d));
}

double World::Impl::hullDistance(const Frame& frame,glm::dvec3 centre,double heading,double length,double beam) const {
    const glm::dvec3 me=frame.local(ecef(lon,lat,alt));
    const glm::dvec2 axis(std::sin(heading*rad),-std::cos(heading*rad));
    const glm::dvec2 c(centre.x,centre.z);
    return segmentDistance({me.x,me.z},c-axis*(length*.5),c+axis*(length*.5))-beam*.5;
}

saida::Node* World::Impl::mooredNode(Loaded& t,Mooring& m) {
    if(!m.node&&!m.taken)m.node=t.node->findByPath(m.name);
    return m.node;
}

bool World::Impl::nearestBoat(BoatReach& out,double& distance,double extra) {
    distance=kBoatReach+extra;bool found=false;
    for(auto& [key,t]:loaded)
        for(size_t i=0;i<t.boats.size();++i) {
            Mooring& m=t.boats[i];
            if(m.taken||!mooredNode(t,m))continue;
            const double d=hullDistance(t.frame,m.local,m.heading,m.length,m.beam);
            if(d<distance){distance=d;out={&t,i,BoatSource::Moored};found=true;}
        }
    for(size_t i=0;i<leftBoats.size();++i) {
        const Vessel& v=leftBoats[i];
        const Frame here(v.lon,v.lat,v.alt);
        const double d=hullDistance(here,glm::dvec3(0),v.yaw,v.length,v.beam);
        if(d<distance){distance=d;out={nullptr,i,BoatSource::Left};found=true;}
    }
    for(size_t i=0;i<seaShips.size();++i) {
        const Vessel& v=seaShips[i].v;
        const Frame here(v.lon,v.lat,v.alt);
        const double d=hullDistance(here,glm::dvec3(0),v.yaw,v.length,v.beam);
        if(d<distance){distance=d;out={nullptr,i,BoatSource::Sea};found=true;}
    }
    return found;
}

bool World::Impl::enterBoat() {
    if(sailing||driving)return false;
    BoatReach target;double distance=0;
    if(!nearestBoat(target,distance))return false;
    return board(target);
}

bool World::Impl::board(const BoatReach& target) {
    if(target.source==BoatSource::None)return false;
    if(target.source==BoatSource::Sea) {
        // A ship under way is taken under way: the helm keeps her speed.
        boat=seaShips[target.index].v;
        seaTaken.insert(seaShips[target.index].id);
        seaShips.erase(seaShips.begin()+long(target.index));
    } else if(target.source==BoatSource::Left) {
        boat=leftBoats[target.index];
        leftBoats.erase(leftBoats.begin()+long(target.index));
    } else {
        Loaded& t=*target.tile;Mooring& m=t.boats[target.index];
        auto owned=t.node->detachChild(m.node);
        if(!owned) {
            saida::Log::warn("[World boat] ",m.name," could not be detached from its tile");
            return false;
        }
        m.taken=true;m.node=nullptr;
        const auto where=geodeticOf(t,glm::vec3(m.local));
        boat=Vessel{};
        boat.node=engine.sceneTree().world().addChild(std::move(owned));
        boat.kind=m.kind;boat.length=m.length;boat.beam=m.beam;
        boat.top=m.top;boat.accel=m.accel;boat.turn=m.turn;
        boat.lon=where.x;boat.lat=where.y;boat.yaw=m.heading;
        boat.alt=waterLevel(boat.lon,boat.lat);
    }
    sailing=true;swimming=false;lookYaw=0;lookIdle=0;
    lon=boat.lon;lat=boat.lat;alt=boat.alt;yaw=boat.yaw;
    player->setEnabled(false);
    for(auto* a:animators)a->play("idle");
    jumpOffset=jumpVelocity=0;
    text("stream-status","At the helm. F: disembark near a quay · S: reverse.");
    saida::Log::info("[World boat] took the helm of a ",boat.kind," (",boat.length," m) at ",lon,", ",lat);
    return true;
}

void World::Impl::keepBoat(Vessel vessel) {
    vessel.speed=0;
    leftBoats.push_back(std::move(vessel));
    while(leftBoats.size()>kLeftBoats) {
        if(leftBoats.front().node)leftBoats.front().node->queueFree();
        leftBoats.erase(leftBoats.begin());
    }
}

std::optional<World::Impl::BoatExit> World::Impl::boatExit() {
    const double s=std::sin(boat.yaw*rad),c=std::cos(boat.yaw*rad);
    std::optional<BoatExit> water;
    for(double off:{1.,2.,3.5})
        for(double along=0;along<=boat.length*.5;along+=2.)
            for(double sign:{1.,-1.})for(double side:{1.,-1.}) {
                const double a=along*sign,o=(boat.beam*.5+off)*side;
                const auto q=onward(boat.lon,boat.lat,s*a+c*o,c*a-s*o);
                if(!tile(q.x,q.y))continue;
                if(deepWater(q.x,q.y)) {
                    if(!water)water=BoatExit{q,true};
                } else if(!footBlocked(q.x,q.y))return BoatExit{q,false};
            }
    return water;
}

bool World::Impl::leaveBoat() {
    if(!sailing)return false;
    if(std::abs(boat.speed)>kBoatExitSpeed) {
        text("stream-status","Too fast to disembark — slow down.");
        saida::Log::info("[World boat] landing refused at ",std::abs(boat.speed)," m/s");
        return false;
    }
    const auto exit=boatExit();
    if(!exit) {
        text("stream-status","Water and shore outside the loaded area — wait for the data.");
        saida::Log::info("[World boat] no loaded exit beside ",boat.lon,", ",boat.lat);
        return false;
    }
    sailing=false;keepBoat(boat);boat=Vessel{};
    yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
    lon=exit->where.x;lat=exit->where.y;swimming=exit->swimming;
    swimTime=0;swimHeading=yaw;swimLean=0;
    alt=swimming?waterLevel(lon,lat):height(lon,lat);
    player->setEnabled(true);
    player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
    followDistance=std::min(followDistance,kOnFootFollow);
    text("stream-status",swimming?"In the water. WASD/ZQSD: swim · F: climb back aboard."
                                 :"Ashore. F: take the boat again.");
    saida::Log::info(swimming?"[World boat] swimming at ":"[World boat] ashore at ",lon,", ",lat);
    request(lon,lat);
    return true;
}

World::Impl::HopResult World::Impl::hopAboard() {
    if(!sailing)return HopResult::NoTarget;
    BoatReach target;double distance=0;
    if(!nearestBoat(target,distance,boat.beam*.5))return HopResult::NoTarget;
    const Vessel* other=target.source==BoatSource::Sea?&seaShips[target.index].v
                        :target.source==BoatSource::Left?&leftBoats[target.index]:nullptr;
    const glm::dvec2 mine(std::sin(boat.yaw*rad)*boat.speed,std::cos(boat.yaw*rad)*boat.speed);
    const glm::dvec2 theirs=other?glm::dvec2(std::sin(other->yaw*rad)*other->speed,
                                              std::cos(other->yaw*rad)*other->speed):glm::dvec2(0);
    const double relativeSpeed=glm::length(mine-theirs);
    if(relativeSpeed>kHopSpeed) {
        text("stream-status","Match its speed to step aboard.");
        saida::Log::info("[World boat] hop refused at ",relativeSpeed," m/s relative");
        return HopResult::Refused;
    }
    Vessel left=boat;left.speed=0;
    sailing=false;boat=Vessel{};
    if(!board(target)) {boat=left;sailing=true;return HopResult::Refused;}
    keepBoat(std::move(left));
    saida::Log::info("[World boat] stepped across to a ",boat.kind," (",boat.length," m)");
    return HopResult::Boarded;
}

void World::Impl::clearBoats() {
    for(auto& v:leftBoats)if(v.node)v.node->queueFree();
    leftBoats.clear();
    for(auto& s:seaShips)if(s.v.node)s.v.node->queueFree();
    seaShips.clear();seaTaken.clear();seaFirst=true;seaAsk=5.;
    if(boat.node)boat.node->queueFree();
    boat=Vessel{};sailing=false;
}

void World::Impl::sailBoat(double dt,double throttle,double rudder) {
    const double drag=boat.accel/(boat.top*boat.top);
    double thrust=0;
    if(throttle>0)thrust=boat.accel*throttle;
    else if(throttle<0)thrust=boat.speed>.3?-2.*boat.accel:boat.accel*throttle*.5;
    double coast=throttle==0?.12*boat.accel*(boat.speed>0?1.:boat.speed<0?-1.:0.):0.;
    boat.speed+=(thrust-drag*boat.speed*std::abs(boat.speed)-coast)*dt;
    if(throttle==0&&std::abs(boat.speed)<.05)boat.speed=0;
    boat.speed=std::clamp(boat.speed,-.3*boat.top,boat.top);
    const double way=std::clamp(std::abs(boat.speed)/(.25*boat.top),.15,1.);
    boat.yaw=wrap(boat.yaw+rudder*boat.turn*way*dt*(boat.speed<-.05?-1.:1.));
    yaw=boat.yaw;
    if(std::abs(boat.speed)<1e-3)return;
    const double s=std::sin(boat.yaw*rad),c=std::cos(boat.yaw*rad);
    auto next=onward(lon,lat,s*boat.speed*dt,c*boat.speed*dt);
    const double lead=boat.length*.5*(boat.speed>=0?1.:-1.);
    auto bow=onward(next.x,next.y,s*lead,c*lead);
    if(!tile(next.x,next.y)||!tile(bow.x,bow.y))stream();
    if(!tile(next.x,next.y)||!tile(bow.x,bow.y)) {
        boat.speed=0;
        text("stream-status","Edge of the loaded area — more data is on its way.");
        return;
    }
    if(!navigable(next.x,next.y)||!navigable(bow.x,bow.y)) {
        // Aground is a refusal like any other: the boat stops, and the
        // player is told which way is out.
        boat.speed=0;
        text("stream-status","Shore or shallows — the boat stops. S: reverse.");
        return;
    }
    lon=next.x;lat=next.y;alt=waterLevel(lon,lat);
    boat.lon=lon;boat.lat=lat;boat.alt=alt;
    request(lon,lat);
}

glm::quat World::Impl::boatSway(const Vessel& v,double phase,double& heave) const {
    const double size=std::min(1.,9./std::max(1.,v.length));
    heave=.12*size*std::sin(seaTime*1.1+phase);
    const double pitch=2.5*size*std::sin(seaTime*1.3+phase*1.7);
    const double roll=3.5*size*std::sin(seaTime*.9+phase*.6);
    return glm::angleAxis(float(-v.yaw*rad),glm::vec3(0,1,0))
          *glm::angleAxis(float(pitch*rad),glm::vec3(1,0,0))
          *glm::angleAxis(float(roll*rad),glm::vec3(0,0,1));
}

void World::Impl::placeBoats() {
    auto place=[&](const Vessel& v,double phase){
        if(!v.node)return;
        double heave=0;const glm::quat sway=boatSway(v,phase,heave);
        v.node->transform().position=glm::vec3(origin.local(ecef(v.lon,v.lat,v.alt+heave)));
        v.node->transform().rotation=sway;
    };
    if(sailing)place(boat,0.);
    for(size_t i=0;i<leftBoats.size();++i)place(leftBoats[i],double(i+1)*1.9);
    for(auto& s:seaShips)place(s.v,s.phase);
}

} // namespace r1::world
