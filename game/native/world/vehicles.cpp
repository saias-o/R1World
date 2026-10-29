#include "session.hpp"

namespace r1::world {
void World::Impl::updateRadio(double dt) {
    const bool keys[]={held(GLFW_KEY_B),held(GLFW_KEY_N),held(GLFW_KEY_PAGE_UP),held(GLFW_KEY_PAGE_DOWN)};
    bool pressed[4];
    for(int i=0;i<4;++i){pressed[i]=keys[i]&&!radioKeys[i]&&!menu&&!cheatOpen;radioKeys[i]=keys[i];}
    const bool aboard=playing&&driving()&&!pending&&!carSinking;
    const bool previous=held(GLFW_KEY_LEFT_SHIFT)||held(GLFW_KEY_RIGHT_SHIFT);
    radio.update(dt,lon,lat,aboard,pressed[0],pressed[1]&&!previous,pressed[1]&&previous,int(pressed[2])-int(pressed[3]));
    style("vehicle-radio","display",aboard?"block":"none");
    if(aboard)text("vehicle-radio",radio.label(lon,lat));
}

glm::quat World::Impl::carRotation() {
    const auto& spec=vehicleSpec(*car);
    const double halfLength=spec.wheelbase*.5,halfWidth=spec.width*.43;
    auto sample=[&](double east,double north){
        auto q=onward(carLon,carLat,east,north);return groundAt(q.x,q.y,carAlt);
    };
    double s=sin(carYaw*rad),c=cos(carYaw*rad);
    double ahead=sample(s*halfLength,c*halfLength),behind=sample(-s*halfLength,-c*halfLength);
    double right=sample(c*halfWidth,-s*halfWidth),left=sample(-c*halfWidth,s*halfWidth);
    auto yawQ=glm::angleAxis(float(-carYaw*rad),glm::vec3(0,1,0));
    auto pitchQ=glm::angleAxis(float(std::atan2(ahead-behind,2*halfLength)),glm::vec3(1,0,0));
    auto rollQ=glm::angleAxis(float(std::atan2(right-left,2*halfWidth)),glm::vec3(0,0,1));
    return yawQ*pitchQ*rollQ;
}

void World::Impl::collectWheels(saida::Node& root) {
    frontWheels.clear();rearWheels.clear();
    std::function<void(saida::Node&)> collect=[&](saida::Node& n){
        if(n.name().rfind("wheel-",0)==0) {
            (n.name().find("front")!=std::string::npos?frontWheels:rearWheels).push_back(&n);
            return;
        }
        for(auto& child:n.children())collect(*child);
    };
    collect(root);
    const size_t levels=(root.findByPath("Far")||root.findByPath("Vehicle collider/Far"))?2:1;
    // The player's scene car is collected before buildTrafficPrototype()
    // populates `fleet`; do not ask vehicleSpec() for its fallback then.
    // Traffic instances have their manifest name on the root node.
    const bool bike=root.name()=="vehicle-moto_city"||root.name()=="vehicle-moto_cross";
    const size_t expected=levels*(bike?1:2);
    if(frontWheels.size()!=expected||rearWheels.size()!=expected)
        saida::Log::warn("[World car] this car has ",frontWheels.size()," front and ",
                         rearWheels.size()," rear wheels named wheel-*; they will not turn");
}

void World::Impl::placeCar() {
    // Abandoned cars are written in the origin's frame like everything else
    // the player owns, so they have to be rewritten when the origin moves.
    // There are at most six of them.
    for(auto& entry:parked) {
        if(r1::VehicleRuntime::physical(*entry.node)) {
            r1::VehicleRuntime::synchronize(*entry.node);
            const auto p=entry.node->transform().position;
            const auto worldPoint=origin.origin+origin.basis*glm::dvec3(p);
            const auto geo=r1::ecefToGeodetic(worldPoint.x,worldPoint.y,worldPoint.z);
            entry.lon=geo.x;entry.lat=geo.y;entry.alt=geo.z-r1::VehicleRuntime::kBodyClearance;
            continue;
        }
        entry.node->transform().position=glm::vec3(origin.local(ecef(entry.lon,entry.lat,entry.alt+.06)));
        entry.node->transform().rotation=glm::angleAxis(float(-entry.yaw*rad),glm::vec3(0,1,0));
    }
    if(!carParked||!car)return;
    if(r1::VehicleRuntime::physical(*car))captureCarPose();
    else {
        car->transform().position=glm::vec3(origin.local(ecef(carLon,carLat,carAlt+.06)));
        car->transform().rotation=carSinking?glm::angleAxis(float(-carYaw*rad),glm::vec3(0,1,0)):carRotation();
    }
    // Wheels turn because a car whose wheels do not turn is a car sliding
    // on ice, and this one is visible from three metres behind it.
    const bool bike=r1::motorcycle(vehicleSpec(*car).name);
    auto spin=glm::angleAxis(float(bike?wheelSpin:-wheelSpin),glm::vec3(1,0,0));
    const auto* drive=r1::VehicleRuntime::driver(*car);
    const double steering=drive?drive->steeringAngle*steerShown/(1.+drive->speedSteering*carSpeed*carSpeed):kCarSteerAngle*steerShown;
    auto lock=glm::angleAxis(float(-steering*rad),glm::vec3(0,1,0));
    for(auto* w:rearWheels)w->transform().rotation=spin;
    for(auto* w:frontWheels)w->transform().rotation=lock*spin;
}

bool World::Impl::freeSpot(double x0,double y0,double radius,double& outLon,double& outLat,
                  double standing) {
    auto free=[&](double x,double y){
        if(!tile(x,y)||blocked(x,y,standing))return false;
        return std::isnan(standing)||std::abs(height(x,y,standing)-standing)<kStepUp;
    };
    if(free(x0,y0)){outLon=x0;outLat=y0;return true;}
    for(int i=1;i<=120;++i) {
        double a=i*2.39996323,d=radius*std::sqrt(double(i)/120.);
        auto q=advance(x0,y0,d*cos(a),d*sin(a));
        if(free(q.x,q.y)){outLon=q.x;outLat=q.y;return true;}
    }
    return false;
}

double World::Impl::clearAhead(double x,double y,double level,double heading,double step,double limit) {
    double clear=0;
    for(double d=step;d<=limit;d+=step) {
        auto q=onward(x,y,sin(heading*rad)*d,cos(heading*rad)*d);
        if(!tile(q.x,q.y)||blocked(q.x,q.y,level))break;
        level=height(q.x,q.y,level);
        clear=d;
    }
    return clear;
}

void World::Impl::reparkIfCovered(const std::string& key) {
    if(!carParked||driving()||!car)return;
    parkingCar=true;
    const bool covered=blocked(carLon,carLat,carAlt);
    double x=carLon,y=carLat;
    const bool found=covered&&freeSpot(carLon,carLat,20.,x,y,carAlt);
    parkingCar=false;
    if(!covered)return;
    if(!found){saida::Log::warn("[World car] ",key," covered the parked car and no free spot is within 20 m");return;}
    saida::Log::info("[World car] ",key," streamed in over the parked car; moved to ",x,", ",y);
    r1::VehicleRuntime::deactivate(*car);
    carLon=x;carLat=y;carAlt=groundAt(x,y,carAlt);placeCar();
}

void World::Impl::parkCar() {
    carParked=false;carSinking=false;carSinkDepth=0;
    carSpeed=0;wheelSpin=0;steerShown=0;
    // Half a planet away from where they were left, so they are not kept.
    clearAbandonedCars();
    if(!car)return;
    r1::VehicleRuntime::reset(*car);
    if(auto* drive=r1::VehicleRuntime::driver(*car))drive->parked=true;
    car->setEnabled(false);
    if(swimming())return;
    // No road has ever reached the pack ice: nobody drives to the Pole.
    if(onSeaIce(lon,lat)) {
        saida::Log::info("[World car] on the sea ice: the player arrives on foot, with no car");
        return;
    }
    // Three metres to his right, which is where a car that dropped him off
    // would be, and a spiral out from there when that spot is a wall.
    auto beside=onward(lon,lat,cos(yaw*rad)*3.,-sin(yaw*rad)*3.);
    double x,y;
    // A car's spot is judged as a car: a shop a walker may enter is a
    // wall to it, or the car would be parked inside the store.
    parkingCar=true;
    const bool found=freeSpot(beside.x,beside.y,14.,x,y,alt);
    parkingCar=false;
    if(!found) {
        // Loud, and said to the player too. A car that silently is not
        // there looks exactly like a car that failed to load, which is the
        // shape of failure this file has already been bitten by three
        // times (see `refused`, and the spawn refusal below).
        saida::Log::warn("[World car] no free kerb within 14 m of ",lon,", ",lat,
                         "; the player spawns on foot with no car");
        text("stream-status","No room for the car here — starting on foot.");
        return;
    }
    carLon=x;carLat=y;carAlt=groundAt(x,y,alt);carYaw=yaw;carParked=true;
    car->setEnabled(true);placeCar();
    saida::Log::info("[World car] parked at ",carLon,", ",carLat);
}

void World::Impl::abandonCurrentCar() {
    if(!car||!carParked)return;
    keepParked({car,carLon,carLat,carAlt,carYaw});
    car=nullptr;carParked=false;carSinking=false;carSinkDepth=0;
}

void World::Impl::keepParked(const Parked& entry) {
    parked.push_back(entry);
    while(parked.size()>kAbandonedCars) {
        // The scene's own car is in this list like any other once it has
        // been left; nothing here is special-cased, so nothing here can
        // disagree with itself about which car is the player's.
        parked.front().node->queueFree();
        parked.erase(parked.begin());
    }
}

void World::Impl::clearAbandonedCars() {
    for(auto& entry:parked)entry.node->queueFree();
    parked.clear();
}

glm::dvec2 World::Impl::geodeticOf(const Loaded& tile,const glm::vec3& local) const {
    const glm::dvec3 here=tile.frame.local(ecef(lon,lat,alt));
    return advance(lon,lat,double(local.x)-here.x,-(double(local.z)-here.z));
}

bool World::Impl::nearestCar(Reach& out,double& distance) {
    distance=kCarReach;
    bool found=false;
    const glm::dvec3 me=ecef(lon,lat,alt);
    if(carParked&&car&&!carSinking) {
        const double d=glm::length(me-ecef(carLon,carLat,carAlt));
        if(d<distance){distance=d;out={car,nullptr,0,0};found=true;}
    }
    for(size_t i=0;i<parked.size();++i) {
        const auto& entry=parked[i];
        const double d=glm::length(me-ecef(entry.lon,entry.lat,entry.alt));
        if(d<distance){distance=d;out={entry.node,nullptr,i,1};found=true;}
    }
    for(auto& [key,tile]:loaded) {
        for(size_t i=0;i<tile.cars.size();++i) {
            if(!tile.cars[i]||!trafficSlotLive(tile,i))continue;
            const auto here=geodeticOf(tile,tile.cars[i]->transform().position);
            const double d=glm::length(me-ecef(here.x,here.y,alt));
            if(d<distance){distance=d;out={tile.cars[i],&tile,i,2};found=true;}
        }
    }
    return found;
}

bool World::Impl::takeTrafficCar(const Reach& target) {
    Loaded& tile=*target.tile;
    r1::VehicleRuntime::synchronize(*target.node);
    target.node->updateTransforms(tile.node->worldTransform(),true);
    const glm::mat4 previousWorld=target.node->worldTransform();
    const glm::vec3 local=target.node->transform().position;
    const auto here=geodeticOf(tile,local);
    auto owned=tile.node->detachChild(target.node);
    if(!owned) {
        saida::Log::warn("[World car] the traffic car could not be detached from ",
                         tile.data.value("key",std::string()));
        return false;
    }
    reportCrime(r1::police::Crime::Theft,nodePosition(*target.node));
    tile.flow.retire(target.agent);
    tile.cars[target.agent]=nullptr;
    abandonCurrentCar();
    car=engine.sceneTree().world().addChild(std::move(owned));
    removeTrafficRider(*car);
    car->transform().position=glm::vec3(previousWorld[3]);
    car->transform().rotation=glm::quat_cast(glm::mat3(previousWorld));
    carParked=true;
    carLon=here.x;carLat=here.y;carAlt=groundAt(carLon,carLat,alt);
    // Keep its heading: the car was driving somewhere, and snapping it to
    // the player's facing is a jolt the eye catches at two metres.
    const glm::vec3 forward=car->transform().rotation*glm::vec3(0,0,-1);
    carYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
    carSpeed=0;wheelSpin=0;steerShown=0;
    collectWheels(*car);
    saida::Log::info("[World car] took a traffic car at ",carLon,", ",carLat);
    return true;
}

void World::Impl::takeAbandonedCar(const Reach& target) {
    const Parked entry=parked[target.agent];
    parked.erase(parked.begin()+long(target.agent));
    abandonCurrentCar();
    car=entry.node;carParked=true;
    carLon=entry.lon;carLat=entry.lat;carAlt=entry.alt;carYaw=entry.yaw;
    carSpeed=0;wheelSpin=0;steerShown=0;
    collectWheels(*car);
}

double World::Impl::carDistance() const {
    return glm::length(ecef(lon,lat,alt)-ecef(carLon,carLat,carAlt));
}

void World::Impl::removeTrafficRider(saida::Node& vehicle) {
    auto* body=r1::VehicleRuntime::body(vehicle);
    auto* owner=body?static_cast<saida::Node*>(body):&vehicle;
    if(auto* rider=owner->findByPath("Rider"))owner->removeChild(rider);
}

bool World::Impl::ridingBike() const {return driving()&&car&&r1::motorcycle(vehicleSpec(*car).name);}

glm::vec3 World::Impl::motorcycleContact(saida::Node& rider,const std::string& vehicle,int hand,bool grip) {
    const bool cross=vehicle=="moto_cross";
    // Contact points measured on the pinned models, before the shared resize.
    const float halfBar=cross?.361f:.285f;
    const glm::vec3 point=grip?glm::vec3(hand?halfBar:-halfBar,cross?1.20f:1.116f,cross?-.344f:-.390f)
        :glm::vec3(hand?.24f:-.24f,cross?.565f:.44f,cross?.338f:.14f);
    return glm::vec3(glm::inverse(rider.localMatrix())*glm::vec4(point*float(kVehicleScale),1.f));
}

void World::Impl::bikeHandlePose() {
    if(!playerBikeRider)return;
    const auto& kind=vehicleSpec(*car).name;
    playerBikeLean->lookAt({0.f,-.4f,1.2f});
    playerBikeHands->hold(true);
    if(!aiming)playerBikeLook->lookAt({0.f,1.5f,100.f});
    for(int h=0;h<2;++h) {
        const auto foot=motorcycleContact(*playerBody,kind,h,false);
        playerBikeLegs[h]->reach(foot);playerBikeLegs[h]->setWeightNow(1.f);
        playerBikeLegs[h]->setTipRotation(r1::rocketboxFootRotation(*animators.front()->rig(),h==1));
        playerBikeLegs[h]->setPole({foot.x*2.f,foot.y+.35f,foot.z+.45f});
        if(h==1&&aiming&&arsenal.held()==r1::combat::Weapon::Pistol)continue;
        auto* arm=playerArms[h];const auto grip=motorcycleContact(*playerBody,kind,h,true);
        const auto palm=r1::rocketboxBarGrip(*animators.front()->rig(),h==1);
        const auto wrist=grip-palm.rotation*palm.position;
        if(h==0)playerBikeAim->keepLeftHandAt(wrist);
        arm->setTipRotation(palm.rotation);arm->reach(wrist);arm->setWeightNow(1.f);
        arm->setPole({grip.x*1.7f,grip.y-.15f,grip.z-.25f});
    }
}

void World::Impl::mountPlayerOnBike() {
    if(!car||!playerBody||animators.empty()||!r1::motorcycle(vehicleSpec(*car).name))return;
    HumanKind* seated=humanKind(0);
    auto* body=r1::VehicleRuntime::body(*car);
    if(!seated||!seated->rig||!body)return;
    removeTrafficRider(*car);
    auto owned=playerBody->parent()->detachChild(playerBody);
    const bool motocross=vehicleSpec(*car).name=="moto_cross";
    playerBikeRider=body->addChild(std::move(owned));
    playerBody->transform().position=r1::rocketboxRidingSeat(*animators.front()->rig(),motocross,
        float(kVehicleScale),float(humanScale),float(seated->seatDrop));
    playerBody->transform().rotation=glm::quat(0,0,1,0); // Rig +Z faces vehicle -Z.
    playerBody->transform().scale=glm::vec3(float(humanScale));
    auto* animator=animators.front();
    for(const auto& [name,clip]:seated->clips)if(name=="sit")animator->addClip("bike-sit",clip);
    animator->play("bike-sit",true,0.f);animator->setPoseRate(0.f);
    r1::holdRocketboxRidingBase(*animator);
    showHeldPistol();
    bikeHandlePose();
    saida::Log::info("[World motorcycle] player body attached to solver on ",vehicleSpec(*car).name);
}

void World::Impl::dismountPlayerFromBike() {
    if(!playerBikeRider)return;
    if(aiming)setAiming(false);
    auto owned=playerBikeRider->parent()->detachChild(playerBikeRider);
    player->addChild(std::move(owned));
    playerBody->transform()=playerBodyRest;
    playerBikeRider=nullptr;
    showHeldPistol();
    for(auto* arm:playerArms)if(arm){arm->release();arm->clearTipRotation();}
    for(auto* leg:playerBikeLegs)if(leg)leg->release();
    if(playerBikeLean)playerBikeLean->release();
    if(playerBikeLook)playerBikeLook->release();
    if(playerBikeAim)playerBikeAim->release();
    if(playerBikeHands)playerBikeHands->hold(false);
    for(auto* animator:animators)animator->play("idle",true,0.f);
}

bool World::Impl::enterCar() {
    if(driving()||swimming())return false;
    Reach target;double distance=0;
    if(!nearestCar(target,distance))return false;
    if(target.kind==2){if(!takeTrafficCar(target))return false;}
    else if(target.kind==1)takeAbandonedCar(target);
    if(!carParked||!car)return false;
    if(aiming)setAiming(false);
    if(auto* physics=engine.sceneTree().world().physics())r1::VehicleRuntime::activate(*car,*physics,glm::vec3(0));
    captureCarPose();
    if(auto* drive=r1::VehicleRuntime::driver(*car))drive->parked=false;
    // The car is where the player is now, and the walk's own lon/lat drive
    // it from here: one position, one streamer, one sun. Nothing downstream
    // has to learn that the player is sitting down.
    lon=carLon;lat=carLat;alt=carAlt;yaw=carYaw;
    transitionTravel(TravelMode::Car,false);
    text("stream-status",ridingBike()?"Riding. F: get off · Space: rear brake · 2: pistol · Right click: aim.":"Driving. F: get out · Space: handbrake.");
    saida::Log::info("[World car] entered at ",carLon,", ",carLat);
    return true;
}

bool World::Impl::leaveCar() {
    if(!driving())return false;
    captureCarPose();
    const auto& spec=vehicleSpec(*car);
    const r1::VehicleDimensions size{float(spec.length),float(spec.width),float(spec.height),float(spec.wheelbase),float(spec.wheelRadius)};
    glm::vec3 exitPosition;bool found=false;
    for(float turn:{0.f,180.f,90.f,270.f}) {
        const double bearing=(carYaw+turn)*rad;
        const auto offset=r1::VehicleRuntime::exitOffset(size,car->transform().rotation,
            {float(cos(bearing)),0,float(-sin(bearing))});
        const auto candidate=car->transform().position+offset;
        const auto point=origin.origin+origin.basis*glm::dvec3(candidate);
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        if(tile(geo.x,geo.y)&&!blocked(geo.x,geo.y,geo.z)){exitPosition=candidate;found=true;break;}
    }
    if(!found)exitPosition=car->transform().position+
        r1::VehicleRuntime::exitOffset(size,car->transform().rotation,{0,1,0});
    const auto point=origin.origin+origin.basis*glm::dvec3(exitPosition);
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    const double exitSpeed=carSpeed;
    float verticalVelocity=0;
    if(auto* body=r1::VehicleRuntime::body(*car);body&&body->physicsWorld())
        verticalVelocity=body->physicsWorld()->linearVelocity(body->bodyId()).y;
    carSpeed=0;steerShown=0;
    if(auto* drive=r1::VehicleRuntime::driver(*car))drive->parked=true;
    // Step out looking where you were looking, not snapped back to the
    // bonnet: the offset becomes the walk's own yaw.
    lon=geo.x;lat=geo.y;alt=groundAt(lon,lat,carAlt);
    jumpOffset=std::max(0.,geo.z-alt);jumpVelocity=std::clamp(double(verticalVelocity),-kFallTerminal,15.);
    transitionTravel(TravelMode::Foot);
    text("stream-status","On foot. F: get back in the vehicle.");
    saida::Log::info("[World car] left at ",lon,", ",lat," speed=",exitSpeed," m/s, fall=",jumpOffset," m");
    request(lon,lat);
    return true;
}

void World::Impl::sinkCar(double x,double y) {
    if(car)if(auto* body=r1::VehicleRuntime::body(*car)) {
        r1::VehicleRuntime::synchronize(*car);body->kinematic=true;body->markDirty();
    }
    carWaterLevel=waterLevel(x,y);carSinkDepth=0;carSinking=true;
    carLon=x;carLat=y;carAlt=carWaterLevel;carSpeed=0;steerShown=0;
    // The driver swims out only when the water is over half his height.
    const bool wet=deepWater(x,y);
    lon=x;lat=y;alt=wet?carWaterLevel:height(x,y);yaw=wrap(carYaw+lookYaw);swimHeading=yaw;
    lookYaw=0;lookIdle=0;jumpOffset=jumpVelocity=0;
    followDistance=std::min(followDistance,kOnFootFollow);
    transitionTravel(wet?TravelMode::Swim:TravelMode::Foot,false);
    text("stream-status",swimming()?"The vehicle is sinking — swim for the shore.":"The vehicle is bogged down — you can stand.");
    saida::Log::info("[World car] sank at ",lon,", ",lat);
    request(lon,lat);
}

void World::Impl::updateSinkingCar(double dt) {
    if(!carSinking||!carParked||!car)return;
    carSinkDepth=std::min(kCarSinkDepth,carSinkDepth+kCarSinkRate*dt);
    carAlt=carWaterLevel-carSinkDepth;
}

void World::Impl::captureCarPose() {
    r1::VehicleRuntime::synchronize(*car);
    const glm::dvec3 point=origin.origin+origin.basis*glm::dvec3(car->transform().position);
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    carLon=geo.x;carLat=geo.y;carAlt=geo.z-r1::VehicleRuntime::kBodyClearance;
    const glm::vec3 forward=car->transform().rotation*glm::vec3(0,0,-1);
    carYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
    if(auto* body=r1::VehicleRuntime::body(*car);body&&body->physicsWorld())
        carSpeed=glm::dot(body->physicsWorld()->linearVelocity(body->bodyId()),forward);
    if(driving()){lon=carLon;lat=carLat;alt=carAlt;yaw=carYaw;}
}

bool World::Impl::needsAfterSimulation() const {
    return driving()&&car&&!pending&&playing&&(worldCapture.pngPath.empty()||!captureView.set);
}

void World::Impl::afterSimulation(float) {
    if(!needsAfterSimulation())return;
    auto* body=r1::VehicleRuntime::body(*car);
    if(!body||!body->physicsWorld()||body->kinematic)return;
    glm::vec3 position;glm::quat rotation;
    body->physicsWorld()->getBodyTransform(body->bodyId(),position,rotation);
    const auto forward=rotation*glm::vec3(0,0,-1);
    const double currentYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
    const auto turn=glm::angleAxis(float(-wrap(currentYaw-carYaw)*rad),glm::vec3(0,1,0));
    // Input/camera placement precedes Jolt. Render the camera against the
    // same completed solver pose as the car, including multi-step frames.
    camera->transform().position=position+turn*(camera->transform().position-car->transform().position);
    camera->transform().rotation=glm::normalize(turn*camera->transform().rotation);
}

void World::Impl::driveCar(double dt,double throttle,double steerInput,bool handbrake) {
    if(!car)return;
    captureCarPose();
    if(onWater(lon,lat,alt)){sinkCar(lon,lat);return;}
    auto* drive=r1::VehicleRuntime::driver(*car);if(!drive)return;
    constexpr double kReverseBrakeThreshold=.2;
    constexpr double kTerrainGuardMargin=8.;
    const double stoppingDistance=carSpeed*carSpeed/(2.*std::max(1.f,drive->braking))+kTerrainGuardMargin;
    const double guardDirection=std::abs(carSpeed)>kReverseBrakeThreshold?carSpeed:throttle;
    const double guardDistance=std::copysign(stoppingDistance,guardDirection);
    const auto ahead=onward(lon,lat,sin(carYaw*rad)*guardDistance,cos(carYaw*rad)*guardDistance);
    if(!tile(ahead.x,ahead.y))stream();
    float brake=0;
    if(!tile(ahead.x,ahead.y)) {
        throttle=0;brake=1;
        if(carStop!="terrain")saida::Log::info("[World car] braking before unloaded terrain at ",ahead.x,", ",ahead.y);
        carStop="terrain";
        text("stream-status","Edge of the loaded area — more data is on its way.");
    } else {
        carStop.clear();
        if(throttle*carSpeed<0&&std::abs(carSpeed)>kReverseBrakeThreshold) { throttle=0;brake=1; }
    }
    drive->command(float(throttle),brake,float(steerInput),handbrake);
    steerShown=drive->steering();
    wheelSpin+=carSpeed/vehicleSpec(*car).wheelRadius*dt;
    request(lon,lat);
}

} // namespace r1::world
