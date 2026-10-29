#include "session.hpp"
#include "photo_detection.hpp"

namespace r1::world {

double World::Impl::updatePlayer(float delta, bool sampleMouse) {
    auto mouse=saida::Input::mouseDelta();
    if(!sampleMouse)mouse={0,0};
    if(photoMode) {
        const double sensitivity=.12/photoZoom(sceneFov,camera->fovDegrees);
        yaw=wrap(yaw+mouse.x*sensitivity);pitch=std::clamp(pitch-mouse.y*sensitivity,-80.,80.);
        updateCombat(std::min(double(delta),.1));return delta;
    }
    // At the wheel the mouse does not steer -- the car does -- so it turns
    // the head instead: a free yaw around the car, which is what you want
    // at a junction and what the first drive was missing entirely. It
    // recentres behind the car once the mouse stops and the car is rolling,
    // so the default view is still the road ahead.
    if(dropFlying()) {
        // In the air the mouse looks round him while the autopilot flies
        // him; once he steers it turns him, as on foot. Not before the
        // world shows.
        if(drop.clock.seconds>=r1::arrival::kZoomSeconds) {
            if(dropSteered())yaw=wrap(yaw+mouse.x*.12);else drop.lookYaw=wrap(drop.lookYaw+mouse.x*.12);
            drop.lookPitch=std::clamp(drop.lookPitch-mouse.y*.12,-35.,35.);
        }
    } else if(driving()||sailing()||piloting()) {
        if(std::abs(mouse.x)>1e-4){lookYaw=wrap(lookYaw+mouse.x*.12);lookIdle=0;}
        else lookIdle+=delta;
        if(!aiming&&lookIdle>.6&&std::abs(driving()?carSpeed:sailing()?boat.speed:plane.speed)>2.)
            lookYaw-=lookYaw*(1-std::exp(-2.2*double(delta)));
    } else yaw+=mouse.x*.12;
    if(!dropFlying())pitch=std::clamp(pitch-mouse.y*.12,-65.,25.);
    double f=(held(GLFW_KEY_W)||held(GLFW_KEY_Z)?1.:0.)-(held(GLFW_KEY_S)?1.:0.);
    double r=(held(GLFW_KEY_D)?1.:0.)-(held(GLFW_KEY_A)||held(GLFW_KEY_Q)?1.:0.);
    if(smokeDive&&dropFlying())smokeDiveKeys(f,r);
    if(smokeStarted&&worldCapture.pngPath.empty()){
        if(!perfSmoke.empty()&&!perfReady) {
            // Measure motion after the initial import, with normal real
            // time and presentation. Later streaming remains included.
            const bool ready=engine.resources().assetLoadsSettled()&&std::all_of(ring.begin(),ring.end(),[&](Tile t){
                const auto i=loaded.find(t.key());if(i==loaded.end())return false;
                const auto& resident=i->second;
                return resident.geography&&!resident.data.value("provisional",false)&&buildingsReady(resident)
                    &&resident.nextPart>=std::any_cast<const PreparedTile&>(resident.served->prepared).parts.size()
                    &&resident.nextProp==resident.props.size();
            });
            perfReadyWait=ready?perfReadyWait+delta:0.;
            if(perfReadyWait>=2.){perfReady=true;saida::Log::info("[World perf] warm ",perfSmoke," starts, preset=",toasterMode?"toaster":"standard");}
        }
        if(perfSmoke.empty()||perfReady){f=1;r=0;smokeWalk+=double(delta);}
        else f=r=0;
        if(std::getenv("R1WORLD_TEST_VEHICLE_ESCAPE"))f=r=0;
        if(qualitySwitchSmoke&&perfReady&&qualitySwitches<24) {
            qualitySwitchWait+=delta;
            if(qualitySwitchWait>=2.) {
                qualitySwitchWait=0;
                setToasterMode(!toasterMode);
                ++qualitySwitches;
                saida::Log::info("[World quality test] switch=",qualitySwitches," private_mb=",int(privateMegabytes()));
            }
        }
    }
    if(wadeTo&&smokeStarted) {
        const auto to=origin.local(ecef(wadeTo->x,wadeTo->y,alt))-origin.local(ecef(lon,lat,alt));
        yaw=std::atan2(to.x,-to.z)/rad;
    }
    if(idleSmoke&&smokeStarted&&smokeWalk>1.) {
        f=r=0;
        if(!idleSmokeStopped){idleSmokeStopped=true;idleSmokeStart=ecef(lon,lat,alt);}
    }
    if(playerDown){f=r=0;}  // on the ground, he does not walk
    if(retailTest())f=r=0;
    if(performanceSmoke){f=r=0;smokeStarted=false;}
    if(trafficValidation_){f=r=0;smokeStarted=false;}
    if(policeSmoke||rocketSmoke||(smoke&&std::getenv("R1WORLD_PHOTO_SMOKE"))){f=r=0;smokeStarted=false;}
    if(smokeApproach) {
        auto to=origin.local(ecef(carLon,carLat,carAlt))-origin.local(ecef(lon,lat,alt));
        yaw=std::atan2(to.x,-to.z)/rad;f=1;r=0;
        smokeApproachTime+=double(delta);
    }
    double length=std::hypot(f,r),dt=double(delta);
    glm::vec3 gestureStep(0);
#ifdef SAIDA_ENABLE_XR
    if(vr) {
        const float turn=vrGestures.consumeTurnDegrees();
        if(turn!=0) {
            yaw=wrap(yaw+turn);
            player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        }
        gestureStep=vrGestures.advance(float(dt),length>0);
    }
#endif
    // One door key for both directions, and the refusals are inside
    // enterCar/leaveCar so that every one of them says something.
    bool doorKey=held(GLFW_KEY_F);
    if(doorKey&&!wasEnterKey&&!playerDown&&!dropFlying()) {
        if(driving())leaveCar();
        else if(sailing()){if(hopAboard()==HopResult::NoTarget)leaveBoat();}
        else if(piloting())leaveAircraft();
        else if(swimming()){
            if(!enterBoat()&&!enterAircraft())text("stream-status","No boat in reach — swim to one.");
        }
        else {
            // The nearest of a car, a boat and an aircraft: one key for every vehicle.
            Reach carReach;BoatReach boatReach;PlaneReach planeReach;double carAway=0,boatAway=0,planeAway=0;
            const bool car=nearestCar(carReach,carAway),vessel=nearestBoat(boatReach,boatAway);
            const bool aircraft=nearestAircraft(planeReach,planeAway);
            if(aircraft&&(!car||planeAway<carAway)&&(!vessel||planeAway<boatAway))enterAircraft();
            else if(vessel&&(!car||boatAway<carAway))enterBoat();
            else if(!enterCar()) {
                // Pressing the door key from across the street is a question,
                // and it gets an answer rather than silence.
                if(carParked||!parked.empty()||trafficLive()>0)
                    text("stream-status",car
                         ?"Door blocked."
                         :"No vehicle in reach — get closer.");
            }
        }
    }
    wasEnterKey=doorKey;
    bool moving=false;
    footEast=footNorth=0;
    stagger=std::max(0.,stagger-dt);
    // His feet are a body while he is: not at the wheel, at the helm or
    // at the controls, where people meet the vehicle instead.
    // Down, his body is the ragdoll's and his feet leave the physics.
    if(feet->enabled()!=player->enabled())feet->setEnabled(player->enabled());
    if(playerDown) {
        moving=false;
    } else if(dropFlying()) {
        flyDrop(dt,f,r);
        moving=true;
    } else if(driving()) {
        if(smokeDriving){f=smokeBrake?-1.:1.;r=0;}
        driveCar(dt,f,r,held(GLFW_KEY_SPACE)&&!smokeDriving);
        moving=std::abs(carSpeed)>.2;
    } else if(sailing()) {
        if(smokeSailing){f=smokeSailBrake?-1.:1.;r=0;}
        sailBoat(dt,f,r);
        moving=std::abs(boat.speed)>.2;
    } else if(piloting()) {
        double up=(held(GLFW_KEY_SPACE)||held(GLFW_KEY_UP)?1.:0.)
                 -(held(GLFW_KEY_LEFT_SHIFT)||held(GLFW_KEY_LEFT_CONTROL)||held(GLFW_KEY_DOWN)?1.:0.);
        if(smokeFly){f=smokeF;r=smokeR;up=smokeUp;}
        if(plane.helicopter())flyHelicopter(dt,f,r,up);else flyPlane(dt,f,r,up);
        lon=plane.lon;lat=plane.lat;alt=plane.alt;yaw=plane.yaw;
        moving=std::abs(plane.speed)>.2||plane.airborne;
        request(lon,lat);
    } else if(swimming()) {
        swimTime+=dt;
        if(smokeSwimming){f=1.;r=0.;length=1.;}
        if(smokeWaterSpawn){f=1.;r=0.;length=1.;smokeWalk+=dt;}
        if(length>0) {
            const double speed=held(GLFW_KEY_LEFT_SHIFT)?2.2:1.5;
            const double east=(sin(yaw*rad)*f+cos(yaw*rad)*r)/length*speed*dt;
            const double north=(cos(yaw*rad)*f-sin(yaw*rad)*r)/length*speed*dt;
            const auto next=onward(lon,lat,east,north);
            if(tile(next.x,next.y)) {
                if(deepWater(next.x,next.y)) {
                    lon=next.x;lat=next.y;alt=waterLevel(lon,lat);moving=true;
                    swimHeading=wrap(std::atan2(east,north)/rad);
                } else if(!obstructed(next.x,next.y,alt)) {
                    const bool wading=onWater(next.x,next.y);
                    lon=next.x;lat=next.y;alt=height(lon,lat,alt);transitionTravel(TravelMode::Foot);moving=true;
                    text("stream-status",wading?"You can stand.":"Ashore.");
                    if(wading)saida::Log::info("[World swim] standing at ",lon,", ",lat," in ",waterLevel(lon,lat)-alt," m of water");
                    else saida::Log::info("[World swim] ashore at ",lon,", ",lat);
                }
                if(moving)swimHeading=wrap(std::atan2(east,north)/rad);
            } else text("stream-status","More data is on its way… swimming held at the edge of the area.");
        }
        if(moving)request(lon,lat);
    } else if((length>0||glm::length(gestureStep)>0)&&jumpOffset<3.) {  // falling from higher, he cannot steer
        double speed=held(GLFW_KEY_LEFT_SHIFT)&&!aiming?7.:2.8;
        // A stride broken by running into someone (World::Impl::staggerPlayer).
        if(stagger>0)speed*=.35;
        double east=length>0?(sin(yaw*rad)*f+cos(yaw*rad)*r)/length*speed*dt:gestureStep.x;
        double north=length>0?(cos(yaw*rad)*f-sin(yaw*rad)*r)/length*speed*dt:-gestureStep.z;
        footEast=east/dt;footNorth=north/dt;
        // People are bodies: he stops at them and slides round them.
        auto next=moveFeet(east,north,dt);
        if(tile(next.x,next.y)) {
            // What his feet rest on there: the ground, a deck, or whatever
            // the character solver stood him on.
            const double support=std::max(height(next.x,next.y,alt),next.z);
            if(deepWater(next.x,next.y,support)) {
                lon=next.x;lat=next.y;alt=waterLevel(lon,lat);
                transitionTravel(TravelMode::Swim);
                swimHeading=wrap(std::atan2(east,north)/rad);
                jumpOffset=jumpVelocity=0;moving=true;
                text("stream-status","In the water — swim for the shore.");
                const auto bed=waterBed(lon,lat);
                if(bed)saida::Log::info("[World swim] entered water at ",lon,", ",lat," depth=",waterLevel(lon,lat)-*bed," m");
                else saida::Log::info("[World swim] entered water at ",lon,", ",lat," (no known bed: deep)");
            } else {
                lon=next.x;lat=next.y;alt=support;moving=true;
                auto facing=glm::angleAxis(float(-std::atan2(east,north)),glm::vec3(0,1,0));
                player->transform().rotation=glm::slerp(player->transform().rotation,facing,float(1-std::exp(-16*dt)));
            }
        }else {
#ifdef SAIDA_ENABLE_XR
            if(vr)vrGestures.rejectStep();
#endif
            text("stream-status","More data is on its way… movement held at the edge of the area.");
        }
        request(lon,lat);
    } else if(jumpOffset<=0) {
        // Standing still, he is still a body: whoever walks into him
        // meets his feet, and a shoulder moves him a little.
        const auto nudged=moveFeet(0,0,dt);
        if(tile(nudged.x,nudged.y)) {
            const double support=std::max(height(nudged.x,nudged.y,alt),nudged.z);
            if(!deepWater(nudged.x,nudged.y,support)){lon=nudged.x;lat=nudged.y;alt=support;}
        }
    }
    // Not while he lies in the ragdoll: its bodies follow his node, which
    // the rebase does not carry. He is up again in a few seconds.
    if(!playerDown&&glm::length(origin.local(ecef(lon,lat,alt)))>kRebaseDistance)rebaseOrigin();
    if(!playerDown&&!driving()&&!sailing()&&!piloting()&&!dropFlying()) {
        if(swimming()) {
            jumpOffset=jumpVelocity=0;wasJump=false;
            swimLean+=(double(moving)-swimLean)*(1-std::exp(-5*dt));
            const double lean=35.*rad*swimLean;
            const double heave=.035*std::sin(swimTime*4.);
            const double rootBelow=playerHeight*(std::cos(lean)-kSwimHeadAbove);
            player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt-rootBelow+heave)));
            player->transform().rotation=glm::angleAxis(float(-swimHeading*rad),glm::vec3(0,1,0))
                                         *glm::angleAxis(float(-lean),glm::vec3(1,0,0));
            playPlayerClip(moving?"run":"idle");
        } else {
            bool jump=held(GLFW_KEY_SPACE);
            if(!idleSmoke&&smokeStarted&&worldCapture.pngPath.empty()&&smokeWalk>.6&&smokeWalk<.8)jump=true;
            if(jump&&!wasJump&&jumpOffset<=0)jumpVelocity=std::sqrt(2*22.*1.5);
            wasJump=jump;
            const double previous=jumpOffset;
            if(jumpVelocity-22*dt>-kFallTerminal){jumpOffset+=jumpVelocity*dt-11*dt*dt;jumpVelocity-=22*dt;}
            else{jumpVelocity=-kFallTerminal;jumpOffset+=jumpVelocity*dt;}
            if(jumpOffset<=0) {
                // Come down on water, from an aircraft: he swims.
                if(jumpVelocity<-5.&&tile(lon,lat)&&deepWater(lon,lat,alt)) {
                    transitionTravel(TravelMode::Swim);swimHeading=yaw;alt=waterLevel(lon,lat);
                    text("stream-status","In the water — swim for the shore.");
                } else if(jumpVelocity<0)landed(-jumpVelocity);
                jumpOffset=0;jumpVelocity=0;
            }
            if(jumpOffset>0) {
                feet->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+previous+.06)));
                const auto end=feet->moveAndSlide({0,float((jumpOffset-previous)/dt),0},float(dt));
                const double resolved=end.y-origin.local(ecef(lon,lat,alt+.06)).y;
                if(resolved<jumpOffset-.01&&jumpVelocity>0)jumpVelocity=0;
                jumpOffset=std::max(0.,resolved);
                if(jumpVelocity<0&&feet->isOnFloor()) {
                    landed(-jumpVelocity);
                    alt+=jumpOffset;jumpOffset=jumpVelocity=0;
                }
            }
            player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+jumpOffset+.06)));
            const bool sprint=moving&&held(GLFW_KEY_LEFT_SHIFT);
            playPlayerClip(jumpOffset>0?"jump":sprint?"sprint":moving?"run":"idle");
        }
    }
    updateSinkingCar(dt);
    placeCar();
    placeBoats();
    placeAircraft(dt);
    updateCombat(dt);
    if(smokeStarted&&worldCapture.pngPath.empty()){
        smokeRan=smokeRan||(moving&&animators.front()->currentClip()=="run");
        smokeJumped=smokeJumped||(jumpOffset>.5&&animators.front()->currentClip()=="jump");
    }
    return dt;
}

} // namespace r1::world
