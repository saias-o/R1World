#include "session.hpp"

namespace r1::world {
bool World::Impl::dropFlying() const {return drop.phase!=Drop::None&&drop.armed;}

bool World::Impl::dropGlobe() const {return drop.phase!=Drop::None&&!menu&&drop.clock.seconds<r1::arrival::kFadeSeconds;}

bool World::Impl::dropSteered() const {return drop.phase==Drop::Canopy?drop.canopy.steered:drop.fall.steered;}

bool World::Impl::toasterHaze() const {return toasterMode&&drop.phase==Drop::None;}

bool World::Impl::giSettled() {const auto& s=engine.sceneTree().world().settings();return !s.giEnabled||s.giWarmupProgress>=.99f;}

bool World::Impl::cinematicArrival() const {return !vr&&(!smoke||smokeDive)&&(worldCapture.pngPath.empty()||smokeDive);}

double World::Impl::smooth(double x) {x=std::clamp(x,0.,1.);return x*x*(3-2*x);}

double World::Impl::smoother(double x) {x=std::clamp(x,0.,1.);return x*x*x*(x*(x*6-15)+10);}

glm::dvec2 World::Impl::streamAnchor() const {
    if(pending||warming)return {pickLon,pickLat};
    if(dropFlying()&&metresBetween(lon,lat,drop.goalLon,drop.goalLat)<kDropStreamReach)return {drop.goalLon,drop.goalLat};
    return {lon,lat};
}

glm::dvec2 World::Impl::metresTo(double x,double y,double tx,double ty) const {
    const glm::dvec3 d=glm::transpose(origin.basis)*(ecef(tx,ty,0.)-ecef(x,y,0.));
    return {d.x,-d.z};
}

double World::Impl::zoomForAltitude(double altitude) {
    double low=std::log(.05),high=std::log(1e8);
    for(int i=0;i<80;++i){
        const double mid=.5*(low+high);r1::GlobeView v;v.zoom=std::exp(mid);
        (v.altitude()>altitude?low:high)=mid;
    }
    return std::exp(.5*(low+high));
}

std::string World::Impl::dropLabel() {
    const std::string chosen=value("city-query");
    if(!chosen.empty())return chosen;
    return number(std::abs(pickLat),4)+"° "+(pickLat<0?"S":"N")+"   "+number(std::abs(pickLon),4)+"° "+(pickLon<0?"W":"E");
}

double World::Impl::dropSurface(double x,double y,double from) {return tile(x,y)?surfaceUnder(x,y,from):drop.ground;}

void World::Impl::startDrop() {
    using namespace r1::arrival;
    endDrop();
    drop=Arrival{};
    drop.phase=Drop::Zoom;
    drop.started=std::chrono::steady_clock::now();
    drop.goalLon=pickLon;drop.goalLat=pickLat;
    // Planned from the point picked, before its ground is known: the fall
    // begins south of it, so it flies north, up the globe's picture.
    const auto start=advance(pickLon,pickLat,0,-(kGlideLead+kApproach));
    drop.plannedLon=start.x;drop.plannedLat=start.y;
    drop.plannedAbove=fallHeight(kZoomSeconds)+playerHeight*kPelvis+cameraAbove(kZoomSeconds);
    const GlobePose from=globe?GlobePose{globe->view.lon,globe->view.lat,globe->view.altitude()}
                              :GlobePose{pickLon,pickLat,2.2};
    drop.flight=GlobeFlight(from,{start.x,start.y,drop.plannedAbove/r1::kRMean},kHandoverSpeed,r1::kRMean);
    // First request of this Go, in the click handler: wake the existing
    // cooking workers with the arrival's priorities, not a nine-tile
    // request immediately replaced after workers have already taken it.
    request(drop.goalLon,drop.goalLat);
    drop.place=dropLabel();drop.named=!value("city-query").empty();
    try{prototype(kParachuteModel);}
    catch(const std::exception& e){saida::Log::error("[World arrival] ",e.what(),": he will land without a canopy");}
    menu=false;optionsOpen=false;globeDragging=false;
    if(cheatOpen)closeCheat();
    applyDropUi();
    saida::Log::info("[World arrival] Go: the globe flies ",int(drop.flight.arc()*r1::kRMean/1000.)," km from ",
                     from.lon,", ",from.lat," (",int(from.altitude*r1::kRMean/1000.)," km up) to ",
                     pickLon,", ",pickLat," (",drop.place,")");
}

void World::Impl::endDrop() {
    auto& audio=saida::AudioManager::get();
    for(auto* id:{&windSound,&flutterSound})if(*id!=saida::kInvalidAudioID){audio.stop(*id);*id=saida::kInvalidAudioID;}
    if(canopyNode){canopyNode->queueFree();canopyNode=nullptr;}
    canopyFold=-1;
    if(drop.armed)releaseDropPose();
    if(arrivalUi)arrivalUi->setEnabled(false);
    if(globe){globe->opacity=1;globe->vignette=1;}
    camera->fovDegrees=sceneFov;
    drop.phase=Drop::None;drop.armed=false;
    style("combat","display","block");style("controls","display","block");
}

void World::Impl::armDrop(double x,double y,bool water) {
    using namespace r1::arrival;
    drop.goalLon=x;drop.goalLat=y;drop.water=water;
    drop.ground=water?waterLevel(x,y):height(x,y);
    const auto open=advance(x,y,0,-kGlideLead),start=advance(x,y,0,-(kGlideLead+kApproach));
    drop.openLon=open.x;drop.openLat=open.y;
    // Spawned as Go always spawned him -- the car that brought him waits
    // beside the ground chosen, the Sun lights it -- then lifted to where
    // the fall begins.
    lon=x;lat=y;alt=drop.ground;
    conditions=json::object();conditionsDoc.reset();observedWeather=Weather{};clearRain();
    // The origin halfway down, over the opening point: the whole fall
    // stays within the 350 m it is rebased at (PLAN §3 I1).
    origin=Frame(open.x,open.y,drop.ground+kDeployHeight+.5*(fallHeight(kZoomSeconds)-kDeployHeight));
    placeTiles();moveSun();
    jumpOffset=jumpVelocity=0;followDistance=kOnFootFollow;wasJump=false;
    yaw=0;lookYaw=0;lookIdle=0;pitch=-12;
    dismountPlayerFromBike();
    driving=false;swimming=water;swimTime=0;swimLean=0;swimHeading=yaw;
    clearBoats();clearAircraft();
    if(aiming)setAiming(false);
    player->setEnabled(true);
    player->transform().rotation=glm::quat(1,0,0,0);
    parkCar();
    swimming=false;
    lon=start.x;lat=start.y;alt=drop.ground+fallHeight(drop.clock.seconds);
    // What the globe was flown to, against what is found.
    const double terrain=tile(start.x,start.y)?height(start.x,start.y):drop.ground;
    const double above=alt+playerHeight*kPelvis+cameraAbove(kZoomSeconds)-terrain;
    drop.shiftLon=std::remainder(start.x-drop.plannedLon,360.);drop.shiftLat=start.y-drop.plannedLat;
    drop.shiftAltitude=std::log(std::max(1.,above)/drop.plannedAbove);
    drop.armed=true;drop.aligned=0;
    playing=true;pending=false;warming=false;
    // Picked on the globe, not searched: the tiles say where it is.
    if(!drop.named) {
        std::vector<const r1::MiniMapTile*> tiles;
        for(const auto& [key,entry]:loaded)tiles.push_back(&entry.served->cooked.minimap);
        std::string place=r1::miniMapPlace(tiles,zoneCountries,countryNames,"",x,y);
        // The country can wait for the weather's time zone; the city is enough.
        if(const auto unknown=place.find(" · Unknown country");unknown!=std::string::npos)place.erase(unknown);
        if(place.rfind("Unknown city",0)!=0){drop.place=place;drop.named=true;drop.ui=1e9;}
    }
    request(x,y);
    saida::Log::info("[World arrival] armed ",drop.clock.seconds," s after Go: lands at ",x,", ",y,
                     water?" (water)":"",", ground ",drop.ground," m; the fall begins ",kApproach+kGlideLead,
                     " m south, ",fallHeight(kZoomSeconds)," m up; the globe moves ",
                     int(metresBetween(drop.plannedLon,drop.plannedLat,start.x,start.y))," m and ",
                     int(above-drop.plannedAbove)," m to meet it");
}

bool World::Impl::dropShows(Tile t) const {
    if(t==tileAt(drop.goalLon,drop.goalLat))return true;
    const auto pose=drop.flight.at(r1::arrival::kZoomSeconds);
    r1::GlobeView view;
    view.lon=wrap(pose.lon+(drop.armed?drop.shiftLon:0.));
    view.lat=pose.lat+(drop.armed?drop.shiftLat:0.);
    view.zoom=zoomForAltitude(pose.altitude*std::exp(drop.armed?drop.shiftAltitude:0.));
    const r1::Bounds b=t.gen().bounds();
    int width=1,height=1;engine.window().size(width,height);
    return view.shows(b.west,b.south,b.east,b.north,width,height,globe?globe->viewport:glm::vec4(0,0,1,1));
}

std::string World::Impl::tileReadiness(Tile t,const std::string& sources) const {
    const auto i=loaded.find(t.key());
    if(i==loaded.end())return "tile "+t.key()+sources;
    const auto& r=i->second;
    const size_t parts=r.served?std::any_cast<const PreparedTile&>(r.served->prepared).parts.size():0;
    if(!r.geography||r.nextPart<parts)return "the ground of tile "+t.key();
    for(auto* mesh:r.partMeshes)if(!mesh->loaded())return "the ground of tile "+t.key();
    if(!r.geography->children().empty()&&static_cast<saida::CollisionObjectNode*>(r.geography->children().back().get())->bodyId().IsInvalid())
        return "the collisions of tile "+t.key();
    if(!buildingsReady(r))return "the buildings of tile "+t.key();
    return {};
}

bool World::Impl::dropReady() {
    SAIDA_PROFILE_SCOPE("World/ArrivalReady");
    if(drop.phase!=Drop::Zoom)return true;
    drop.groundReady=false;
    auto missing=[&](std::string why){drop.waitingFor=std::move(why);return false;};
    const auto status=service->status();
    const std::string sources=status.outOfMemory?" -- memory full, retrying"
                             :status.memoryPaused?" -- waiting for free memory"
                             :!status.error.empty()?" -- "+status.error
                             :status.offline?" -- a source is not responding":"";
    if(!drop.armed)return missing("the ground of the drop zone"+sources);
    if(drop.aligned<1)return missing("the picture to meet the ground");
    // The fall begins once kDropTiles of the neighbourhood are ready, the
    // one he lands on among them; the rest cook behind the visible ground.
    const auto want=nearby(drop.goalLon,drop.goalLat);
    const Tile landing=tileAt(drop.goalLon,drop.goalLat);
    size_t streets=0,objects=0,ready=0;
    std::string first;  // why the first tile not ready is not
    for(Tile t:want) {
        const auto why=tileReadiness(t,sources);
        if(!why.empty()) {
            if(t==landing)return missing(why);
            if(first.empty())first=why;
            continue;
        }
        const auto& r=loaded.at(t.key());
        ++ready;
        streets+=r.data.value("provisional",false);
        objects+=r.nextProp<r.props.size();
    }
    const size_t needed=std::min(kDropTiles,want.size());
    if(ready<needed)return missing(std::to_string(ready)+" of "+std::to_string(needed)+" tiles ready; "+first);
    const size_t later=want.size()-ready;
    drop.groundReady=true;
    if(drop.held<kDropExtrasWait&&!status.memoryPaused) {
        // The picture going: the finest imagery over the drop zone, all
        // of it on the GPU -- unless the imagery cannot come (offline).
        if(globe&&globe->imagery.z>=0&&!globe->imageryCovers&&!imageryTiles.unavailable())
            return missing("the satellite picture of the drop zone");
        if(streets&&!status.offline&&status.error.empty())
            return missing(std::to_string(streets)+" tiles' streets and buildings (OpenStreetMap)");
        // Trees, street furniture, textures and models never hold the
        // fall: they come in while he falls.
    }
    drop.waitingFor=streets||objects||later
        ?"fading after "+number(drop.held,0)+" s with "+std::to_string(later)+" tiles of the neighbourhood still cooking, "
         +std::to_string(streets)+" without OSM and "+std::to_string(objects)+" still placing objects":std::string();
    return true;
}

void World::Impl::advanceDrop(double dt) {
    SAIDA_PROFILE_SCOPE("World/Arrival");
    using namespace r1::arrival;
    if(drop.phase==Drop::None||menu)return;
    const bool ready=dropReady();
    drop.ready=ready;
    drop.clock.advance(std::min(dt,.1),ready);
    if(drop.armed)drop.aligned=std::min(1.,drop.aligned+dt/kDropAlignSeconds);
    if(drop.clock.holding()) {
        drop.held+=dt;
        if(drop.held-drop.heldSaid>=5.) {
            drop.heldSaid=drop.held;
            saida::Log::info("[World arrival] the fade waits ",int(drop.held)," s for ",drop.waitingFor);
        }
    }
    if(drop.phase==Drop::Zoom&&drop.clock.seconds>=kZoomSeconds) {
        drop.phase=Drop::Fall;
        request(drop.goalLon,drop.goalLat); // the rest may now cook behind the visible ground
        saida::Log::info("[World arrival] the world shows ",msSince(drop.started)/1000.," s after Go, the zoom having waited ",
                         drop.held," s",drop.waitingFor.empty()?std::string(", everything in"):"; "+drop.waitingFor);
        if(smokeDive)smokeDiveFade();
    }
    if(globe) {
        const double fade=(drop.clock.seconds-kZoomSeconds)/(kFadeSeconds-kZoomSeconds);
        globe->opacity=float(1-smooth(fade));
        globe->vignette=float(1-smooth((drop.clock.seconds-(kZoomSeconds-2.5))/2.5));
    }
    smokeDiveShot();
    hideBehindGlobe();
    if(!drop.armed)return;
    const bool cinematic=drop.clock.seconds<kFadeSeconds;
    if(cinematic!=(written["style:hud:display"]!="block"))applyDropUi();
}

void World::Impl::applyDropUi() {
    using namespace r1::arrival;
    if(drop.phase==Drop::None||menu)return;
    const bool cinematic=drop.clock.seconds<kFadeSeconds;
    style("menu","display","none");style("options-screen","display","none");
    style("hud","display",cinematic?"none":"block");
    style("combat","display","none");style("controls","display","none");
    minimapUi->setEnabled(!cinematic);
    if(performanceUi)performanceUi->setEnabled(performanceDebug&&!cinematic);
    ensureArrivalCanvas();arrivalUi->setEnabled(true);drop.ui=1e9;
    engine.window().setCursorCaptured(true);
}

void World::Impl::ensureArrivalCanvas() {
    if(arrivalUi)return;
    // Its own small texture: the caption and the altimeter change five
    // times a second, and every write to the main interface re-renders all
    // of it (World::Impl::text).
    json doc={{"type","WebCanvasNode"},{"name","Arrival"},{"width",640},{"height",170},
        {"mode",0},{"url",(game/"ui/arrival.html").string()},{"hotReload",false},
        {"interactive",false},{"renderOrder",1003},{"enabled",false},
        {"transform",{{"position",{400,40,0}},{"scale",{1,1,1}},{"rotation",{1,0,0,0}}}}};
    auto canvas=saida::SceneSerializer::nodeFromJson(doc.dump(),engine.resources());
    arrivalUi=dynamic_cast<saida::WebCanvasNode*>(canvas.get());
    if(!arrivalUi||!arrivalUi->lastLoadOk())throw std::runtime_error("Arrival caption failed to load (ui/arrival.html)");
    engine.sceneTree().world().addChild(std::move(canvas));
}

void World::Impl::arrivalText(const std::string& id,const std::string& value) {
    auto& previous=written["drop:"+id];
    if(previous!=value&&arrivalUi->setElementText(id,value))previous=value;
}

void World::Impl::captionDrop(double dt) {
    SAIDA_PROFILE_SCOPE("World/ArrivalCaption");
    using namespace r1::arrival;
    if(drop.phase==Drop::None||menu||!arrivalUi)return;
    const glm::vec2 scale=ui->screenSize()/glm::vec2(ui->width(),ui->height());
    // Over the picture while the globe flies; under the HUD's panels once
    // they show.
    const float top=drop.clock.seconds<kFadeSeconds?40.f:150.f;
    const glm::vec2 at=ui->screenPosition()+glm::vec2(400.f*scale.x,top*scale.y);
    arrivalUi->transform().position={at.x,at.y,0};
    arrivalUi->transform().scale={scale.x,scale.y,1};
    if((drop.ui+=dt)<.2)return;
    drop.ui=0;
    const double tau=drop.clock.seconds;
    const bool imagery=globe&&globe->imagery.z>=0&&tau<kFadeSeconds;
    arrivalText("drop-credit",imagery?"Sentinel-2 cloudless – s2maps.eu by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016)":"");
    arrivalText("drop-place",tau<kFadeSeconds?drop.place:"");
    if(drop.phase==Drop::Zoom) {
        const std::string where=number(std::abs(drop.goalLat),4)+"° "+(drop.goalLat<0?"S":"N")+"  ·  "
                                +number(std::abs(drop.goalLon),4)+"° "+(drop.goalLon<0?"W":"E");
        arrivalText("drop-line",drop.named?where:"");
        arrivalText("drop-help",drop.clock.holding()?"Preparing the drop zone… waiting for "+drop.waitingFor:"");
        return;
    }
    const int altitude=int(std::round(std::max(0.,drop.agl)));
    if(drop.phase==Drop::Fall) {
        const int speed=int(std::round(std::hypot(fallSpeed(tau),drop.fall.speed())*3.6));
        const int left=int(std::ceil(std::max(0.,kDeploySeconds-tau)));
        arrivalText("drop-line","ALTITUDE  "+std::to_string(altitude)+" m   ·   "+std::to_string(speed)+" km/h   ·   parachute in "+std::to_string(left)+" s");
        arrivalText("drop-help","Mouse: look around   ·   WASD / ZQSD: steer the fall");
    } else {
        const auto& c=drop.canopy;
        arrivalText("drop-line","ALTITUDE  "+std::to_string(altitude)+" m   ·   "+std::to_string(int(std::round(c.forward*3.6)))+" km/h   ·   canopy open");
        arrivalText("drop-help","A / D or Q / D: turn   ·   W / Z: dive   ·   S: brake   ·   Mouse: look");
    }
}

void World::Impl::updateDropSounds(double dt) {
    using namespace r1::arrival;
    if(smoke)return;
    auto& audio=saida::AudioManager::get();
    auto loop=[&](saida::AudioID& id,const char* path,float volume){
        if(volume<=.002f) {
            if(id!=saida::kInvalidAudioID&&audio.isPlaying(id))audio.pause(id);
            return;
        }
        if(id==saida::kInvalidAudioID) {
            saida::AudioSettings settings;settings.loop=true;settings.volume=0;
            id=audio.play(path,settings);
            if(id==saida::kInvalidAudioID) {
                if(!dropSoundSaid)saida::Log::error("[World arrival] ",path," could not play (python -m r1.arrival_sounds): the fall is silent");
                dropSoundSaid=true;return;
            }
        } else if(!audio.isPlaying(id))audio.resume(id);
        audio.setVolume(id,volume);
    };
    const double tau=drop.clock.seconds;
    // The wind rises as the globe comes down, roars at terminal speed and
    // drops away under the canopy; then the fabric's flutter.
    double wind=0,flutter=0;
    if(drop.phase==Drop::Zoom)wind=.5*smooth((tau-(kZoomSeconds-3.))/3.);
    else if(drop.phase==Drop::Fall) {
        const double v=std::hypot(fallSpeed(tau),drop.fall.speed())/kTerminal;
        wind=.75*std::min(1.2,v*v);
    } else if(drop.phase==Drop::Canopy) {
        wind=.75*std::pow(std::clamp(drop.canopy.sink/kTerminal,0.,1.),2.)*(1-smooth(drop.canopy.age/1.5));
        flutter=.45*drop.canopy.inflation();
    }
    (void)dt;
    loop(windSound,kWindSound,float(wind));
    loop(flutterSound,kFlutterSound,float(flutter));
}

void World::Impl::flyDrop(double dt,double f,double r) {
    SAIDA_PROFILE_SCOPE("World/ArrivalFlight");
    using namespace r1::arrival;
    const double tau=drop.clock.seconds;
    const Steer in{tau>=kZoomSeconds?f:0.,tau>=kZoomSeconds?r:0.};
    if(drop.phase==Drop::Canopy)flyCanopy(dt,in);
    else {
        if(drop.phase==Drop::Fall) {
            // The arrival's own seconds: the fall keeps the clock's pace.
            const double step=std::min(.1,dt*drop.clock.rate);
            if(!drop.fall.steered&&touched(in)){yaw=wrap(drop.fall.facing(yaw)+drop.lookYaw);drop.lookYaw=0;
                saida::Log::info("[World arrival] the player takes the fall over ",tau," s after Go, ",int(drop.agl)," m up");}
            const auto goal=metresTo(lon,lat,drop.openLon,drop.openLat);
            drop.fall.step(step,in,yaw,goal.x,goal.y,kDeploySeconds-tau);
            const auto next=onward(lon,lat,drop.fall.east*step,drop.fall.north*step);
            if(tile(next.x,next.y)){lon=next.x;lat=next.y;}
            else {drop.fall.east=drop.fall.north=0;text("stream-status","More data is on its way… the fall is held at the edge of the area.");}
        }
        alt=drop.ground+fallHeight(tau);
        drop.agl=alt+playerHeight*kPelvis-dropSurface(lon,lat,alt+playerHeight*kPelvis);
        // Over higher ground than the ground chosen -- a hill, a tower --
        // it opens early, higher the faster he still falls, so the wing
        // always has the height it needs to bite (r1test Arrival).
        if(drop.phase==Drop::Fall&&(tau>=kDeploySeconds||drop.agl<=openingHeight(fallSpeed(tau))))openCanopy();
        feet->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
    }
    const auto anchor=streamAnchor();request(anchor.x,anchor.y);
    if(drop.phase==Drop::None)return;  // landed this frame
    poseDrop(dt,in);
    placeCanopy(dt);
}

void World::Impl::openCanopy() {
    using namespace r1::arrival;
    const double tau=drop.clock.seconds;
    const double facing=drop.fall.facing(yaw);
    // Along where he faces, what he flew across at.
    const double h=facing*rad,along=std::max(0.,drop.fall.east*std::sin(h)+drop.fall.north*std::cos(h));
    drop.canopy.open(facing,along,fallSpeed(tau));
    // A player who flew the fall flies the canopy.
    drop.canopy.steered=drop.fall.steered;
    if(!drop.canopy.steered){yaw=facing;}
    drop.phase=Drop::Canopy;drop.openedAt=tau;drop.openedAbove=drop.agl;drop.upright=0;drop.canopyCamera=0;
    drop.openedOverGoal=alt-drop.ground;
    saida::AudioSettings opening;opening.loop=false;opening.volume=.9f;
    if(!smoke&&saida::AudioManager::get().play(kOpeningSound,opening)==saida::kInvalidAudioID&&!dropSoundSaid) {
        dropSoundSaid=true;saida::Log::error("[World arrival] ",kOpeningSound," could not play (python -m r1.arrival_sounds)");
    }
    saida::Log::info("[World arrival] canopy open ",tau," s after Go, ",drop.openedOverGoal," m above the ground chosen and ",
                     drop.agl," m above what is under him, ",
                     int(metresBetween(lon,lat,drop.goalLon,drop.goalLat))," m from the ground chosen",
                     drop.canopy.steered?" (steered)":" (autopilot)");
}

void World::Impl::flyCanopy(double dt,const r1::arrival::Steer& in) {
    auto& c=drop.canopy;
    if(!c.steered&&r1::arrival::touched(in)) {
        yaw=wrap(c.heading+drop.lookYaw);drop.lookYaw=0;
        saida::Log::info("[World arrival] the player takes the canopy over, ",int(drop.agl)," m up");
    }
    const auto goal=metresTo(lon,lat,drop.goalLon,drop.goalLat);
    const double below=dropSurface(lon,lat,alt+.5);
    drop.agl=alt-below;
    const double wanted=c.step(dt,in,yaw,goal.x,goal.y,drop.agl);
    drop.canopyWanted=wanted;
    if(c.steered)yaw=wanted;
    // His feet are a body: the canopy flies him down onto what is there,
    // a roof as well as a street, and he slides along a wall he meets.
    auto next=onward(lon,lat,c.east()*dt,c.north()*dt);
    if(!tile(next.x,next.y)){next={lon,lat};text("stream-status","More data is on its way… the canopy is held at the edge of the area.");}
    const glm::vec3 from(origin.local(ecef(lon,lat,alt+.06))),to(origin.local(ecef(next.x,next.y,alt-c.sink*dt+.06)));
    feet->transform().position=from;
    const glm::vec3 end=feet->moveAndSlide((to-from)/float(dt),float(dt));
    const auto moved=onward(lon,lat,end.x-from.x,-(end.z-from.z));
    lon=moved.x;lat=moved.y;alt+=end.y-from.y;
    const bool wet=tile(lon,lat)&&deepWater(lon,lat)&&alt<=waterLevel(lon,lat);
    const double ground=tile(lon,lat)?height(lon,lat,alt):below;
    if(wet||feet->isOnFloor()||alt<=ground+.02)landDrop(wet,ground);
}

void World::Impl::landDrop(bool wet,double ground) {
    using namespace r1::arrival;
    const double sink=drop.canopy.sink,tau=drop.clock.seconds;
    const double away=metresBetween(lon,lat,drop.goalLon,drop.goalLat);
    cameraFrom=CameraPose{camera->transform().position,camera->transform().rotation,camera->fovDegrees};cameraHandoff=0;
    // He looks on where the camera looked; on foot the camera stands
    // behind him again.
    yaw=wrap(dropSteered()?yaw:drop.canopy.heading+drop.lookYaw);
    pitch=-12;lookYaw=0;lookIdle=0;
    followDistance=std::min(kOnFootFollow,double(glm::length(camera->transform().position-player->transform().position)));
    jumpOffset=jumpVelocity=0;wasJump=false;
    if(wet){swimming=true;swimTime=0;swimLean=0;swimHeading=yaw;alt=waterLevel(lon,lat);}
    else alt=std::max(alt,ground);
    // A landing is not a fall (World::Impl::landed).
    arrivalClock=fightClock;
    releaseDropPose();
    player->transform().rotation=glm::angleAxis(float(-drop.canopy.heading*rad),glm::vec3(0,1,0));
    player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
    playPlayerClip("idle");
    // The wing comes down in front of him and settles.
    if(canopyNode) {
        canopyFold=0;canopyRest=canopyNode->transform().position;canopyTurn=canopyNode->transform().rotation;
        const auto ahead=onward(lon,lat,std::sin(drop.canopy.heading*rad)*3.,std::cos(drop.canopy.heading*rad)*3.);
        canopyGround=glm::vec3(origin.local(ecef(ahead.x,ahead.y,tile(ahead.x,ahead.y)?height(ahead.x,ahead.y,alt):alt)));
    }
    auto& audio=saida::AudioManager::get();
    for(auto* id:{&windSound,&flutterSound})if(*id!=saida::kInvalidAudioID){audio.stop(*id);*id=saida::kInvalidAudioID;}
    const double seconds=msSince(drop.started)/1000.;
    saida::Log::info("[World arrival] landed ",seconds," s after Go (",tau-drop.openedAt," s under the canopy) at ",lon,", ",lat,
                     wet?" in the water":"",", ",away," m from the ground chosen, at ",sink," m/s; frames: ",drop.smoothness());
    const bool steered=dropSteered();
    drop.phase=Drop::None;drop.armed=false;
    applyWeather();  // on his feet: toaster mode's fog is back this frame
    if(arrivalUi)arrivalUi->setEnabled(false);
    style("combat","display","block");style("controls","display","block");
    text("stream-status",wet?"In the water — swim for the shore.":"Landed. Welcome to "+drop.place+".");
    request(lon,lat);
    if(smokeDive)smokeDiveLanded(sink,away,steered,tau);
}

void World::Impl::releaseDropPose() {
    for(auto* arm:playerArms)if(arm){arm->release();arm->clearTipRotation();arm->clearPole();}
    for(auto* leg:playerBikeLegs)if(leg){leg->release();leg->clearTipRotation();leg->clearPole();}
    if(playerBikeLook)playerBikeLook->release();
}

void World::Impl::poseDrop(double dt,const r1::arrival::Steer& in) {
    using namespace r1::arrival;
    const bool canopy=drop.phase==Drop::Canopy;
    const double facing=canopy?drop.canopy.heading:drop.fall.facing(yaw);
    if(canopy)drop.upright=std::min(1.,drop.upright+dt/.9);
    const double u=smooth(drop.upright);
    // In free fall he lies on the air, belly to earth, his head along the
    // way he flies, head lower the faster he tracks, banked into a slide;
    // the canopy's opening swings him upright under it.
    const double h=facing*rad;
    const double forward=drop.fall.east*std::sin(h)+drop.fall.north*std::cos(h);
    const double side=drop.fall.east*std::cos(h)-drop.fall.north*std::sin(h);
    const double track=std::clamp(forward/Freefall::kForward,0.,1.);
    const double slide=std::clamp(side/Freefall::kSide,-1.,1.)*18.;
    const glm::quat turn=glm::angleAxis(float(-h),glm::vec3(0,1,0));
    const glm::quat lying=turn*glm::angleAxis(float(-slide*rad),glm::vec3(0,0,1))
                         *glm::angleAxis(float((-90.+8.-14.*track)*rad),glm::vec3(1,0,0));
    const glm::quat hanging=turn*glm::angleAxis(float(-drop.canopy.bank(drop.canopyWanted)*rad),glm::vec3(0,0,1));
    const glm::quat body=glm::normalize(glm::slerp(lying,hanging,float(u)));
    const glm::vec3 pelvis(0,float(playerHeight*kPelvis),0);
    player->transform().rotation=body;
    player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+playerHeight*kPelvis)))-body*pelvis;
    playPlayerClip(u>.5?"idle":"jump");
    // The limbs, in the rig's own space (+Z his front, +Y up): the box
    // position of free fall -- upper arms out, forearms up beside the head,
    // knees bent, feet to the sky -- then, hanging, hands up on the
    // toggles and legs down.
    auto* animator=animators.front();
    if(!animator||!animator->rig())return;
    const auto& pose=animator->globalPose().globalMatrices;
    const saida::Rig& rig=*animator->rig();
    const int shoulders[2]={rig.findBoneIndex("Bip01 L UpperArm"),rig.findBoneIndex("Bip01 R UpperArm")};
    const int hips[2]={rig.findBoneIndex("Bip01 L Thigh"),rig.findBoneIndex("Bip01 R Thigh")};
    if(shoulders[0]<0||shoulders[1]<0||hips[0]<0||hips[1]<0||pose.size()<rig.boneCount())return;
    const glm::vec3 Y(0,1,0),Z(0,0,1);
    const float hang=float(u),brake=float(canopy&&drop.canopy.steered?std::max(0.,-in.forward):0.);
    // The toggle of the side he turns to is pulled down (k: 0 left, 1 right).
    const float turning=float(canopy?std::clamp(drop.canopy.bank(drop.canopyWanted)/25.,-1.,1.):0.);
    for(int k=0;k<2;++k) {
        const glm::vec3 shoulder(pose[size_t(shoulders[k])][3]),hip(pose[size_t(hips[k])][3]);
        // Out is away from his middle, whichever side the rig puts left.
        const glm::vec3 out(shoulder.x>=0?1.f:-1.f,0,0);
        const float pull=brake+std::max(0.f,k?turning:-turning);
        const glm::vec3 hand=glm::mix(shoulder+out*.30f+Y*.22f-Z*.10f,shoulder+out*.12f+Y*(.42f-.34f*std::min(1.f,pull))+Z*.08f,hang);
        const glm::vec3 elbow=glm::mix(shoulder+out*.55f-Y*.12f-Z*.18f,shoulder+out*.45f+Y*.05f-Z*.10f,hang);
        const glm::vec3 foot=glm::mix(hip+out*.20f-Y*.48f-Z*.40f,hip+out*.07f-Y*.85f+Z*.12f,hang);
        const glm::vec3 knee=glm::mix(hip+out*.28f-Y*.35f+Z*.55f,hip+out*.06f-Y*.40f+Z*.60f,hang);
        if(auto* arm=playerArms[k]){arm->clearTipRotation();arm->reach(hand);arm->setPole(elbow);}
        if(auto* leg=playerBikeLegs[k]){leg->clearTipRotation();leg->reach(foot);leg->setPole(knee);}
    }
    // Falling, he lifts his head to see where he flies.
    if(playerBikeLook)playerBikeLook->lookAt(glm::mix(glm::vec3(0,100,40),glm::vec3(0,1.5f,100),hang));
}

void World::Impl::placeCanopy(double dt) {
    using namespace r1::arrival;
    if(drop.phase!=Drop::Canopy)return;
    if(!canopyNode) {
        saida::Node* source=nullptr;
        try{source=prototype(kParachuteModel);}
        catch(const std::exception& e){if(!canopyMissingSaid)saida::Log::error("[World arrival] ",e.what());canopyMissingSaid=true;return;}
        if(!source) {
            if(!canopyMissingSaid)saida::Log::warn("[World arrival] the parachute model is still loading: the canopy shows when it is in");
            canopyMissingSaid=true;return;
        }
        canopyNode=engine.sceneTree().world().addChild(clonePlant(*source));
        canopyNode->setName("parachute");
    }
    (void)dt;
    const auto& c=drop.canopy;
    const double age=c.age,p=c.inflation();
    // A wing snatched open overshoots a little and settles.
    const double snap=1+.06*std::sin(age*9.)*std::exp(-age*2.5)*smooth((age-.6)/.4);
    const glm::vec3 scale=float(kParachuteScale)*glm::vec3(float((.12+.88*p)*snap),float(.15+.85*smooth(age/.7)),float(.25+.75*p));
    const glm::quat turn=glm::angleAxis(float(-c.heading*rad),glm::vec3(0,1,0))
                        *glm::angleAxis(float(-c.bank(drop.canopyWanted)*rad),glm::vec3(0,0,1))
                        // The wing trails behind him until it bites.
                        *glm::angleAxis(float((1-p)*35.*rad),glm::vec3(1,0,0))
                        // The model's front is glTF's +Z; his is -Z.
                        *glm::angleAxis(float(r1::arrival::kPi),glm::vec3(0,1,0));
    const glm::vec3 harness=player->transform().position+player->transform().rotation*glm::vec3(0,float(playerHeight*kShoulders),0);
    canopyNode->transform().position=harness;
    canopyNode->transform().rotation=turn;
    canopyNode->transform().scale=scale;
}

void World::Impl::foldCanopy(double dt) {
    if(canopyFold<0||!canopyNode)return;
    canopyFold+=dt;
    const double s=smooth(canopyFold/2.4);
    canopyNode->transform().position=glm::mix(canopyRest,canopyGround,float(s));
    canopyNode->transform().rotation=glm::slerp(canopyTurn,canopyTurn*glm::angleAxis(float(-80.*rad),glm::vec3(1,0,0)),float(s));
    canopyNode->transform().scale=float(kParachuteScale)*glm::vec3(float(1-.2*s),float(1-.92*s),float(1-.35*s));
    if(canopyFold>5.){canopyNode->queueFree();canopyNode=nullptr;canopyFold=-1;}
}

void World::Impl::placeDropCamera(double dt) {
    using namespace r1::arrival;
    const double tau=drop.clock.seconds;
    const glm::vec3 body(origin.local(ecef(lon,lat,alt+playerHeight*kPelvis)));
    // The globe's own camera, as the world draws it: straight down over
    // him, north up, the globe's lens and its off-centre framing (the
    // globe's centre sits at GlobeView::centreY of the document).
    CameraPose overhead;
    {
        const glm::vec4 vp=globe?globe->viewport:glm::vec4(0,0,1,1);
        const double vh=std::max(.05,double(vp.w)),middle=(.5-double(vp.y))/vh;
        const double up=(r1::GlobeView::centreY-middle)*2*r1::GlobeView::tangent;
        overhead.position=body+glm::vec3(0,float(cameraAbove(tau)),0);
        overhead.rotation=glm::angleAxis(float(-90*rad),glm::vec3(1,0,0))*glm::angleAxis(float(std::atan(up)),glm::vec3(1,0,0));
        overhead.fov=float(2*std::atan(r1::GlobeView::tangent/vh)/rad);
    }
    const double facing=drop.phase==Drop::Canopy?drop.canopy.heading:drop.fall.facing(yaw);
    const double look=wrap(dropSteered()?yaw:facing+drop.lookYaw);
    auto chase=[&](double below,double distance,double rise,double lead,float fov){
        // `below`: degrees under the horizon the camera looks down at him.
        CameraPose eye;
        const glm::quat turn=glm::angleAxis(float(-look*rad),glm::vec3(0,1,0));
        const glm::vec3 back=turn*glm::vec3(0,0,1);
        const glm::vec3 target=body+glm::vec3(0,float(rise),0)-back*float(lead);
        eye.position=target+back*float(distance*std::cos(below*rad))+glm::vec3(0,float(distance*std::sin(below*rad)),0);
        eye.rotation=glm::quatLookAt(glm::normalize(target-eye.position),glm::vec3(0,1,0));
        eye.fov=fov;return eye;
    };
    auto blend=[](const CameraPose& a,const CameraPose& b,double s){
        CameraPose out;out.position=glm::mix(a.position,b.position,float(s));
        out.rotation=glm::normalize(glm::slerp(a.rotation,b.rotation,float(s)));
        out.fov=glm::mix(a.fov,b.fov,float(s));return out;
    };
    // Steeply over him high up, flatter as the ground comes, as a
    // skydiving camera flies; the mouse tilts it.
    const double steep=40.+22.*std::clamp((drop.agl-kDeployHeight)/500.,0.,1.);
    const auto falling=chase(std::clamp(steep-drop.lookPitch,10.,85.),7.,0.,2.,64.f);
    CameraPose eye;
    if(drop.phase==Drop::Canopy) {
        drop.canopyCamera=std::min(1.,drop.canopyCamera+dt/1.5);
        eye=blend(falling,chase(std::clamp(4.-drop.lookPitch,-25.,60.),7.5,1.3,0.,64.f),smoother(drop.canopyCamera));
    } else eye=blend(overhead,falling,smoother((tau-kFadeSeconds)/(kSwoopSeconds-kFadeSeconds)));
    // The air buffets the camera at terminal speed.
    if(drop.phase==Drop::Fall&&tau>kFadeSeconds) {
        const double t=msSince(drop.started)/1000.,a=.25*rad*smooth((tau-kFadeSeconds)/2.);
        eye.rotation=eye.rotation*glm::angleAxis(float(a*std::sin(t*7.3)),glm::vec3(1,0,0))*glm::angleAxis(float(a*std::sin(t*11.1+1.)),glm::vec3(0,1,0));
    }
    // Never inside a wall or under the ground: it comes in along its line
    // to him, as the camera on foot does.
    if(auto* physics=engine.sceneTree().world().physics()) {
        const glm::vec3 line=eye.position-body;const float d=glm::length(line);
        if(d>.3f){const auto hit=physics->raycast(body,line/d,d,obstacleFilter());if(hit.hit)eye.position=body+line/d*std::max(.4f,hit.distance-.3f);}
    }
    {
        const glm::dvec3 world=origin.origin+origin.basis*glm::dvec3(eye.position);
        const auto geo=r1::ecefToGeodetic(world.x,world.y,world.z);
        if(tile(geo.x,geo.y)) {
            const double floor=height(geo.x,geo.y)+.4;
            if(geo.z<floor)eye.position+=glm::vec3(0,float(floor-geo.z),0);
        }
    }
    camera->transform().position=eye.position;
    camera->transform().rotation=eye.rotation;
    camera->fovDegrees=eye.fov;
    hideBehindGlobe();
    // While the globe fades it is this camera, straight down: the
    // picture going is the picture coming.
    if(globe&&dropGlobe()&&drop.clock.seconds>=kZoomSeconds)flyGlobe();
}

bool World::Impl::behindGlobe() const {return drop.phase==Drop::Zoom&&(drop.clock.seconds<r1::arrival::kZoomSeconds-.5||!drop.ready);}

void World::Impl::hideBehindGlobe() {
    if(!behindGlobe())return;
    camera->farZ=1.f;
    if(farRelief.node&&farRelief.node->visible())farRelief.node->setVisible(false);
}

void World::Impl::flyGlobe() {
    using namespace r1::arrival;
    auto& view=globe->view;
    double gLon=0,gLat=0,gAltitude=1;
    if(drop.clock.seconds<kZoomSeconds||!drop.armed) {
        const auto pose=drop.flight.at(drop.clock.seconds);
        const double a=drop.armed?smooth(drop.aligned):0.;
        gLon=wrap(pose.lon+a*drop.shiftLon);gLat=pose.lat+a*drop.shiftLat;
        gAltitude=pose.altitude*std::exp(a*drop.shiftAltitude);
    } else {
        const glm::dvec3 world=origin.origin+origin.basis*glm::dvec3(camera->transform().position);
        const auto eye=r1::ecefToGeodetic(world.x,world.y,world.z);
        const double terrain=tile(eye.x,eye.y)?height(eye.x,eye.y):drop.ground;
        gLon=eye.x;gLat=eye.y;gAltitude=std::max(1.,eye.z-terrain)/r1::kRMean;
    }
    view.lon=gLon;view.lat=gLat;view.zoom=zoomForAltitude(gAltitude);
    // The map, opened from the air, opens where he is.
    globeTarget.lon=gLon;globeTarget.lat=gLat;zoom=std::clamp(view.zoom,1.,131072.);
}

void World::Impl::smokeDiveFade() {
    // The fade began with the landing tile and kDropTiles of the
    // neighbourhood cooked, and the world's camera is the globe's.
    const auto want=nearby(drop.goalLon,drop.goalLat);
    const Tile landing=tileAt(drop.goalLon,drop.goalLat);
    size_t ready=0;
    for(Tile t:want) {
        const bool cooked=tileReadiness(t,"").empty();
        ready+=cooked;
        if(t==landing&&!cooked){saida::Log::error("[World E2E] FAIL the fade began without the landing tile ",t.key());testFailed=true;engine.sceneTree().quit();return;}
    }
    if(ready<std::min(kDropTiles,want.size())){saida::Log::error("[World E2E] FAIL the fade began with ",ready," tiles ready of ",kDropTiles);testFailed=true;engine.sceneTree().quit();return;}
    if(fogToaster||camera->farZ<=800.f){saida::Log::error("[World E2E] FAIL the fade began in toaster mode's fog (far plane ",camera->farZ," m)");testFailed=true;engine.sceneTree().quit();return;}
    smokeDiveFaded=true;smokeDiveHealth=health.hp;
    saida::Log::info("[World E2E] PASS the fade found the drop zone cooked: the landing tile and ",ready," of ",want.size(),
                     " tiles of the neighbourhood with ground, buildings and collisions");
}

void World::Impl::runSmokeDive() {
    using namespace r1::arrival;
    if(!smokeDive||drop.phase==Drop::None)return;
    const double tau=drop.clock.seconds;
    if(drop.phase==Drop::Fall&&tau>=kZoomSeconds+.5&&tau<kZoomSeconds+.6&&globe) {
        // The globe draws what the world's camera sees.
        const glm::dvec3 world=origin.origin+origin.basis*glm::dvec3(camera->transform().position);
        const auto eye=r1::ecefToGeodetic(world.x,world.y,world.z);
        const double off=metresBetween(eye.x,eye.y,globe->view.lon,globe->view.lat);
        if(off>1.){saida::Log::error("[World E2E] FAIL the fading globe is ",off," m off the world's camera");testFailed=true;engine.sceneTree().quit();return;}
    }
    // The autopilot flies him north: two seconds sliding right, east,
    // are the player's and nobody else's.
    if(drop.phase==Drop::Fall&&tau>=13.&&!smokeDiveSteerSet){smokeDiveSteerFrom={lon,lat};smokeDiveSteerSet=true;}
    if(drop.phase!=Drop::Zoom&&tau>=15.&&smokeDiveSteerSet&&smokeDiveSlid<0)
        smokeDiveSlid=metresTo(smokeDiveSteerFrom.x,smokeDiveSteerFrom.y,lon,lat).x;
}

void World::Impl::smokeDiveShot() {
    const char* png=std::getenv("R1WORLD_DIVE_SHOT");
    if(!smokeDive||!png||smokeDiveShotQueued||drop.phase==Drop::None)return;
    const char* at=std::getenv("R1WORLD_DIVE_SHOT_AT");
    const double when=at?std::atof(at):r1::arrival::kZoomSeconds+1.;
    if(drop.clock.seconds<when)return;
    saida::CaptureRequest shot;shot.pngPath=png;shot.frame=1;
    // The instant asked for, whatever is still loading: the arrival is
    // the moment the world loads.
    shot.waitForAssets=false;
    smokeDiveShotQueued=true;
    saida::Log::info("[World E2E] dive picture ",drop.clock.seconds," s after Go, ",int(drop.agl)," m up (",png,")");
    engine.captureFrameThenExit(shot);
}

void World::Impl::smokeDiveKeys(double& f,double& r) {
    f=r=0;
    if(drop.phase==Drop::Fall&&drop.clock.seconds>=13.&&drop.clock.seconds<15.)r=1;
}

void World::Impl::smokeDiveLanded(double sink,double away,bool steered,double tau) {
    using namespace r1::arrival;
    auto fail=[&](const std::string& why){saida::Log::error("[World E2E] FAIL dive: ",why);testFailed=true;engine.sceneTree().quit();};
    if(!smokeDiveFaded)return fail("landed without the fade having been checked");
    if(!steered)return fail("the keys did not take the fall over");
    if(smokeDiveSlid<8.)return fail("two seconds sliding right moved him "+number(smokeDiveSlid,1)+" m east");
    const bool onTime=std::abs(drop.openedAt-kDeploySeconds)<.1&&std::abs(drop.openedOverGoal-kDeployHeight)<.5;
    const bool early=drop.openedAt<kDeploySeconds;
    if(!onTime&&!early)return fail("the canopy opened "+number(drop.openedAt,2)+" s after Go, "+number(drop.openedOverGoal,1)+" m above the ground chosen");
    if(tau-drop.openedAt<3.)return fail("only "+number(tau-drop.openedAt,1)+" s under the canopy");
    if(r1::combat::fallDamage(sink)>0||health.hp<smokeDiveHealth)return fail("the landing hurt: "+number(sink,1)+" m/s");
    if(toasterMode&&!fogToaster)return fail("landed in toaster mode without its fog back");
    saida::Log::info("[World E2E] PASS dive: canopy open ",drop.openedAt," s after Go, ",drop.openedOverGoal," m above the ground chosen",
                     onTime?"":" (early, over higher ground)",", slid ",smokeDiveSlid," m east under the keys, landed at ",sink," m/s, ",away,
                     " m from the ground chosen; frames: ",drop.smoothness());
    engine.sceneTree().quit();
}

} // namespace r1::world
