#include "session.hpp"

namespace r1::world {
void World::Impl::runRocketSmoke(double dt) {
    if(dropFlying())return;
    rocketSmokeTime+=dt;
    auto fail=[&](const char* why){saida::Log::error("[World rocket E2E] FAIL ",why," phase=",rocketSmokePhase);testFailed=true;engine.sceneTree().quit();};
    auto next=[&]{++rocketSmokePhase;rocketSmokeTime=0;};
    if(rocketSmokeTime>35){fail("phase timeout");return;}
    if(rocketSmokePhase==0&&rocketSmokeTime>3) {
        clearPolice();health.protect(90);
        const auto at=player->transform().position+glm::vec3(0,90,0);
        rocketSmokeWall=boxCollider(engine.sceneTree().world(),"Rocket thin wall fixture",at,{8,8,.1f});
        rocketSmokeCount=rocketImpacts;next();
    } else if(rocketSmokePhase==1&&rocketSmokeTime>.2) {
        auto* shape=dynamic_cast<saida::CollisionShapeNode*>(rocketSmokeWall->children().front().get());
        const auto at=shape->offset;
        if(rocketExposed(at+glm::vec3(0,0,.2f),at-glm::vec3(0,0,2))||
           !rocketExposed(at+glm::vec3(0,0,.2f),at+glm::vec3(0,0,2))) {fail("wall does not shield blast");return;}
        if(!launchRocket(at+glm::vec3(0,0,30),at,{},false)){fail("fixture launch refused");return;}
        next();
    } else if(rocketSmokePhase==2&&rocketSmokeTime>1) {
        if(rocketImpacts!=rocketSmokeCount+1){fail("thin wall missed or exploded twice");return;}
        retireRocketVehicle(rocketSmokeWall);rocketSmokeWall=nullptr;
        selectWeapon(r1::combat::Weapon::RocketLauncher);arsenal.update(3);
        const int shots=rocketLaunches;fireRocket();
        if(rocketLaunches!=shots+1||arsenal.loaded()!=0||!arsenal.reloading()){fail("on-foot launcher/ammo");return;}
        auto flight=std::find_if(rockets.begin(),rockets.end(),[](const auto& r){return r.life>0;});
        const auto before=origin.origin+origin.basis*glm::dvec3(flight->position);
        const double savedLon=lon;lon+=.001;rebaseOrigin();lon=savedLon;
        const auto after=origin.origin+origin.basis*glm::dvec3(flight->position);
        if(glm::distance(before,after)>.002){fail("rocket moved geographically during rebase");return;}
        clearRockets();
        saida::Log::info("[World rocket E2E] PASS swept thin wall, blast cover, exactly one impact, on-foot ammunition and rebase");
        const auto found=aircraftPrototypes.find("helicopter");if(found==aircraftPrototypes.end()){fail("helicopter asset missing");return;}
        plane.node=engine.sceneTree().world().addChild(clonePlant(*found->second));plane.type=r1::aircraftType("helicopter");
        plane.lon=lon;plane.lat=lat;plane.alt=alt+90;plane.airborne=true;plane.rotor=1;plane.yaw=yaw;
        collectAircraftParts(plane);piloting=true;player->setEnabled(false);placeAircraft(0);next();
    } else if(rocketSmokePhase==3&&rocketSmokeTime>.3) {
        arsenal.update(3);const int shots=rocketLaunches;fireRocket();
        auto r=std::find_if(rockets.begin(),rockets.end(),[](const auto& r){return r.life>0;});
        if(rocketLaunches!=shots+1||r==rockets.end()||r->filter.ignore!=aircraftCollider(plane.node)->bodyId()) {fail("helicopter launch/self-filter");return;}
        clearRockets();
        wanted.percent=100;wanted.violent=true;wanted.helicopter=true;
        policeLastKnown=origin.origin+origin.basis*glm::dvec3(policePlayerPosition());
        rocketSmokeCount=rocketDestroyed;next();
    } else if(rocketSmokePhase==4) {
        if(piloting)return;
        if(plane.node||!player->enabled()||!playerDown||rocketDestroyed<=rocketSmokeCount){fail("police helicopter did not eject pilot and retire hull");return;}
        saida::Log::info("[World rocket E2E] PASS player helicopter shot and live police aerial rocket, pilot ejected");
        if(!policeHelicopter){fail("police helicopter missing before retaliation");return;}
        const auto air=nodePosition(*policeHelicopter);
        // This phase verifies destruction ownership, not the accuracy of a
        // stationary test aim against an aircraft moving at up to 38 m/s.
        // Projectile contact is independently exercised by the thin wall.
        explodeRocket(air+glm::vec3(0,1,0),aircraftCollider(policeHelicopter),false);
        next();
    } else if(rocketSmokePhase==5&&rocketSmokeTime>1) {
        if(policeHelicopter||policeAirRespawn<15){fail("police helicopter destruction/replacement delay");return;}
        saida::Log::info("[World rocket E2E] PASS police helicopter destroyed, replacement delayed");
        clearPolice();playerFall.release();playerBody->transform()=playerBodyRest;playerDown=wasted=false;
        if(!car){fail("car fixture unavailable");return;}
        lon=carLon;lat=carLat;alt=carAlt;jumpOffset=jumpVelocity=0;
        player->transform().position=nodePosition(*car)+glm::vec3(1.8f,0,0);
        if(!enterCar()){fail("car entry");return;}
        rocketSmokeCount=rocketDestroyed;next();
    } else if(rocketSmokePhase==6&&rocketSmokeTime>.3) {
        if(!policeRocket(policePlayerPosition()+glm::vec3(0,40,0),nullptr,false)){fail("police vehicle shot refused");return;}
        next();
    } else if(rocketSmokePhase==7&&!driving) {
        if(car||!player->enabled()||!playerDown||rocketDestroyed!=rocketSmokeCount+1){fail("car destruction not atomic");return;}
        saida::Log::info("[World rocket E2E] PASS car exploded once, driver ejected, ownership cleared");
        next();
    } else if(rocketSmokePhase==8&&rocketSmokeTime>.16&&std::getenv("R1WORLD_ROCKET_CAPTURE")) {
        saida::CaptureRequest capture;capture.pngPath=std::getenv("R1WORLD_ROCKET_CAPTURE");capture.waitForAssets=false;
        engine.captureFrameThenExit(capture);rocketSmoke=false;
    } else if(rocketSmokePhase==8&&rocketSmokeTime>1) {
        if(car||driving||rocketDestroyed!=rocketSmokeCount+1){fail("destroyed vehicle returned or duplicate impact");return;}
        saida::Log::info("[World rocket E2E] PASS all Rocket Update scenarios");engine.sceneTree().quit();rocketSmoke=false;
    }
}

} // namespace r1::world
