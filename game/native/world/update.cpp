#include "session.hpp"
#include "simulation_time.hpp"

namespace r1::world {
void World::Impl::update(float delta) {
    SAIDA_PROFILE_SCOPE("World/Update");
    SAIDA_PROFILE_COUNTER("World/PerfPhase", !perfSmoke.empty()&&perfReady?(smokeDriving?2:smokeStarted?1:0):0);
    const auto now=std::chrono::steady_clock::now();
    const double frameSeconds=lastFrame.time_since_epoch().count()
        ?std::chrono::duration<double>(now-lastFrame).count():0.;
    // A slow frame says why until he lands, not only for the ten seconds
    // counted: the world first drawn is where it would freeze.
    const bool landing=drop.phase!=Drop::None;
    if((!arrivalSaid||landing)&&lastFrame.time_since_epoch().count()) {
        const double frameMs=std::chrono::duration<double,std::milli>(now-lastFrame).count();
        if(!arrivalSaid){++arrivalFrames;arrivalHitches+=frameMs>33.;arrivalWorst=std::max(arrivalWorst,frameMs);}
        if(frameMs>50.) {
            const double ours=cost.stream+cost.parts+cost.warm+cost.props+cost.distant+cost.world;
            saida::Log::info("[World frame] ",frameMs," ms: stream ",cost.stream," parts ",cost.parts," warm ",cost.warm,
                             " props ",cost.props," distant ",cost.distant," world ",cost.world,
                             " engine ",frameMs-ours);
        }
        if(!arrivalSaid&&std::chrono::duration<double>(now-goStarted).count()>10.) {
            saida::Log::info("[World streaming] arrival: ",arrivalHitches," of ",arrivalFrames,
                             " frames over 33 ms, worst ",arrivalWorst," ms");
            sayBodies();
            arrivalSaid=true;
        }
    }
    lastFrame=now;
    const bool debugKeys[]={held(GLFW_KEY_R),held(GLFW_KEY_V),held(GLFW_KEY_Y),held(GLFW_KEY_M),held(GLFW_KEY_T)};
    samplePerformance(frameSeconds);handlePerformanceKeys(debugKeys);placePerformanceCanvas();
    cost=FrameCost{};
    if(checkPhysics())return;
    if(!refreshInterface())return;
    if(!updateSession(delta,frameSeconds))return;
    updateWorldScene(delta);
    const SimulationTime simulation(delta);
    SAIDA_PROFILE_COUNTER("World/DroppedSimulationMs",simulation.dropped*1000.);
    for(int step=0;step<simulation.steps;++step) {
        updateCheats(simulation.step);
        if(drop.phase!=Drop::Zoom)cost.world+=timed([&]{
            updateTraffic(float(simulation.step));updateBeach(float(simulation.step));
            updateCrowd(float(simulation.step));updateFauna(float(simulation.step));
            updatePolice(simulation.step);updateSea(simulation.step);
        });
        updatePlayer(float(simulation.step),step==0);
    }
    const double dt=simulation.seconds;
    updateCamera(dt);
    // Photo detection and exposure run after scene transforms/physics, against
    // the same animal poses and occluders that the renderer will draw.
    if(!updateCapture(dt))return;
    if(!updateHud(delta))return;
    updateSmoke(delta,dt);
}

bool World::Impl::updateSession(float delta, double frameSeconds) {
    clock+=delta;
    applyForcedTime();
    updateCityLookup();
    advanceDrop(saida::Time::unscaledDelta());
    updateGlobe(saida::Time::unscaledDelta());
    if((menu||dropGlobe())&&globe){SAIDA_PROFILE_SCOPE("World/MapPump");const bool map=menuTiles.pump(),imagery=imageryTiles.pump();if(map||imagery)renderMapTiles();}
    if(drop.phase!=Drop::None&&!menu) {
        // How smooth the whole arrival was, said when he lands.
        const int k=drop.stage();
        ++drop.frames[k];drop.hitches[k]+=frameSeconds>.033;drop.worst[k]=std::max(drop.worst[k],frameSeconds*1000.);
        captionDrop(delta);updateDropSounds(delta);
    }
    if(smoke && pending && refused==tileAt(pickLon,pickLat).key()) {
        saida::Log::error("[World E2E] FAIL ",refused," did not fit the resident vertex budget");
        testFailed=true;engine.sceneTree().quit();return false;
    }
    if(smoke){testElapsed+=delta;if(testElapsed>(trafficValidation_?trafficValidationSeconds_*3.+120.:hopWanted?420:smokeFly?300:smokeDive?240:180)){saida::Log::error("[World E2E] FAIL data timeout");testFailed=true;engine.sceneTree().quit();return false;}}
    if(testResume){
        resumeWait+=delta;
        if(resumeWait>.25){
            testClick("resume");double d=glm::length(ecef(lon,lat,alt)-smokeStart);
            testFailed=d<=2||menu||!smokeRan||!smokeJumped;
            saida::Log::info("[World E2E] ",testFailed?"FAIL":"PASS"," walking=",d,"m resident=",loaded.size()," resume=",!menu);
            engine.sceneTree().quit();
        }
        return false;
    }
    if(hopArmed) {
        // A menu that has just been shown has not been laid out yet, and an
        // element's absolute offset is still the hidden document's until it
        // has. Clicking in that frame aims at the top-left corner and hits
        // nothing -- which looks exactly like a dead button, and is not one.
        hopWait+=delta;
        if(hopWait>.3) {
            hopArmed=false;testClick("go",false);
            if(!pending){saida::Log::error("[World E2E] FAIL the second Go did nothing");testFailed=true;engine.sceneTree().quit();return false;}
            saida::Log::info("[World E2E] teleporting to ",hopLon,", ",hopLat);
        }
    }
    updatePhotography(delta);
    if(smoke&&std::getenv("R1WORLD_PHOTO_SMOKE"))runPhotoSmoke(delta);
    bool key=engine.window().keyDown(GLFW_KEY_ESCAPE);
    if(key&&!wasMenuKey&&!cheatOpen&&!photoResult) {
        if(playerMenu) {
            auto* viewer=ui->findElementById("portfolio-viewer");
            if(viewer&&viewer->IsClassSet("open"))ui->executeJS("closePortfolioViewer();");
            else showPlayerMenu(false);
        }
        else if(photoMode)setPhotoMode(false);
        else if(optionsOpen)showOptions(false);
        else if(menu&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}
        else showMap(true);
    }
    wasMenuKey=key;
    const bool f11Key=engine.window().keyDown(GLFW_KEY_F11);
    if(f11Key&&!wasF11Key){toggleFullscreenMode();}
    wasF11Key=f11Key;
    poll+=delta;hud+=delta;
    if(poll>(pending?.016:.10)){poll=0;if(pending||playing||warming)cost.stream=timed([&]{stream();});}
    cost.parts=timed([&]{uploadParts();});
    updateBuildingLods();
    if(drop.phase==Drop::Zoom&&!menu) {
        // Start each tile's models/textures as soon as it is mounted,
        // even before the player is armed or the other ground tiles arrive.
        // Keep headroom for the frame and both outstanding cooks on small
        // PCs; GPU/node publication keeps streamProps' 2 ms soft budget.
        const auto status=service->status();
        const auto headroom=saida::MemoryProfiler::availableHostBytes();
        constexpr uint64_t reserve=1024ull*1024*1024;
        if(!status.memoryPaused&&(drop.groundReady||!headroom||*headroom>=reserve))
            cost.props=timed([&]{streamProps();});
    }
    if(!playing)cost.warm=timed([&]{warm();});
    updateRadio(delta);
#ifdef SAIDA_ENABLE_XR
    if(vr) {
        const bool permitted=playing&&!menu&&!optionsOpen&&!cheatOpen&&!playerDown&&
            !driving()&&!sailing()&&!piloting()&&!swimming()&&drop.phase==Drop::None&&jumpOffset<3.&&delta>0;
        vrGestures.sample(saida::Time::unscaledElapsed(),permitted);
        if(const char* notice=vrGestures.notice())text("stream-status",notice);
    }
#endif
    // A drop not yet armed: the world under the globe is still the one
    // left, or none; nothing of it plays.
    if(!playing||menu||playerMenu||(drop.phase!=Drop::None&&!drop.armed)){updateRain(delta);return false;}
    return true;
}

void World::Impl::updateWorldScene(float delta) {
    updateInteriors(delta);
    seaTime+=delta;
    {const auto anchor=streamAnchor();ring=orderedNearby(anchor.x,anchor.y);}
    const uint64_t moved=engine.sceneTree().world().indexedNodesTotal()-indexedAtFrame;
    indexedAtFrame=engine.sceneTree().world().indexedNodesTotal();
    churn+=double(moved);churnFrames+=1;dirtyFrames+=moved?1:0;
    // Props already stream above during the flight. Keep distant terrain
    // and moving actors out of the arrival's asset queue: waiting for
    // their unrelated loads used to hold the camera over a ready drop zone.
    if(drop.phase!=Drop::Zoom) {
        cost.props=timed([&]{streamProps();});
        cost.distant=timed([&]{updateFar();updateFarPack();updateFarRelief();});
        cost.world=timed([&]{updateNature();bendGrass();});
    }
    conditionsRead+=delta;
    if(conditionsRead>.5){conditionsRead=0;readConditions();}
    if((farPack.node!=nullptr)!=fogFar||toasterHaze()!=fogToaster)applyWeather();
    // The view reaches the horizon where something is drawn out to it:
    // 4.7 km at eye height, 113 km from a thousand metres, never past
    // the far pack. Elsewhere it stops at the 5 km haze, as it always has.
    if(toasterHaze()) {
        camera->farZ=800.f;
    } else {
        camera->farZ=farPack.node?float(std::clamp(std::sqrt(2.*r1::kRMean*std::max(2.,alt+10.))*1.2+1500.,5000.,r1::kFarPackRadius+5000.)):5000.f;
        // Out to the farthest ring drawn: a summit 200 km off stands above
        // the horizon from a beach, and the haze, not the far plane, hides it.
        if(farRelief.node&&farRelief.node->visible()&&farRelief.reach>0)
            camera->farZ=std::max(camera->farZ,float(farRelief.reach+2000.));
    }
    // The sky takes the haze of the farthest ground drawn, so the horizon
    // is one gradient rather than a silhouette between fogged land and
    // clear sky (engine fogSkyDistance).
    engine.sceneTree().world().settings().fogSkyDistance=camera->farZ;
    updateSnow(delta);
    if(checkStanding)keepStanding();
    updateSnowCover();
    engine.window().setCursorCaptured(true);
}

} // namespace r1::world
