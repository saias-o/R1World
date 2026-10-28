#include "session.hpp"

namespace r1::world {

bool World::Impl::updateHud(float delta) {
    updateRain(delta);
    if(hud>.5) {
        hud=0;
        updateMinimap();
        text("local-conditions",weatherLabel());
        text("coordinates",number(lat)+"°  /  "+number(lon)+"°     "+number(alt,1)+" m"
             +(driving?"     "+std::to_string(int(std::round(std::abs(carSpeed)*3.6)))+" km/h":"")
             +(sailing?"     "+std::to_string(int(std::round(std::abs(boat.speed)*1.943844)))+" knots":"")
             +(piloting?"     "+std::to_string(int(std::round(std::abs(plane.speed)*3.6)))+" km/h · "
                +std::to_string(int(std::round(plane.alt-planeGround)))+" m AGL"
                +(plane.helicopter()?"":" · throttle "+std::to_string(int(std::round(plane.lever*100)))+" %"):""));
        Reach within;BoatReach moored;PlaneReach standing;double howFar=0,boatFar=0,planeFar=0;
        std::string mode=driving
            ?" · Driving · F: get out"
            :piloting?std::string(plane.helicopter()
                ?" · Helicopter · Click: rocket · 3: launcher · E: reload · Space/Shift: climb/descend · F: get out"
                :" · Plane · W/S: throttle · A/D: bank · Space/Shift: pitch up/down · F: get out")
              +(tile(lon,lat)?"":" · terrain below the aircraft unknown")
            :sailing?" · At the helm · F: disembark or step aboard"
            :swimming?" · In the water · WASD/ZQSD: swim · F: climb back aboard"
            :nearestAircraft(standing,planeFar)?" · F: take the aircraft"
            :nearestBoat(moored,boatFar)?" · F: take the boat"
            :(nearestCar(within,howFar)?" · F: get in the vehicle":performanceDebug?" · Esc: map":" · M: map");
        const auto* currentTile=tile(lon,lat);
        // A cook that ran out of memory is said where the player looks,
        // and fails the smoke on that reason (World service, OUT-OF-MEMORY).
        const auto serviceStatus=service->status();
        if(serviceStatus.outOfMemory>outOfMemorySaid) {
            outOfMemorySaid=serviceStatus.outOfMemory;
            saida::Log::warn("[World memory] the world service ran out of memory (",outOfMemorySaid," times); ",
                             serviceStatus.allowed," tile(s) cooked at once now, private ",int(privateMegabytes())," MB");
            if(smoke){saida::Log::error("[World E2E] FAIL out of memory cooking ",serviceStatus.error);testFailed=true;engine.sceneTree().quit();return false;}
        }
        if(outOfMemorySaid)mode+=" · Memory full: tiles load "+std::string(serviceStatus.allowed>1?"more slowly":"one at a time");
        if(serviceStatus.memoryPaused)mode+=" · Loading waits for free memory";
        if(currentTile&&currentTile->data.value("provisional",false))
            mode+=" · Streets and buildings on their way (OpenStreetMap)";
        if(currentTile&&currentTile->data.value("groundPending",false))
            mode+=" · Provisional terrain: elevation loading";
        std::string retailPlace;
        for(const auto& [key,t]:loaded) {
            auto p=t.frame.local(ecef(lon,lat,alt));const r1::P2 q{p.x,p.z};
            for(const auto& room:t.interiors)if(r1::pointInPolygon(q,room.plan.ring))retailPlace=room.plan.place(q);
            if(!retailPlace.empty())break;
            for(const auto& lot:t.parking){bool in=false;for(const auto& ring:lot.rings)in^=r1::pointInPolygon(q,ring);
                if(in){retailPlace="Car park "+lot.name;break;}}
        }
        if(!retailPlace.empty())mode=" · "+retailPlace+mode;
        combatHud();
        text("stream-status",std::to_string(loaded.size())+" active tiles"+mode
             +(physicsRefused?" · collisions incomplete: the physics world is full":""));
    }
    return true;
}

} // namespace r1::world
