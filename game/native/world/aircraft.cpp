#include "session.hpp"

namespace r1::world {
void World::Impl::aircraftLod(saida::Node& node) {
    auto* lod=node.addBehaviour<saida::LODGroupBehaviour>();
    lod->setLevels({{"Near",.03f},{"Far",0.f}});
}

void World::Impl::buildAircraftPrototypes() {
    for(const auto& type:r1::palette().aircraft) {
        auto root=std::make_unique<saida::Node>("aircraft-"+type.name);
        for(const auto& level:std::vector<std::pair<std::string,std::string>>{{type.nearModel,"Near"},{type.farModel,"Far"}}) {
            auto* source=prototypeForStartup(level.first);
            if(!source)throw std::runtime_error("Aircraft model failed: "+level.first);
            auto child=clonePlant(*source);child->setName(level.second);
            child->transform().rotation=glm::quat(0,0,1,0); // Authored nose +Z.
            root->addChild(std::move(child));
        }
        aircraftPrototypes[type.name]=prototypes->addChild(std::move(root));
    }
    saida::Log::info("[World aircraft] ",aircraftPrototypes.size()," aircraft types ready");
}

std::unique_ptr<saida::Node> World::Impl::lettering(const std::string& text,double width,double height,glm::vec3 colour) {
    auto letters=r1::facadeLettering(text,width,height);if(!letters)return nullptr;
    auto sign=plantNode(*letters);if(!sign)return nullptr;
    saida::MaterialDesc desc;
    desc.baseColor={colour.x,colour.y,colour.z,1.f};
    desc.roughness=.55f;desc.alphaCutoff=.35f;
    desc.albedoId=texture("assets/textures/interiors/fascia_letters.png",true);
    auto* ink=engine.resources().getMaterial(desc);
    std::function<void(saida::Node&)> paint=[&](saida::Node& node) {
        if(auto* mesh=dynamic_cast<saida::MeshNode*>(&node)){mesh->setMaterial(ink);mesh->castShadows()=false;}
        for(auto& child:node.children())paint(*child);
    };
    paint(*sign);
    return sign;
}

void World::Impl::mountLettering(Loaded& t) {
    const auto started=std::chrono::steady_clock::now();
    auto list=t.data.find("lettering");if(list==t.data.end())return;
    while(t.nextLettering<list->size()) {
        const auto& entry=(*list)[t.nextLettering];
        const std::string text=entry.at("text");
        const auto letters=r1::facadeLettering(text,entry.at("width"),entry.at("height"));
        if(letters&&!modelsReady(*letters))return;
        ++t.nextLettering;
        const auto& c=entry.at("colour");
        auto sign=lettering(text,entry.at("width"),entry.at("height"),{float(c[0]),float(c[1]),float(c[2])});
        if(!sign){saida::Log::warn("[World lettering] font cannot spell: ",text);continue;}
        const auto& at=entry.at("at");const auto& n=entry.at("normal");
        sign->transform().position={float(at[0]),float(at[1]),float(at[2])};
        sign->transform().rotation=glm::angleAxis(float(std::atan2(n[0].get<double>(),n[1].get<double>())),glm::vec3(0,1,0));
        t.node->addChild(std::move(sign));
        if(msSince(started)>=kPropImportMs)return;
    }
}

void World::Impl::mountAircraft(Loaded& t) {
    for(auto& spot:t.aircraft) {
        auto found=aircraftPrototypes.find(spot.type);
        if(found==aircraftPrototypes.end()) {
            saida::Log::warn("[World aircraft] ",t.data.value("key",std::string())," parks an unknown type: ",spot.type);
            continue;
        }
        auto node=clonePlant(*found->second);
        node->setName(spot.name);
        aircraftLod(*node);
        node->transform().position=glm::vec3(spot.local);
        node->transform().rotation=glm::angleAxis(float(-spot.heading*rad),glm::vec3(0,1,0));
        spot.node=t.node->addChild(std::move(node));
    }
}

void World::Impl::collectAircraftParts(Aircraft& a) {
    if(a.node&&a.type)armAircraft(*a.node,*a.type);
    a.gear.clear();a.mainRotors.clear();a.tailRotors.clear();
    // Outermost match only, as with the car's wheels: the importer wraps a
    // named node around a mesh node that inherits its name.
    std::function<void(saida::Node&)> walk=[&](saida::Node& n){
        if(n.name()=="gear"){a.gear.push_back(&n);return;}
        if(n.name()=="rotor-main"){a.mainRotors.push_back(&n);return;}
        if(n.name()=="rotor-tail"){a.tailRotors.push_back(&n);return;}
        for(auto& c:n.children())walk(*c);
    };
    if(a.node)walk(*a.node);
}

double World::Impl::fuselageHalf(const r1::AircraftType& t) {return std::max(1.2,t.length*.05);}

double World::Impl::roofAt(double x,double y,double above,saida::CollisionObjectNode* ignore) {
    auto* physics=engine.sceneTree().world().physics();if(!physics)return -1e30;
    const glm::vec3 start(origin.local(ecef(x,y,above+1.)));
    auto filter=obstacleFilter();if(ignore)filter.ignore=ignore->bodyId();
    const auto hit=physics->raycast(start,{0,-1,0},500.f,filter);
    if(!hit.hit||hit.normal.y<.5f)return -1e30;
    const double surface=above+1.+double(hit.point.y-start.y);
    return surface>groundAt(x,y,above)+1.?surface:-1e30;
}

bool World::Impl::buildingAt(double x,double y,double level) {
    return solidAt(x,y,level,.45f);
}

double World::Impl::surfaceUnder(double x,double y,double above,bool* onRoof,bool* wet) {
    if(onRoof)*onRoof=false;
    if(wet)*wet=false;
    if(!tile(x,y))return planeGround;
    double surface=height(x,y,above);
    if(onWater(x,y)){surface=waterLevel(x,y);if(wet)*wet=true;}
    const double roof=roofAt(x,y,above);
    if(roof>surface){surface=roof;if(onRoof)*onRoof=true;if(wet)*wet=false;}
    planeGround=surface;
    return surface;
}

bool World::Impl::aircraftHits(double x,double y,double alt,double heading,bool sweep) {
    const auto& t=*plane.type;
    const double s=std::sin(heading*rad),c=std::cos(heading*rad);
    const double half=t.length*.5,span=t.span*.5,body=alt+.4;
    for(const auto& [a,o]:{std::pair{0.,0.},std::pair{half,0.},std::pair{-half,0.},std::pair{0.,span},std::pair{0.,-span}}) {
        const auto q=onward(x,y,s*a+c*o,c*a-s*o);
        if(buildingAt(q.x,q.y,body))return true;
        if(sweep) {
            const auto from=onward(plane.lon,plane.lat,s*a+c*o,c*a-s*o);
            const glm::vec3 start(origin.local(ecef(from.x,from.y,body))),end(origin.local(ecef(q.x,q.y,body)));
            const glm::vec3 delta=end-start;const float length=glm::length(delta);
            auto* physics=engine.sceneTree().world().physics();
            if(physics&&length>.001f&&physics->raycast(start,delta/length,length,obstacleFilter()).hit)return true;
        }
    }
    return false;
}

bool World::Impl::nearestAircraft(PlaneReach& out,double& distance) {
    distance=kAircraftReach;bool found=false;
    for(auto& [key,t]:loaded)
        for(size_t i=0;i<t.aircraft.size();++i) {
            const AircraftSpot& spot=t.aircraft[i];
            const auto* type=r1::aircraftType(spot.type);
            if(spot.taken||!spot.node||!type)continue;
            const double d=hullDistance(t.frame,spot.local,spot.heading,type->length,2*fuselageHalf(*type));
            if(d<distance){distance=d;out={&t,i,PlaneSource::Parked};found=true;}
        }
    for(size_t i=0;i<leftAircraft.size();++i) {
        const Aircraft& a=leftAircraft[i];
        if(a.airborne)continue;  // still falling
        const Frame here(a.lon,a.lat,a.alt);
        const double d=hullDistance(here,glm::dvec3(0),a.yaw,a.type->length,2*fuselageHalf(*a.type));
        if(d<distance){distance=d;out={nullptr,i,PlaneSource::Left};found=true;}
    }
    return found;
}

bool World::Impl::enterAircraft() {
    if(piloting||driving||sailing)return false;
    PlaneReach target;double distance=0;
    if(!nearestAircraft(target,distance))return false;
    Aircraft a;
    if(target.source==PlaneSource::Left) {
        a=leftAircraft[target.index];
        leftAircraft.erase(leftAircraft.begin()+long(target.index));
    } else {
        Loaded& t=*target.tile;AircraftSpot& spot=t.aircraft[target.index];
        auto owned=t.node->detachChild(spot.node);
        if(!owned) {
            saida::Log::warn("[World aircraft] ",spot.name," could not be detached from its tile");
            text("stream-status","This aircraft cannot be taken.");
            return false;
        }
        spot.taken=true;spot.node=nullptr;
        const auto where=geodeticOf(t,glm::vec3(spot.local));
        a.node=engine.sceneTree().world().addChild(std::move(owned));
        a.type=r1::aircraftType(spot.type);
        a.lon=where.x;a.lat=where.y;a.yaw=spot.heading;
        a.alt=surfaceUnder(a.lon,a.lat,alt+1.);
    }
    a.speed=0;a.climb=0;a.lever=0;a.thrust=0;a.airborne=false;a.pitch=0;a.roll=0;
    collectAircraftParts(a);
    plane=std::move(a);piloting=true;swimming=false;planeStopped=false;
    if(plane.helicopter())selectWeapon(r1::combat::Weapon::RocketLauncher);
    lookYaw=0;lookIdle=0;
    lon=plane.lon;lat=plane.lat;alt=plane.alt;yaw=plane.yaw;
    player->setEnabled(false);
    for(auto* anim:animators)anim->play("idle");
    jumpOffset=jumpVelocity=0;
    text("stream-status",plane.helicopter()
        ?"Flying. Click: rocket · 3: launcher · E: reload · Space/Shift: climb/descend · F: get out."
        :"Flying. W/S: throttle · A/D: bank · Space/Shift: pitch up/down · F: get out.");
    saida::Log::info("[World aircraft] took the controls of a ",plane.type->name," at ",lon,", ",lat,
                     plane.helicopter()?" (helicopter)":"");
    return true;
}

void World::Impl::keepAircraft(Aircraft a) {
    a.speed=0;a.climb=0;a.lever=0;a.thrust=0;
    leftAircraft.push_back(std::move(a));
    while(leftAircraft.size()>kLeftAircraft) {
        if(leftAircraft.front().node)leftAircraft.front().node->queueFree();
        leftAircraft.erase(leftAircraft.begin());
    }
}

bool World::Impl::leaveAircraft() {
    if(!piloting)return false;
    const auto& t=*plane.type;
    const double s=std::sin(plane.yaw*rad),c=std::cos(plane.yaw*rad),half=fuselageHalf(t);
    std::optional<glm::dvec2> dry,wet;
    for(double along:{t.length*.3,t.length*.42,0.,-t.length*.25})
        for(double side:{-1.,1.})
            for(double off:{2.,4.}) {
                if(dry)break;
                const double o=(half+off)*side;
                const auto q=onward(plane.lon,plane.lat,s*along+c*o,c*along-s*o);
                if(!tile(q.x,q.y))continue;
                if(deepWater(q.x,q.y)){if(!wet)wet=q;}
                else if(!footBlocked(q.x,q.y))dry=q;
            }
    double x=0,y=0;
    // Over a city the ground beside the fuselage may all be roofs: in the
    // air he can come down further off, in the open.
    const double around=plane.airborne?t.span*.5+80.:t.span*.5+8.;
    if(!dry&&!wet&&freeSpot(plane.lon,plane.lat,around,x,y))dry=glm::dvec2(x,y);
    if(!dry&&!wet) {
        text("stream-status","Cannot get out here — the ground below is not loaded yet.");
        saida::Log::warn("[World aircraft] no loaded ground beside the aircraft at ",plane.lon,", ",plane.lat);
        return false;
    }
    const glm::dvec2 at=dry?*dry:*wet;
    const double from=plane.alt,climb=plane.climb;
    piloting=false;keepAircraft(plane);plane=Aircraft{};
    yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
    lon=at.x;lat=at.y;
    swimTime=0;swimHeading=yaw;swimLean=0;
    alt=dry?height(lon,lat):waterLevel(lon,lat);
    // Above what he lands on, he falls, into the water too; he swims once
    // he is in it (the landing, in the frame's walk).
    const double drop=from-alt;
    const bool falls=drop>.5;
    swimming=!dry&&!falls;
    jumpOffset=falls?drop:0.;
    jumpVelocity=falls?std::clamp(climb,-kFallTerminal,15.):0.;
    player->setEnabled(true);
    player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
    followDistance=std::min(followDistance,kOnFootFollow);
    text("stream-status",falls?"Jump!":swimming?"In the water. F: climb back aboard.":"On foot. F: take the aircraft again.");
    saida::Log::info("[World aircraft] ",falls?"jumped":"stepped down"," at ",lon,", ",lat,
                     falls?", falling "+std::to_string(int(drop))+" m":"",!dry?" into the water":"");
    request(lon,lat);
    return true;
}

void World::Impl::dropLeftAircraft(Aircraft& a,double dt) {
    if(!a.airborne||!tile(a.lon,a.lat))return;
    a.climb=std::max(a.climb-kGravity*dt,-kFallTerminal);
    a.alt+=a.climb*dt;
    double under=onWater(a.lon,a.lat)?waterLevel(a.lon,a.lat):height(a.lon,a.lat);
    under=std::max(under,roofAt(a.lon,a.lat,a.alt+1.,aircraftCollider(a.node)));
    a.pitch*=std::exp(-dt);a.roll*=std::exp(-dt);
    if(a.alt>under)return;
    a.alt=under;a.climb=0;a.airborne=false;a.pitch=0;a.roll=0;
    saida::Log::info("[World aircraft] a ",a.type->name," left in the air came down at ",a.lon,", ",a.lat);
}

void World::Impl::clearAircraft() {
    for(auto& a:leftAircraft)if(a.node)a.node->queueFree();
    leftAircraft.clear();
    if(plane.node)plane.node->queueFree();
    plane=Aircraft{};piloting=false;
}

bool World::Impl::moveAircraft(double dt,double horizontal) {
    if(std::abs(horizontal)<1e-4)return true;
    const double s=std::sin(plane.yaw*rad),c=std::cos(plane.yaw*rad);
    const auto next=onward(plane.lon,plane.lat,s*horizontal*dt,c*horizontal*dt);
    if(!tile(next.x,next.y))stream();
    if(!tile(next.x,next.y)&&!plane.airborne) {
        plane.speed=0;
        text("stream-status","Edge of the loaded area — more data is on its way.");
        return false;
    }
    const double heading=horizontal<0?wrap(plane.yaw+180.):plane.yaw;
    if(aircraftHits(next.x,next.y,plane.alt,heading,true)&&!aircraftHits(plane.lon,plane.lat,plane.alt,heading)) {
        plane.speed=0;
        if(!planeStopped) {
            text("stream-status","Building — the aircraft stops.");
            saida::Log::info("[World aircraft] stopped by a building at ",plane.lon,", ",plane.lat,
                             ", ",plane.alt-planeGround," m up");
        }
        planeStopped=true;
        return false;
    }
    if(!plane.airborne&&!plane.helicopter()&&tile(next.x,next.y)&&onWater(next.x,next.y)&&!onWater(plane.lon,plane.lat)) {
        plane.speed=0;
        text("stream-status","Water's edge — the aircraft stops.");
        return false;
    }
    planeStopped=false;
    plane.lon=next.x;plane.lat=next.y;
    return true;
}

void World::Impl::flyPlane(double dt,double f,double r,double pitchIn) {
    Aircraft& p=plane;const auto& t=*p.type;
    p.lever=std::clamp(p.lever+f*.45*dt,0.,1.);
    p.thrust+=(p.lever-p.thrust)*(1-std::exp(-t.spool*3.*dt));
    const double ratio=p.speed/t.top,drag=t.accel*ratio*std::abs(ratio);
    if(!p.airborne) {
        bool wet=false;
        surfaceUnder(p.lon,p.lat,p.alt+2.,nullptr,&wet);
        const double v=p.speed;
        const double sign=v>0?1.:v<0?-1.:0.;
        double a=t.accel*p.thrust-drag-(wet?1.5:.3)*sign;
        if(f<0&&p.lever<=0.)a=v>.3?-t.brake:(v>-3.?-1.5:0.);
        p.speed=v+a*dt;
        if(sign>0&&p.speed<0&&!(f<0&&p.lever<=0.))p.speed=0;
        if(sign<0&&p.speed>0)p.speed=0;
        const double speed=std::abs(p.speed);
        if(speed>.2)p.yaw=wrap(p.yaw+r*std::min(r1::degrees(speed/t.turnRadius),30.)*dt*(p.speed<0?-1.:1.));
        if(pitchIn>0&&p.speed>=t.rotate)p.pitch=std::min(t.maxPitch*.6,p.pitch+t.pitchRate*dt);
        else p.pitch=std::max(0.,p.pitch-t.pitchRate*dt);
        p.roll-=p.roll*(1-std::exp(-4.*dt));
        if(p.pitch>2.5&&p.speed>=t.rotate) {
            // Off the ground with the climb the rotation already gives,
            // or the next hump of the terrain would set it back down.
            p.airborne=true;p.climb=std::max(1.5,p.speed*std::sin(p.pitch*rad));p.alt+=.2;
            text("stream-status","Take-off.");
            saida::Log::info("[World aircraft] lift-off at ",p.speed," m/s");
        }
        moveAircraft(dt,p.speed);
        if(!p.airborne)p.alt=surfaceUnder(p.lon,p.lat,p.alt+2.);
        return;
    }
    const double bank=r*t.maxBank;
    p.roll+=std::clamp(bank-p.roll,-t.rollRate*dt,t.rollRate*dt);
    if(pitchIn!=0)p.pitch+=pitchIn*t.pitchRate*dt;
    else p.pitch-=std::clamp(p.pitch,-.3*t.pitchRate*dt,.3*t.pitchRate*dt);
    p.pitch=std::clamp(p.pitch,-t.maxPitch,t.maxPitch);
    // Climbing costs speed and diving gives it -- half of what gravity
    // would, which is the arcade in it.
    p.speed=std::max(0.,p.speed+(t.accel*p.thrust-drag-kGravity*std::sin(p.pitch*rad)*.5)*dt);
    // A coordinated turn, half again as quick as a real one.
    const double v=std::max(p.speed,t.stall*.6);
    p.yaw=wrap(p.yaw+r1::degrees(kGravity*std::tan(p.roll*rad)/v)*1.5*dt);
    double vertical=p.speed*std::sin(p.pitch*rad);
    if(p.speed<t.stall) {
        // The wing gives way: the aircraft sinks and its nose drops.
        const double k=(t.stall-p.speed)/t.stall;
        vertical-=14.*k;
        p.pitch-=18.*k*dt;
    }
    p.climb+=(vertical-p.climb)*(1-std::exp(-3.*dt));
    moveAircraft(dt,p.speed*std::cos(p.pitch*rad));
    p.alt+=p.climb*dt;
    bool onRoof=false,wet=false;
    const double ground=surfaceUnder(p.lon,p.lat,p.alt,&onRoof,&wet);
    if(p.alt<=ground) {
        p.alt=ground;p.airborne=false;
        const bool hard=p.climb<-7.||std::abs(p.roll)>25.||p.pitch<-8.;
        if(hard){p.speed=0;p.lever=0;p.thrust=0;}
        text("stream-status",hard?"Hard landing — the aircraft stops."
                             :wet?"Water landing.":onRoof?"Landed on the roof.":"Landing.");
        saida::Log::info("[World aircraft] ",hard?"hard landing":"landed"," at ",p.climb," m/s vertical, ",
                         p.speed," m/s",wet?" on water":onRoof?" on a roof":"");
        p.climb=0;p.roll=0;p.pitch=std::max(0.,p.pitch);
    }
}

void World::Impl::flyHelicopter(double dt,double f,double r,double up) {
    Aircraft& h=plane;const auto& t=*h.type;
    h.rotor=std::min(1.,h.rotor+dt/3.);
    const bool lift=h.rotor>.85;
    double want=lift?t.climb*up:(h.airborne?-3.:0.);
    if(!h.airborne&&want<0)want=0;
    h.climb+=(want-h.climb)*(1-std::exp(-3.*dt));
    const double target=h.airborne?(f>=0?f*t.top:f*t.top*.3):0.;
    h.speed+=std::clamp(target-h.speed,-t.accel*dt,t.accel*dt);
    if(!h.airborne)h.speed*=std::exp(-6.*dt);
    if(lift)h.yaw=wrap(h.yaw+r*t.rollRate*dt*(h.airborne?1.:.35));
    const double pitchTarget=h.airborne?-t.maxPitch*std::clamp(target/t.top,-.4,1.):0.;
    const double rollTarget=h.airborne?r*t.maxBank*std::clamp(.3+std::abs(h.speed)/15.,0.,1.):0.;
    h.pitch+=(pitchTarget-h.pitch)*(1-std::exp(-2.5*dt));
    h.roll+=(rollTarget-h.roll)*(1-std::exp(-2.5*dt));
    moveAircraft(dt,h.speed);
    h.alt+=h.climb*dt;
    bool onRoof=false,wet=false;
    const double ground=surfaceUnder(h.lon,h.lat,h.alt+.5,&onRoof,&wet);
    if(h.alt<=ground) {
        if(h.airborne) {
            text("stream-status",onRoof?"Landed on the roof.":wet?"Landed on the water.":"Landed.");
            saida::Log::info("[World aircraft] helicopter set down",onRoof?" on a roof":wet?" on water":"",
                             " at ",h.lon,", ",h.lat);
        }
        h.alt=ground;h.airborne=false;h.climb=std::max(0.,h.climb);
    } else if(h.alt>ground+.3&&!h.airborne) {
        h.airborne=true;
        text("stream-status","Take-off.");
        saida::Log::info("[World aircraft] helicopter lifted off");
    }
    h.mainSpin=std::fmod(h.mainSpin+h.rotor*h.rotor*28.*dt,kTwoPi);
    h.tailSpin=std::fmod(h.tailSpin+h.rotor*h.rotor*44.*dt,kTwoPi);
}

glm::quat World::Impl::aircraftRotation(const Aircraft& a) const {
    return glm::angleAxis(float(-a.yaw*rad),glm::vec3(0,1,0))
          *glm::angleAxis(float(a.pitch*rad),glm::vec3(1,0,0))
          *glm::angleAxis(float(-a.roll*rad),glm::vec3(0,0,1));
}

void World::Impl::placeAircraft(double dt) {
    auto place=[&](Aircraft& a){
        if(!a.node||!a.type)return;
        const glm::quat q=aircraftRotation(a);
        const glm::vec3 cg=glm::vec3(origin.local(ecef(a.lon,a.lat,a.alt+a.type->cg)));
        a.node->transform().position=cg-q*glm::vec3(0,float(a.type->cg),0);
        a.node->transform().rotation=q;
        const bool down=!a.airborne||a.alt-groundAt(a.lon,a.lat,a.alt)<30.;
        for(auto* g:a.gear)g->setVisible(down);
        for(auto* m:a.mainRotors)m->transform().rotation=glm::angleAxis(float(a.mainSpin),glm::vec3(0,1,0));
        for(auto* m:a.tailRotors)m->transform().rotation=glm::angleAxis(float(a.tailSpin),glm::vec3(1,0,0));
    };
    if(piloting)place(plane);
    for(auto& a:leftAircraft) {
        // A helicopter left behind winds down rather than stopping dead.
        a.rotor=std::max(0.,a.rotor-dt/8.);
        a.mainSpin=std::fmod(a.mainSpin+a.rotor*a.rotor*28.*dt,kTwoPi);
        a.tailSpin=std::fmod(a.tailSpin+a.rotor*a.rotor*44.*dt,kTwoPi);
        dropLeftAircraft(a,dt);
        place(a);
    }
}

} // namespace r1::world
