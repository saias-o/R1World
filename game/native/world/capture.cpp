#include "session.hpp"

namespace r1::world {

bool World::Impl::updateCapture(double dt) {
    if(smoke&&!worldCapture.pngPath.empty()) {
        if(captureView.set) {
            // The viewpoint is relative to where the player stands now, not
            // to the origin of the spawn: a first visit spawns on ground
            // that the surveyed relief later raises (Kyoto: 0 m, then the
            // hillside), and a camera 30 m above the old ground is inside
            // the hill.
            glm::vec3 at=player->transform().position;
            if(captureAltitude)at.y+=float(*captureAltitude-alt);
            glm::vec3 position=at+glm::vec3(captureView.position[0],captureView.position[1],captureView.position[2]);
            glm::vec3 target=at+glm::vec3(captureView.target[0],captureView.target[1],captureView.target[2]);
            if(captureGeo) {
                // A surveyed inspection camera may overlap the spawn.
                // Keep the player active for streaming, hide only its mesh.
                player->setVisible(false);
                const auto& geo=*captureGeo;
                const double eye=captureAltitude?*captureAltitude:height(geo[0],geo[1])+geo[2];
                Frame photo(geo[0],geo[1],eye);
                position=origin.local(photo.origin);
                const glm::dvec3 direction(captureView.target[0]-captureView.position[0],
                    captureView.target[1]-captureView.position[1],captureView.target[2]-captureView.position[2]);
                target=origin.local(photo.origin+photo.basis*direction);
            }
            camera->transform().position=position;
            camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
        }
        if(std::getenv("R1WORLD_CAPTURE_VEHICLE")) {
            r1::VehicleRuntime::synchronize(*car);
            const auto& spec=vehicleSpec(*car);const auto rotation=car->transform().rotation;
            const auto position=car->transform().position+rotation*glm::vec3(float(spec.width*1.8),float(spec.height*1.4),float(-spec.length*1.2));
            const auto target=car->transform().position+rotation*glm::vec3(0,float(spec.height*.5),0);
            player->setVisible(false);camera->transform().position=position;
            camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
        }
        if(motorcycleCaptureStarted&&car) {
            r1::VehicleRuntime::synchronize(*car);
            const auto rotation=car->transform().rotation;
            const auto at=car->transform().position;
            const bool armed=std::getenv("R1WORLD_CAPTURE_MOTORCYCLE_AIM")!=nullptr;
            camera->transform().position=at+rotation*(armed?glm::vec3(-1.8f,1.5f,2.6f):glm::vec3(1.7f,1.3f,-1.7f));
            camera->transform().rotation=glm::quatLookAt(glm::normalize(at+glm::vec3(0,.95f,0)-camera->transform().position),glm::vec3(0,1,0));
            if(std::getenv("R1WORLD_CAPTURE_MOTORCYCLE_AIM")) {
                selectWeapon(r1::combat::Weapon::Pistol);setAiming(true);poseArms();bikeHandlePose();
            }
        }
        if(std::getenv("R1WORLD_CAPTURE_SIGNAL"))frameTrafficSignal();
        if(const char* view=std::getenv("R1WORLD_CAPTURE_LAUNCHER")) {
            const std::string angle=view;
            if(angle!="side"&&angle!="front"&&angle!="down")throw std::runtime_error("R1WORLD_CAPTURE_LAUNCHER requires side, front or down");
            selectWeapon(r1::combat::Weapon::RocketLauncher);setAiming(true);
            player->setVisible(true);
            const auto turn=glm::normalize(glm::quat_cast(glm::mat3(playerBody->worldTransform())/float(humanScale)));
            const auto aim=turn*glm::normalize(angle=="side"?glm::vec3(-.7f,0,.7f):angle=="down"?glm::vec3(0,-.8f,.6f):glm::vec3(0,0,1));
            poseArms(&aim);for(auto* arm:playerArms)arm->setWeightNow(1);
            const auto at=nodePosition(*player);
            const auto offset=angle=="side"?glm::vec3(-3.0f,1.6f,1.1f):angle=="front"?glm::vec3(-.75f,1.65f,3.5f):glm::vec3(-2.4f,2.5f,2.3f);
            camera->transform().position=at+turn*offset;
            camera->transform().rotation=glm::quatLookAt(glm::normalize(at+glm::vec3(0,float(playerHeight*.72),0)-camera->transform().position),glm::vec3(0,1,0));
            camera->fovDegrees=42;
            // This close-up needs the avatar and weapon, not every distant
            // map tile. Offline scenery must not prevent model inspection.
            if(!captureQueued&&captureWait>8.&&engine.resources().assetLoadsSettled()) {
                style("hud","display","none");minimapUi->setEnabled(false);
                captureQueued=true;engine.captureFrameThenExit(worldCapture);
            }
        }
        auto want=nearby(lon,lat);
        captureWait+=dt;
        // A first visit mounts its ground before its streets: a picture of
        // that is a picture of nothing. The capture waits for OSM, and
        // after two minutes takes what there is and says so.
        // The same for the surveyed summits: a mountain photographed
        // before its summit list landed is the relief they correct.
        size_t provisional=0,summitsPending=0;
        // And the rings to the horizon, where they are drawn.
        const bool farPending=farRelief.node&&farRelief.node->visible()&&
            (!farRelief.job||[&]{std::lock_guard<std::mutex> g(farRelief.job->lock);return farRelief.job->busy||!farRelief.job->done.empty();}()
             ||[&]{for(int k=0;k<kFarLevels;++k)if(!farRelief.node->level(k))return true;return false;}());
        bool settled=engine.resources().assetLoadsSettled()&&std::all_of(want.begin(),want.end(),[&](Tile t){
            auto i=loaded.find(t.key());if(i==loaded.end())return false;
            if(i->second.data.value("provisional",false))++provisional;
            if(i->second.data.contains("peaks")&&i->second.data.at("peaks").value("pending",false))++summitsPending;
            // Its meshes too: a tile cooked again uploads them over
            // several frames (uploadParts), and its props may be done first.
            const auto& resident=i->second;
            if(!resident.geography||resident.nextPart<std::any_cast<const PreparedTile&>(resident.served->prepared).parts.size())return false;
            if(!buildingsReady(resident))return false;
            for(const auto& building:resident.buildings) {
                const int desired=building.lod->desiredLevel();
                if(desired<0)return false;
                const auto& state=building.levels[size_t(desired)];
                if(!state.refused&&(building.lod->activeLevel()!=desired||building.lod->fadingLevel()>=0))return false;
            }
            return resident.nextProp==resident.props.size()&&
                resident.nextLettering==resident.data.value("lettering",json::array()).size();
        });
        // A capture that waits says what for, every ten seconds: otherwise
        // the only report is the smoke's data timeout, which blames the
        // network for whatever it was (CLAUDE.md §3).
        captureReport+=dt;
        if(!settled&&!captureQueued&&captureReport>=10.) {
            captureReport=0;
            std::string why;
            for(Tile t:want) {
                auto i=loaded.find(t.key());
                if(i==loaded.end()){why+=" "+t.key()+" not mounted;";continue;}
                const auto& r=i->second;
                const size_t parts=r.served?std::any_cast<const PreparedTile&>(r.served->prepared).parts.size():0;
                if(!r.geography)why+=" "+t.key()+" no geography;";
                else if(r.nextPart<parts)why+=" "+t.key()+" parts "+std::to_string(r.nextPart)+"/"+std::to_string(parts)+";";
                else if(r.nextProp<r.props.size())why+=" "+t.key()+" props "+std::to_string(r.nextProp)+"/"+std::to_string(r.props.size())+";";
            }
            saida::Log::info("[World capture wait] ",int(captureWait)," s, not settled:",why.empty()?" every tile is in":why);
        }
        // Summits refine a relief that is already there: a minute less.
        if(settled&&((provisional&&captureWait<120.)||((summitsPending||farPending)&&captureWait<45.)))settled=false;
        if(settled&&provisional&&!captureQueued)
            saida::Log::error("[World capture] ",provisional," of ",want.size(),
                              " tiles still wait for OSM after two minutes; photographed as they are");
        if(settled&&summitsPending&&!captureQueued)
            saida::Log::error("[World capture] ",summitsPending," of ",want.size(),
                              " tiles still wait for their surveyed summits after 45 s; photographed as they are");
        // R1WORLD_CAPTURE_SEA: a picture of the sea waits for its ships
        // (half a minute at most -- the log then says there were none).
        if(settled)captureSeaWait+=dt;
        const bool seaReady=(!std::getenv("R1WORLD_CAPTURE_SEA")||(!seaShips.empty()&&captureSeaWait>4.)
            ||captureSeaWait>30.)
            // The local weather is part of the picture: a few seconds for it.
            &&(effectiveWeather().known||captureSeaWait>6.)
            // So is the bounce light: probes still warming photograph as a flat ambient.
            &&(giSettled()||captureSeaWait>60.);
        if(settled&&seaReady&&!captureQueued&&!giSettled())
            saida::Log::error("[World capture] GI warm-up at ",int(engine.sceneTree().world().settings().giWarmupProgress*100.f),
                              "% after a minute; photographed as it is");
        if(settled&&seaReady&&!captureQueued){
            if(const char* motorcycle=std::getenv("R1WORLD_CAPTURE_MOTORCYCLE")) {
                auto fail=[&](const char* reason){saida::Log::error("[World E2E] FAIL motorcycle: ",reason);testFailed=true;engine.sceneTree().quit();};
                if(!motorcycleCaptureStarted) {
                    const std::string kind=motorcycle;
                    if(kind=="traffic") {
                        Reach target;
                        for(auto& [key,t]:loaded)for(size_t i=0;i<t.cars.size();++i)
                            if(t.cars[i]&&trafficSlotLive(t,i)&&r1::motorcycle(vehicleSpec(*t.cars[i]).name)){target={t.cars[i],&t,i,2};break;}
                        if(!target.node){fail("no live traffic motorcycle at this spawn");return false;}
                        auto* collider=r1::VehicleRuntime::body(*target.node);
                        if(!collider||!collider->findByPath("Rider")){fail("traffic motorcycle has no driver");return false;}
                        if(!takeTrafficCar(target)){fail("traffic takeover refused");return false;}
                        if(target.tile->cars[target.agent]){fail("traffic still owns the stolen motorcycle");return false;}
                    } else {
                        if(kind!="moto_city"&&kind!="moto_cross"){fail("unknown requested motorcycle");return false;}
                        const auto error=spawnParkedVehicle(kind);
                        if(!error.empty()){fail(error.c_str());return false;}
                        if(parked.back().node->findByPath("Vehicle collider/Rider")){fail("spawned motorcycle has a driver");return false;}
                        takeAbandonedCar({parked.back().node,nullptr,parked.size()-1,1});
                    }
                    lon=carLon;lat=carLat;alt=carAlt;
                    if(!enterCar()||!playerBikeRider||playerBikeRider!=playerBody||playerBody->parent()!=r1::VehicleRuntime::body(*car)){
                        fail("real player body is not attached to the solver");return false;
                    }
                    if(car->findByPath("Vehicle collider/Rider")){fail("previous driver remains on the motorcycle");return false;}
                    motorcycleCaptureStarted=true;
                    // Clear the unused parked car from this inspection view;
                    // it otherwise stands between the camera and the saddle.
                    for(const auto& p:parked)if(p.node!=car)p.node->setVisible(false);
                    saida::Log::info("[World E2E] PASS motorcycle empty spawn or live traffic takeover and single solver-attached player");
                    return false;
                }
                motorcycleCaptureWait+=dt;
                if(std::getenv("R1WORLD_CAPTURE_MOTORCYCLE_AIM")) {
                    selectWeapon(r1::combat::Weapon::Pistol);setAiming(true);poseArms();bikeHandlePose();
                    if(motorcycleCaptureWait>1.&& !motorcycleCaptureFired) {
                        const auto before=arsenal.loaded();fire();motorcycleCaptureFired=true;
                        if(arsenal.loaded()!=before-1||combatFilter().ignore!=r1::VehicleRuntime::body(*car)->bodyId()){
                            fail("mounted weapon shot or own-body filter failed");return false;
                        }
                        saida::Log::info("[World E2E] PASS motorcycle pistol fires with shared ammo, effects and collider filtering");
                    }
                }
                if(motorcycleCaptureWait<2.)return false;
                // Inspect the final solved pose, not just the IK request:
                // an unreachable target must not pass with a floating hand.
                const auto* animator=animators.front();const auto& rig=*animator->rig();
                const auto& matrices=animator->globalPose().globalMatrices;
                for(int h=0;h<2;++h) {
                    if(h==1&&aiming)continue;
                    const int bone=rig.findBoneIndex(h?"Bip01 R Hand":"Bip01 L Hand");
                    const auto palm=r1::rocketboxBarGrip(rig,h==1);
                    const auto actual=glm::vec3(matrices.at(size_t(bone))*glm::vec4(palm.position,1.f));
                    const float error=glm::length(actual-motorcycleContact(*playerBody,vehicleSpec(*car).name,h,true))*float(humanScale);
                    saida::Log::info("[World E2E] motorcycle ",h?"right":"left"," palm grip error=",error," m");
                    if(error>.015f){fail("solved palm is more than 1.5 cm from its handlebar grip");return false;}
                }
                saida::Log::info("[World E2E] PASS solved motorcycle hand contacts");
            }
            // Optional visual proof uses the shipped car, normal cage API
            // and renderer, after source/terrain streaming has completed.
            if(std::getenv("R1WORLD_CAPTURE_VEHICLE_DENT")) {
                if(!captureVehicleDent) {
                    const auto& spec=vehicleSpec(*car);
                    auto* damage=r1::VehicleRuntime::body(*car)->getBehaviour<saida::DeformableMeshBehaviour>();
                    if(!damage->applyImpact({0,float(spec.height*.5),float(-spec.length*.5)},{0,0,1},damage->maximumImpactEnergy)) {
                        saida::Log::error("[World E2E] FAIL vehicle dent: source geometry is not ready");
                        testFailed=true;engine.sceneTree().quit();return false;
                    }
                    captureVehicleDent=true;
                }
                captureVehicleWait+=dt;
                constexpr double kDentSettleTime=2.;
                if(captureVehicleWait<kDentSettleTime||!engine.resources().assetLoadsSettled())return false;
            }
            // An inspection picture is of the world: no HUD, no minimap.
            if(inspectAt){style("hud","display","none");minimapUi->setEnabled(false);}
            size_t plants=0;for(auto& [key,t]:loaded)plants+=t.vegetation.size();
            saida::Log::info("[World nature] resident plants=",plants," shared static prototypes=",naturePrototypes.size());
            int measuredW=0,measuredH=0;engine.window().size(measuredW,measuredH);
            saida::Log::info("[World capture quality] preset=",toasterMode?"toaster":"standard",
                " window=",measuredW,"x",measuredH," farZ=",camera->farZ,
                " gi=",engine.sceneTree().world().settings().giEnabled);
            std::array<size_t,3> buildingLevels{};size_t detailedBuildings=0;
            for(const auto& [key,t]:loaded)for(const auto& b:t.buildings) {
                const int level=b.lod->activeLevel();
                if(level>=0)++buildingLevels[size_t(level)];
                if(level==0&&!t.reducedDensity)++detailedBuildings;
            }
            saida::Log::info("[World building LOD] visible Near/Mid/Far=",buildingLevels[0],"/",buildingLevels[1],"/",buildingLevels[2],
                " detailed Near=",detailedBuildings," Far triangles per building <=12");
            saida::Log::info("[World traffic] resident cars=",trafficLive()," of ",trafficWanted()," asked for");
            const auto& rendering=engine.sceneTree().world().settings();
            saida::Log::info("[World PBR] ibl=",rendering.iblEnabled," specular=",rendering.iblSpecularIntensity,
                             " sky=",rendering.skyboxTexture," exposure=",rendering.skyboxExposure);
            saida::Log::info("[World rain] mm_h=",effectiveWeather().rain,
                " intensity=",rendering.rainIntensity," wetness=",rendering.wetness,
                " storm_clouds=",rendering.stormCloudCover," sheltered=",rainSheltered);
            car->traverse([&](saida::Node& n,const glm::mat4&){
                if(n.material()&&n.name().rfind("paint-",0)==0) {
                    const auto& m=n.material()->desc();
                    saida::Log::info("[World PBR] ",n.name()," metallic=",m.metallic," roughness=",m.roughness,
                                     " type=",int(m.type)," rgb=",m.baseColor.r,",",m.baseColor.g,",",m.baseColor.b);
                }
            });
            size_t spots=0,gear=0,swimmers=0,resting=0;
            for(const auto& [key,t]:loaded) {
                spots+=t.beachSpots.size();
                for(const auto& v:t.beachVisitors)if(v.spot>=0) {
                    gear+=v.furniture&&v.furniture->enabled();
                    const auto& spot=t.beachSpots[size_t(v.spot)];
                    const auto pose=r1::beachPose(spot.towel,spot.water,v.age,unsigned(v.spot));
                    swimmers+=pose.swimming;resting+=pose.recline>.9;
                }
            }
            const auto beachWeather=effectiveWeather();
            saida::Log::info("[World beach] spots=",spots," visitors=",beachLive," towels=",gear,
                " resting=",resting," bathing=",swimmers," temperature=",beachWeather.temperature,
                " known=",beachWeather.temperatureKnown," factor=",
                r1::beachAttendance(localSolarHour(),beachSun,beachWeather));
            captureQueued=true;engine.captureFrameThenExit(worldCapture);
        }
    }
    return true;
}

} // namespace r1::world
