#include "session.hpp"

namespace r1::world {

void World::Impl::updateSmoke(float delta, double dt) {
    if(smoke&&std::getenv("R1WORLD_TEST_VEHICLE_ESCAPE")&&smokeWalk>2){runVehicleEscapeSmoke(delta);return;}
    if(rocketSmoke){runRocketSmoke(delta);return;}
    if(policeSmoke){runPoliceSmoke(delta);return;}
    if(performanceSmoke){runPerformanceSmoke(delta);return;}
    if(smokeFighting){runSmokeFight(dt);return;}
    if(trafficValidation_){validateTraffic(dt);return;}
    if(smokeWaterSpawn&&smokeWalk>1.5) {
        const double covered=glm::length(ecef(lon,lat,alt)-smokeStart);
        const double rootBelow=origin.local(ecef(lon,lat,alt)).y-player->transform().position.y;
        testFailed=!swimming()||!onWater(lon,lat)||carParked||covered<1.||rootBelow<playerHeight*.39||rootBelow>playerHeight*.78;
        saida::Log::info("[World E2E] ",testFailed?"FAIL":"PASS"," water spawn: swam ",covered,
                         "m, body depth=",rootBelow,"m, car parked=",carParked);
        engine.sceneTree().quit();return;
    }
    if(retailTest()){runRetailTest(delta);return;}
    if(smokeFlyWait||smokeFlyPhase){runSmokeFly(delta);return;}
    if(smokeSailWait||smokeSailing){runSmokeSail(delta);return;}
    if(smokeSwimming){runSmokeSwim(delta);return;}
    if(smokeSeaWait){runSmokeSea(delta);return;}
    if(idleSmoke&&smokeStarted&&idleSmokeStopped) {
        const auto displacement=origin.local(ecef(lon,lat,alt))-origin.local(idleSmokeStart);
        idleSmokeDrift=std::max(idleSmokeDrift,std::hypot(displacement.x,displacement.z));
        if(smokeWalk>14.) {
            const bool passed=smokeRan&&idleSmokeDrift<.005&&feet->isOnFloor()
                &&jumpOffset==0&&animators.front()->currentClip()=="idle";
            saida::Log::info("[World E2E] ",passed?"PASS":"FAIL",
                " stop after walking: idle_horizontal_drift_m=",idleSmokeDrift,
                " grounded=",feet->isOnFloor()," clip=",animators.front()->currentClip());
            testFailed=!passed;smokeStarted=false;engine.sceneTree().quit();return;
        }
    }
    if(wadeSmoke&&smokeStarted) {
        auto fail=[&](const std::string& why){
            saida::Log::error("[World E2E] FAIL wade: ",why);testFailed=true;engine.sceneTree().quit();
        };
        if(!wadeTo) {
            std::istringstream in(wadeSmoke);double x=0,y=0;
            if(!(in>>x>>y)||!std::isfinite(x)||!std::isfinite(y))return fail(std::string("R1WORLD_WADE_SMOKE '")+wadeSmoke+"' is not '<lon> <lat>'");
            wadeTo=glm::dvec2(x,y);
        }
        if(swimming())return fail("swimming at "+number(lon,6)+", "+number(lat,6)+" after "+number(wadeOverWater,1)+
                                " m over water, feet at "+number(alt,2)+" m");
        if(onWater(lon,lat)) {
            wadeOverWater+=std::hypot(footEast,footNorth)*std::min(.05,double(delta));
            wadeDeepest=std::max(wadeDeepest,waterLevel(lon,lat)-alt);
        }
        const double left=metresFrom(wadeTo->x,wadeTo->y);
        if(left<2.||smokeWalk>40.) {
            if(left>=2.)return fail("stopped "+number(left,1)+" m short of the target, at "+number(lon,6)+", "+number(lat,6));
            if(wadeOverWater<1.)return fail("the walk never crossed a water cell");
            saida::Log::info("[World E2E] PASS wade: ",wadeOverWater," m walked over water cells, deepest ",
                             wadeDeepest," m of water at the feet (swims from ",playerHeight*kSwimDepth," m)");
            smokeStarted=false;engine.sceneTree().quit();
        }
        return;
    }
    if(!idleSmoke&&smokeStarted&&smokeWalk>4&&(!qualitySwitchSmoke||qualitySwitches>=24)) {
        if(!smokeRan||!smokeJumped||jumpOffset!=0||followDistance<.15||followDistance>kOnFootFollow+.001){
            saida::Log::error("[World E2E] FAIL player animation/jump/follow");testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] player run/jump/landing/follow passed, distance=",followDistance);
        sayBodies();
        if(!trunksSolid()||!groundMatchesDrawing()||!seamsClosed()){testFailed=true;engine.sceneTree().quit();return;}
        // A neighbourhood that asked for people and shows none is the
        // failure; a moor at night that asked for none is not one.
        if(bodiesRefused>0||feetRefused) {
            saida::Log::error("[World E2E] FAIL crowd: ",bodiesRefused," people cannot look or stagger",
                              feetRefused?", and the player's feet are no physics body":"");
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(crowdWanted()>0&&crowdLive()==0) {
            saida::Log::error("[World E2E] FAIL crowd: ",crowdWanted()," people asked for, none placed");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] crowd: ",crowdLive()," people of ",crowdWanted(),
                         " asked for at ",localSolarHour(),"h solar (factor ",crowdFactor(),"), bumps ",playerBumps);
        if(ragdollRefused>0) {
            saida::Log::error("[World E2E] FAIL combat: ",ragdollRefused," knocked-down bodies refused by the physics world");
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeStarted=false;
        if(perfSmoke=="walk") {saida::Log::info("[World perf] PASS warm walking");engine.sceneTree().quit();return;}
        if(perfSmoke.empty()&&!smokeFought){startSmokeFight();return;}
        if(!smokeDroveOnce&&!onSeaIce(lon,lat)){startSmokeDrive();return;}
        if(!smokeDroveOnce)saida::Log::info("[World E2E] on the sea ice: no car to drive, the walk is the test");
        smokeFinishPhase();
    }
    if(smokeApproach) {
        if(carDistance()<=kCarReach*.8){smokeApproach=false;beginSmokeDriving();return;}
        if(smokeApproachTime>20.) {
            // What stands in the way, said: the next step toward the car.
            auto to=origin.local(ecef(carLon,carLat,carAlt))-origin.local(ecef(lon,lat,alt));
            const double l=std::max(1e-6,std::hypot(to.x,to.z));
            const auto q=onward(lon,lat,to.x/l*.5,-to.z/l*.5);
            saida::Log::error("[World E2E] FAIL could not walk back to the car, still ",
                              carDistance(),"m away after ",smokeApproachTime,"s; player at ",lon,", ",lat,
                              " alt ",alt,", car alt ",carAlt,"; next step: water=",onWater(q.x,q.y,alt),
                              " obstacle=",blocked(q.x,q.y,alt)," blocked=",blocked(q.x,q.y,alt),
                              " level there ",height(q.x,q.y,alt));
            testFailed=true;engine.sceneTree().quit();
        }
        return;
    }
    if(smokeDriving&&smokeBrake) {
        smokeDriveTime+=std::min(.05,double(delta));
        // The traffic takeover also exercises service braking before exit.
        if(std::abs(carSpeed)>kSmokeBrakeSpeed) {
            if(smokeDriveTime>20.) {
                saida::Log::error("[World E2E] FAIL the car would not slow down, still ",
                                  std::abs(carSpeed)," m/s");
                testFailed=true;engine.sceneTree().quit();
            }
            return;
        }
        smokeDriving=false;
        saida::Log::info("[World E2E] braked to ",std::abs(carSpeed)," m/s");
        if(!leaveCar()||driving()||!player->enabled()) {
            saida::Log::error("[World E2E] FAIL could not step out of the car at ",
                              std::abs(carSpeed)," m/s");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] on foot again at ",lon,", ",lat);
        if(!smokeTookOver&&trafficLive()>0){runSmokeTakeover();return;}
        smokeFinishPhase();
        return;
    }
    if(smokeDriving) {
        smokeDriveTime+=std::min(.05,double(delta));
        smokeTopSpeed=std::max(smokeTopSpeed,carSpeed);
        size_t live=0;
        for(auto& [key,t]:loaded)
            for(const auto& a:t.flow.agents()) {
                if(!a.alive)continue;
                ++live;
                smokeTrafficMoved=smokeTrafficMoved||a.speed>2.f;
            }
        smokeTrafficSeen=std::max(smokeTrafficSeen,live);
        // Check the exit while the car is still moving. In a dense city a
        // clear 60 m stretch can end before five seconds at full throttle.
        const double covered=glm::length(ecef(lon,lat,alt)-smokeDriveStart);
        const double needed=std::min(20.,smokeClear*.6);
        if(smokeDriveTime<2. || (smokeDriveTime<5. &&
           (covered<needed||std::abs(carSpeed)<=kSmokeBrakeSpeed)))return;
        // A street that asked for cars and got none is the failure this
        // assertion exists for -- and a spawn on a moor that asked for
        // none is not one, so the claim is checked against the ask.
        if(trafficWanted()>0&&(smokeTrafficSeen==0||!smokeTrafficMoved)) {
            saida::Log::error("[World E2E] FAIL traffic: ",trafficWanted(),
                              " cars asked for, ",smokeTrafficSeen," seen, moving=",
                              smokeTrafficMoved);
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] traffic: ",smokeTrafficSeen," cars driving of ",
                         trafficWanted()," the neighbourhood asked for");
        // What is actually being asserted: that the car goes somewhere, and
        // that it goes there faster than a man can run (7 m/s sprinting).
        // The distance is measured against the clearance the driver found
        // rather than against a fixed number, because a spawn is wherever
        // the map was clicked and some of them are courtyards.
        if(covered<needed||smokeTopSpeed<7.2||std::abs(carSpeed)<=kSmokeBrakeSpeed) {
            saida::Log::error("[World E2E] FAIL drive: covered=",covered,
                              "m of ",smokeClear,"m clear, top=",smokeTopSpeed," m/s","; last stopped by ",carStop," at ",carLon,", ",carLat," alt ",carAlt);
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] drive passed, covered=",covered,
                         "m top=",smokeTopSpeed," m/s (",int(smokeTopSpeed*3.6)," km/h), power loss=",
                         r1::VehicleRuntime::driver(*car)->damage);
        reportChurn("driving");measureWalk();
        auto* body=r1::VehicleRuntime::body(*car);
        const auto velocity=body->physicsWorld()->linearVelocity(body->bodyId());
        const double exitSpeed=carSpeed;
        if(!leaveCar()||driving()||!player->enabled()||
           glm::distance(velocity,body->physicsWorld()->linearVelocity(body->bodyId()))>.001f) {
            saida::Log::error("[World E2E] FAIL moving exit or vehicle momentum at ",exitSpeed," m/s");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS moving exit at ",exitSpeed," m/s with vehicle momentum preserved");
        smokeDriving=false;
        if(perfSmoke=="drive"){saida::Log::info("[World perf] PASS warm driving");engine.sceneTree().quit();return;}
        if(!smokeTookOver&&trafficLive()>0){runSmokeTakeover();return;}
        smokeFinishPhase();
        return;
    }
}

} // namespace r1::world
