#include "session.hpp"

namespace r1::world {
void World::Impl::transitionTravel(TravelMode next,bool carryView) {
    travel.transition(next,[&](TravelMode previous,TravelMode destination) {
        dismountPlayerFromBike();
        if(previous==TravelMode::Car&&destination!=TravelMode::Car&&car)
            if(auto* driver=r1::VehicleRuntime::driver(*car))driver->parked=true;
        if(carryView&&TravelState::vehicle(previous)&&!TravelState::vehicle(destination))yaw=wrap(yaw+lookYaw);
        lookYaw=0;lookIdle=0;
    },[&](TravelMode,TravelMode destination) {
        if(!player)return;
        const bool aboard=TravelState::vehicle(destination);
        player->setEnabled(!aboard);
        if(aboard) {
            jumpOffset=jumpVelocity=0;
            for(auto* animator:animators)animator->play("idle");
            if(destination==TravelMode::Car)mountPlayerOnBike();
        } else {
            player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+jumpOffset+.06)));
            player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
            followDistance=std::min(followDistance,kOnFootFollow);
        }
        if(destination==TravelMode::Swim){swimTime=0;swimLean=0;swimHeading=yaw;jumpOffset=jumpVelocity=0;}
    });
}
}
