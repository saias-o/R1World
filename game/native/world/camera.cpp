#include "session.hpp"
#include "photo_detection.hpp"

namespace r1::world {

void World::Impl::updateCamera(double dt) {
    if(photoMode) {
        if(!photoResult)camera->fovDegrees=zoomPhotoFov(sceneFov,camera->fovDegrees,saida::Input::scrollDelta().y);
        text("photo-zoom",number(photoZoom(sceneFov,camera->fovDegrees),1)+"×");
        camera->transform().position=player->transform().position+glm::vec3(0,float(playerHeight*.89),0);
        camera->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0))*glm::angleAxis(float(pitch*rad),glm::vec3(1,0,0));
        return;
    }
    // In the air the arrival flies its own camera (World::Impl::placeDropCamera).
    if(dropFlying())placeDropCamera(dt);
    else {
        // At the controls of a plane the view pitches with the nose, a little.
        const double viewPitch=std::clamp(pitch+(piloting()&&!plane.helicopter()?plane.pitch*.6:0.),-80.,40.);
        camera->transform().rotation=glm::angleAxis(float(-(yaw+lookYaw)*rad),glm::vec3(0,1,0))*glm::angleAxis(float(viewPitch*rad),glm::vec3(1,0,0));
        // A car is longer than a man and moves twice as fast, so the camera
        // stands further back -- far enough to see the bonnet turn.
        // A boat's camera stands off by its own length: a container ship is
        // two hundred metres of hull, and eight metres behind its bridge is
        // inside it.
        // Aiming, the camera closes in over his right shoulder.
        aimBlend+=((aiming?1.:0.)-aimBlend)*(1-std::exp(-12*dt));
        const double maxFollow=sailing()?std::clamp(boat.length*1.1+6.,9.,230.)
                              :piloting()?plane.type->length*.9+10.
                              :ridingBike()?5.2-1.8*aimBlend
                              :driving()?std::max(kDrivingFollow,vehicleSpec(*car).length*.7+5.)
                              :kOnFootFollow+(kAimFollow-kOnFootFollow)*aimBlend;
        const double eye=sailing()?std::clamp(boat.length*.12+2.,2.,26.):piloting()?plane.type->height*.75+1.5:driving()?vehicleSpec(*car).height+.5:swimming()?playerHeight*1.17
                        :playerDown?playerHeight*.35:playerHeight*(.89+.06*aimBlend);
        const glm::vec3 anchor=sailing()&&boat.node?boat.node->transform().position
                              :piloting()&&plane.node?plane.node->transform().position
                              :driving()?car->transform().position
                              :playerDown?downAnchor():player->transform().position;
        const glm::vec3 target=anchor+glm::vec3(0,eye,0)
            +camera->transform().rotation*glm::vec3(float(kAimShoulder*aimBlend),0.f,0.f);
        const glm::vec3 backward=camera->transform().rotation*glm::vec3(0,0,1);
        double clear=maxFollow;
        if(auto* physics=engine.sceneTree().world().physics()) {
            const auto hit=physics->raycast(target,backward,float(maxFollow),obstacleFilter());
            if(hit.hit)clear=std::max(.15,double(hit.distance)-.3);
        }
        // Keep the camera above the geographic surface and inside loaded tiles.
        for(double d=.25;d<=clear;d+=.25){
            auto q=onward(lon,lat,backward.x*d,-backward.z*d);
            if(!tile(q.x,q.y)&&piloting())continue;  // in the air, unstreamed ground ahead is no wall
            if(!tile(q.x,q.y)||height(q.x,q.y,alt+jumpOffset)+.3>alt+jumpOffset+.06+eye+backward.y*d){clear=std::max(.15,d-.25);break;}
        }
        followDistance=clear<followDistance?clear:followDistance+(clear-followDistance)*(1-std::exp(-7*dt));
        camera->transform().position=target+backward*float(followDistance);
        // Handed over from the arrival's camera, it eases into this one.
        if(cameraFrom) {
            cameraHandoff=std::min(1.,cameraHandoff+dt/.9);
            const float s=float(smooth(cameraHandoff));
            camera->transform().position=glm::mix(cameraFrom->position,camera->transform().position,s);
            camera->transform().rotation=glm::normalize(glm::slerp(cameraFrom->rotation,camera->transform().rotation,s));
            camera->fovDegrees=glm::mix(cameraFrom->fov,sceneFov,s);
            if(cameraHandoff>=1)cameraFrom.reset();
        }
#ifdef SAIDA_ENABLE_XR
        if(vrRig) {
            // First person: the body would fill the view from inside.
            player->setVisible(false);
            vrRig->transform().position=anchor;
            vrRig->yawDegrees=float(-(yaw+lookYaw));
        }
#endif
    }
    foldCanopy(dt);
    if(!vr&&blastShake>0)camera->transform().rotation=glm::normalize(camera->transform().rotation*
        glm::angleAxis(float(std::sin(fightClock*63)*blastShake*.022),glm::vec3(1,0,0))*
        glm::angleAxis(float(std::cos(fightClock*47)*blastShake*.016),glm::vec3(0,0,1)));
    runSmokeDive();
}

} // namespace r1::world
