#include "session.hpp"

namespace r1::world {
bool World::Impl::smokeZoom() {
    auto fail=[&](const char* why){
        saida::Log::error("[World menu E2E] FAIL zoom: ",why," (zoom=",zoom,")");
        testFailed=true;engine.sceneTree().quit();return false;
    };
    const double previousZoom=zoom;
    zoomMap(.25);
    if(zoom!=1)return fail("zoom must keep the whole-Earth lower bound");
    zoomMap(1e9);
    if(zoom!=131072.)return fail("zoom must keep the street-level upper bound");
    // Whole Earth lives on the Destination tab: press it where a player can.
    showOptions(true);ui->executeJS("showSettingsTab('destination');");
    testClick("reset-map");
    ui->executeJS("showSettingsTab('time');");showOptions(false);
    if(zoom!=1)return fail("reset must restore the whole Earth");
    zoomMap(previousZoom);
    saida::Log::info("[World menu E2E] PASS zoom: bounds and reset");
    return true;
}

bool World::Impl::smokeOptions() {
    const bool savedToaster=toasterMode;
    const int savedGIQuality=giQuality;
    const auto savedTime=forcedMinutes;
    const auto savedWeather=forcedWeather;
    auto fail=[&](const std::string& why){
        saida::Log::error("[World E2E] FAIL options: ",why);testFailed=true;engine.sceneTree().quit();return false;
    };
    testClick("options",false);
    if(!optionsOpen)return fail("the Options press did not open the screen");
    field("forced-time","14h30");testClick("forced-apply",false);
    if(forcedMinutes!=14*60+30)return fail("14h30 was not read as 14:30");
    double at=0;bool known=false;const int offset=utcOffset(known);
    if(!gameTime(at)||std::abs(std::fmod(at+offset,86400.)-(14*3600.+1800.))>1.)
        return fail("the Sun is not at 14:30 local (gameTime "+number(at,0)+", offset "+std::to_string(offset)+")");
    field("forced-time","25:00");testClick("forced-apply",false);
    if(forcedMinutes!=14*60+30)return fail("25:00 replaced the forced hour instead of being refused");
    testClick("forced-preset-12",false);
    if(forcedMinutes!=12*60)return fail("the 12:00 preset did not apply");
    testClick("forced-clear",false);
    if(forcedMinutes||forcedSent)return fail("Real time did not give the Sun back to the clock");
    // From the menu no tile may be resident yet: forcing the weather there
    // once threw "Missing terrain" and closed the game.
    ui->executeJS("showSettingsTab('weather');");
    testClick("weather-clear",false);
    if(forcedWeather!=r1::WeatherOverride::Clear)return fail("Clear sky did not force clear weather");
    testClick("weather-real",false);
    if(forcedWeather!=r1::WeatherOverride::Real)return fail("Real weather did not restore the forecast");
    ui->executeJS("showSettingsTab('graphics');");
    testClick("toaster-on",false);
    if(!toasterMode)return fail("Mode grille-pain did not enable toaster mode");
    if(engine.sceneTree().world().settings().giEnabled)return fail("Toaster mode left GI enabled");
    testClick("toaster-off",false);
    if(toasterMode)return fail("Standard quality did not disable toaster mode");
    for(int q=-1;q<=4;++q) {
        testClick(std::string("gi-quality-")+giQualityKeys[q+1],false);
        const auto& settings=engine.sceneTree().world().settings();
        if(giQuality!=q || settings.giEnabled!=(q>=0) || int(settings.giQuality)!=std::max(0,q))
            return fail("GI quality button did not apply its profile");
    }
    testClick("toaster-on",false);testClick("toaster-off",false);
    if(giQuality!=4 || engine.sceneTree().world().settings().giQuality!=saida::GIQuality::Ultra)
        return fail("Toaster toggle lost the selected GI quality");
    setGIQuality(savedGIQuality);
    testClick("fullscreen",false);
    if(!engine.window().isFullscreen())return fail("Full screen button did not enable full screen");
    testClick("fullscreen-off",false);
    if(engine.window().isFullscreen())return fail("Windowed button did not disable full screen");
    // Go from the Destination tab refuses what is not a coordinate and
    // stays open to say which field and why (the valid trip is the
    // smoke's own Go below). South and west are typed as players type
    // them, key by key into the focused field: a typographic minus and a
    // decimal comma, while M -- the AZERTY comma -- leaves the menu alone.
    {
        const std::string latitude=value("latitude"),longitude=value("longitude");
        const bool wasPending=pending;
        ui->executeJS("showSettingsTab('destination');");
        ui->documentElement()->GetContext()->Update();
        auto type=[&](const char* id,std::u32string_view keys){
            auto* input=ui->findElementById(id);
            if(!input)return false;
            field(id,std::string());input->Focus(true);
            for(char32_t key:keys)ui->fireTextInput(uint32_t(key));
            return true;
        };
        if(!type("latitude",U"−33,8688")||!typing())return fail("the latitude field did not take the keyboard");
        if(!type("longitude",U" 151.2093")||!typing())return fail("the longitude field did not take the keyboard");
        if(const auto sydney=r1::readDestination(value("latitude"),value("longitude"));
           !sydney||sydney.lat!=-33.8688||sydney.lon!=151.2093)
            return fail("typed Sydney read as '"+value("latitude")+"', '"+value("longitude")+"': "+sydney.refusal);
        type("latitude",U"-91");type("longitude",U"-73.9857");
        ui->findElementById("longitude")->Blur();
        testClick("destination-go",false);
        if(!optionsOpen||pending!=wasPending)return fail("Go accepted latitude -91 or left the settings");
        if(auto* status=ui->findElementById("destination-status");!status||status->GetInnerRML().find("Latitude -91")==std::string::npos)
            return fail("Go refused latitude -91 without naming the field and the range");
        field("latitude",latitude);field("longitude",longitude);
        ui->executeJS("showSettingsTab('time');");
    }
    testClick("options-back",false);
    if(optionsOpen)return fail("Retour did not close the screen");
    saida::Log::info("[World E2E] options passed (forced hour reaches the Sun, bad hour refused, real time restored, weather forced from the menu, toaster mode toggled, full screen toggled, negative coordinates typed, bad destination refused in place)");
    forceTime(savedTime);
    forceWeather(savedWeather);
    // Testing both buttons must not silently change the measured preset.
    bool requestedToaster=savedToaster;
    if(const char* quality=std::getenv("R1WORLD_SMOKE_QUALITY")) {
        const std::string preset=quality;
        if(preset!="standard"&&preset!="toaster")return fail("R1WORLD_SMOKE_QUALITY must be standard or toaster");
        requestedToaster=preset=="toaster";
    }
    setToasterMode(requestedToaster);
    // A street by day for the walk and the fight: R1WORLD_SMOKE_HOUR=HH:MM
    // forces the local hour as the Options screen would. Night empties it.
    if(const char* hour=std::getenv("R1WORLD_SMOKE_HOUR")) {
        const auto minutes=r1::parseClockTime(hour);
        if(!minutes)return fail(std::string("R1WORLD_SMOKE_HOUR '")+hour+"' is not HH:MM");
        forceTime(minutes);
    }
    return true;
}

void World::Impl::startSmokeDrive() {
    smokeDroveOnce=true;
    if(!carParked) {
        // The car not being there is a real outcome with a real cause --
        // no free kerb within 14 m -- and it fails on that cause rather
        // than on the drive that could not be attempted.
        saida::Log::error("[World E2E] FAIL no car parked within reach of the spawn");
        testFailed=true;engine.sceneTree().quit();return;
    }
    // A test driver chooses a free direction, like a human: aiming the car
    // at the nearest real wall is not a failure of the car. Same search the
    // spawn runs for the walk, over a road's worth of distance.
    smokeClear=0;double heading=carYaw;
    for(int direction=0;direction<32;++direction) {
        double candidate=direction*11.25,clear=clearAhead(carLon,carLat,carAlt,candidate,1.,60.);
        if(clear>smokeClear){smokeClear=clear;heading=candidate;}
    }
    r1::VehicleRuntime::deactivate(*car);
    carYaw=heading;placeCar();
    // The car is where the player spawned and the walk phase has taken him
    // eleven metres away from it, which is the first thing this test found
    // and the right thing for it to find: a door has a handle, not a
    // radius. So the driver walks back to it, the way a player does.
    smokeApproach=true;smokeApproachTime=0;
    saida::Log::info("[World E2E] walking back to the car, ",carDistance(),"m away");
}

void World::Impl::beginSmokeDriving() {
    if(!enterCar()) {
        saida::Log::error("[World E2E] FAIL could not enter the car, ",
                          carDistance(),"m away, parked=",carParked);
        testFailed=true;engine.sceneTree().quit();return;
    }
    smokeDriving=true;smokeDriveTime=0;smokeTopSpeed=0;smokeDriveStart=ecef(lon,lat,alt);
    if(std::getenv("R1WORLD_SMOKE_VEHICLE_DAMAGE")) {
        auto* body=r1::VehicleRuntime::body(*car);
        auto* damage=body->getBehaviour<saida::DeformableMeshBehaviour>();
        const auto& spec=vehicleSpec(*car);
        if(!damage->applyImpact({0,float(spec.height*.5),float(-spec.length*.5)},{0,0,1},damage->maximumImpactEnergy)) {
            saida::Log::error("[World E2E] FAIL could not damage the actual car mesh");
            testFailed=true;engine.sceneTree().quit();return;
        }
        r1::VehicleRuntime::synchronize(*car);
        saida::Log::info("[World E2E] damaged car, absorbed energy=",damage->absorbedEnergy(),
            " J, power loss=",r1::VehicleRuntime::driver(*car)->damage);
    }
    churn=0;churnFrames=0;dirtyFrames=0;
    saida::Log::info("[World E2E] driving, ",smokeClear,"m clear on heading ",carYaw);
}

void World::Impl::runSmokeTakeover() {
    smokeTookOver=true;
    Loaded* tile=nullptr;size_t index=0;
    for(auto& [key,l]:loaded) {
        for(size_t i=0;i<l.cars.size();++i)
            if(l.cars[i]&&trafficSlotLive(l,i)){tile=&l;index=i;break;}
        if(tile)break;
    }
    if(!tile) {
        saida::Log::error("[World E2E] FAIL no traffic car to take over");
        testFailed=true;engine.sceneTree().quit();return;
    }
    saida::Node* wanted=tile->cars[index];
    saida::Node* previous=car;
    const double previousLon=carLon,previousLat=carLat;
    const auto where=geodeticOf(*tile,wanted->transform().position);
    // Two and a half metres to the east of it, which is a door's width and
    // inside kCarReach.
    const auto beside=advance(where.x,where.y,2.5,0.);
    if(!World::Impl::tile(beside.x,beside.y)) {
        saida::Log::error("[World E2E] FAIL the traffic car stands off the loaded world");
        testFailed=true;engine.sceneTree().quit();return;
    }
    lon=beside.x;lat=beside.y;alt=height(lon,lat);
    if(!enterCar()||!driving()) {
        saida::Log::error("[World E2E] FAIL could not take over a traffic car 2.5 m away");
        testFailed=true;engine.sceneTree().quit();return;
    }
    if(car!=wanted) {
        saida::Log::error("[World E2E] FAIL took over a different car than the one reached");
        testFailed=true;engine.sceneTree().quit();return;
    }
    if(tile->cars[index]!=nullptr||tile->flow.agents()[index].alive) {
        saida::Log::error("[World E2E] FAIL the traffic still owns the car the player is in");
        testFailed=true;engine.sceneTree().quit();return;
    }
    // And the car he stepped out of is still standing where he left it,
    // which is the half of this that copying a model would have broken.
    const auto left=std::find_if(parked.begin(),parked.end(),
                                 [&](const Parked& p){return p.node==previous;});
    if(left==parked.end()||std::abs(left->lon-previousLon)>1e-9||std::abs(left->lat-previousLat)>1e-9) {
        saida::Log::error("[World E2E] FAIL the previous car did not stay where it was left");
        testFailed=true;engine.sceneTree().quit();return;
    }
    saida::Log::info("[World E2E] took over a traffic car; ",parked.size(),
                     " car(s) left standing");
    // Taking the same solver body preserves its momentum. Brake it through
    // normal frames before testing the door, just as for the first drive.
    smokeDriving=true;smokeBrake=true;smokeDriveTime=0;
}

void World::Impl::runSmokeSail(float delta) {
    const double dt=std::min(.05,double(delta));
    smokeSailTime+=dt;
    if(smokeSailWait) {
        BoatReach best;double bestDistance=1e30;
        for(auto& [key,t]:loaded)
            for(size_t i=0;i<t.boats.size();++i) {
                Mooring& m=t.boats[i];
                if(m.taken||!mooredNode(t,m)||m.kind=="cargo"||m.kind=="liner")continue;
                const double d=hullDistance(t.frame,m.local,m.heading,m.length,m.beam);
                if(d<bestDistance){bestDistance=d;best={&t,i,BoatSource::Moored};}
            }
        if(!best.tile) {
            if(smokeSailTime>60.) {
                saida::Log::error("[World E2E] FAIL sail: no moored boat streamed within 60 s");
                testFailed=true;engine.sceneTree().quit();
            }
            return;
        }
        // Stand the player on the first dry spot beside the hull, which is
        // where a quay or a pier leaves him.
        Mooring& m=best.tile->boats[best.index];
        const auto centre=geodeticOf(*best.tile,glm::vec3(m.local));
        const double s=std::sin(m.heading*rad),c=std::cos(m.heading*rad);
        bool placed=false;
        for(double off:{1.,2.,3.}) {
            for(double along=-m.length*.4;along<=m.length*.4&&!placed;along+=1.)
                for(double side:{1.,-1.}) {
                    const double o=(m.beam*.5+off)*side;
                    auto q=advance(centre.x,centre.y,s*along+c*o,c*along-s*o);
                    if(tile(q.x,q.y)&&!blocked(q.x,q.y)){lon=q.x;lat=q.y;alt=height(lon,lat);placed=true;break;}
                }
            if(placed)break;
        }
        if(!placed) {
            saida::Log::error("[World E2E] FAIL sail: no standable ground beside the nearest boat (",
                              m.kind,", ",m.name,")");
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeSailWait=false;
        if(!enterBoat()) {
            saida::Log::error("[World E2E] FAIL sail: could not take the helm from beside the hull");
            testFailed=true;engine.sceneTree().quit();return;
        }
        // Head for the longest run of open water, as a skipper would.
        smokeSailClear=0;double heading=boat.yaw;
        for(int direction=0;direction<32;++direction) {
            double candidate=direction*11.25,clear=0;
            for(double d=2.;d<=120.;d+=2.) {
                auto q=onward(boat.lon,boat.lat,std::sin(candidate*rad)*d,std::cos(candidate*rad)*d);
                if(!navigable(q.x,q.y))break;
                clear=d;
            }
            if(clear>smokeSailClear){smokeSailClear=clear;heading=candidate;}
        }
        boat.yaw=yaw=heading;
        smokeSailing=true;smokeSailBrake=false;smokeSailTime=0;smokeSailTop=0;
        smokeSailStart=ecef(lon,lat,alt);
        saida::Log::info("[World E2E] sailing a ",boat.kind,", ",smokeSailClear,"m of open water ahead");
        return;
    }
    smokeSailTop=std::max(smokeSailTop,boat.speed);
    if(!navigable(lon,lat)) {
        saida::Log::error("[World E2E] FAIL sail: the boat left the water at ",lon,", ",lat);
        testFailed=true;engine.sceneTree().quit();return;
    }
    if(!smokeSailBrake) {
        if(smokeSailTime<8.)return;
        const double covered=glm::length(ecef(lon,lat,alt)-smokeSailStart);
        if(covered<std::min(15.,smokeSailClear*.4)||smokeSailTop<std::min(3.,boat.top*.5)) {
            saida::Log::error("[World E2E] FAIL sail: covered=",covered,"m of ",smokeSailClear,
                              "m clear, top=",smokeSailTop," m/s");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] sail passed, covered=",covered,"m top=",smokeSailTop," m/s");
        if(std::abs(boat.speed)>kBoatExitSpeed&&leaveBoat()) {
            saida::Log::error("[World E2E] FAIL sail: stepped ashore at ",std::abs(boat.speed)," m/s");
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeSailBrake=true;smokeSailTime=0;return;
    }
    if(std::abs(boat.speed)>kBoatExitSpeed) {
        if(smokeSailTime>40.) {
            saida::Log::error("[World E2E] FAIL sail: the boat would not slow down, still ",
                              std::abs(boat.speed)," m/s");
            testFailed=true;engine.sceneTree().quit();
        }
        return;
    }
    smokeSailing=false;
    const double stopped=std::abs(boat.speed);
    smokeSwimBoat=boat.node;
    if(!leaveBoat()||!swimming()) {
        saida::Log::error("[World E2E] FAIL swim: could not enter the water from a stopped boat");
        testFailed=true;engine.sceneTree().quit();return;
    }
    saida::Log::info("[World E2E] PASS sailing: stopped at ",stopped," m/s and entered the water");
    smokeSwimming=true;smokeSailTime=0;smokeSwimStart=ecef(lon,lat,alt);
}

void World::Impl::runSmokeSwim(float delta) {
    smokeSailTime+=std::min(.05,double(delta));
    if(smokeSailTime<1.5)return;
    const double covered=glm::length(ecef(lon,lat,alt)-smokeSwimStart);
    const double rootBelow=origin.local(ecef(lon,lat,alt)).y-player->transform().position.y;
    if(!swimming()||!onWater(lon,lat)||covered<1.||rootBelow<playerHeight*.39||rootBelow>playerHeight*.78) {
        saida::Log::error("[World E2E] FAIL swim: covered=",covered,"m, water=",onWater(lon,lat),
                          ", body depth=",rootBelow,"m");
        testFailed=true;engine.sceneTree().quit();return;
    }
    smokeSwimming=false;
    if(!car) {
        saida::Log::error("[World E2E] FAIL car sink: no car model to test");
        testFailed=true;engine.sceneTree().quit();return;
    }
    // A car already on this water cell must eject its driver and descend.
    carParked=true;car->setEnabled(true);
    carLon=lon;carLat=lat;carAlt=alt;carYaw=yaw;
    carSinking=false;transitionTravel(TravelMode::Car,false);
    driveCar(.05,1.,0.,false);
    updateSinkingCar(1.);
    if(driving()||!swimming()||!carSinking||carAlt>carWaterLevel-1.) {
        saida::Log::error("[World E2E] FAIL car sink: driver afloat=",swimming(),
                          ", car depth=",carWaterLevel-carAlt,"m");
        testFailed=true;engine.sceneTree().quit();return;
    }
    saida::Log::info("[World E2E] PASS car sink: driver swimming, car ",carWaterLevel-carAlt,"m below water");
    if(!enterBoat()||boat.node!=smokeSwimBoat) {
        saida::Log::error("[World E2E] FAIL swim: could not reboard the same boat");
        testFailed=true;engine.sceneTree().quit();return;
    }
    saida::Log::info("[World E2E] PASS swim: ",covered,"m through water, reboarded the same boat");
    smokeSeaWait=true;smokeSailTime=0;
}

void World::Impl::runSmokeSea(float delta) {
    smokeSailTime+=std::min(.05,double(delta));
    size_t under=0;
    for(auto& s:seaShips)under+=s.v.speed>0;
    if(!seaShips.empty()&&smokeSailTime>12.) {
        saida::Log::info("[World E2E] PASS sea: ",seaShips.size()," ships out, ",under," under way");
        const SeaShip* target=nullptr;
        double nearest=1e30;
        for(const auto& ship:seaShips)if(ship.v.speed>2.) {
            const double distance=metresFrom(ship.v.lon,ship.v.lat);
            if(distance<nearest){nearest=distance;target=&ship;}
        }
        if(!target) {
            saida::Log::error("[World E2E] FAIL sea helm: no moving ship to approach");
            testFailed=true;engine.sceneTree().quit();return;
        }
        const Vessel& v=target->v;
        const double side=v.beam*.5+boat.beam*.5+1.;
        const auto alongside=advance(v.lon,v.lat,std::cos(v.yaw*rad)*side,-std::sin(v.yaw*rad)*side);
        boat.lon=lon=alongside.x;boat.lat=lat=alongside.y;
        boat.alt=alt=v.alt;boat.yaw=yaw=v.yaw;boat.speed=v.speed;
        const auto* hull=v.node;
        const double speed=v.speed;
        if(hopAboard()!=HopResult::Boarded||!sailing()||boat.node!=hull||std::abs(boat.speed-speed)>1e-6) {
            saida::Log::error("[World E2E] FAIL sea helm: could not take the same moving hull from alongside");
            testFailed=true;
        } else saida::Log::info("[World E2E] PASS sea helm: took over the same moving hull at ",boat.speed," m/s");
        engine.sceneTree().quit();
    } else if(smokeSailTime>30.) {
        std::string last=sea->lastLine();
        if(last.empty())last="the sea said nothing";
        saida::Log::error("[World E2E] FAIL sea: no ship at sea after 30 s (",last,")");
        testFailed=true;engine.sceneTree().quit();
    }
}

void World::Impl::flyShot(const char* moment) {
    const char* path=std::getenv("R1WORLD_FLY_SHOT");
    const char* at=std::getenv("R1WORLD_FLY_SHOT_AT");
    if(!path||!at||std::string(at)!=moment||captureQueued)return;
    saida::CaptureRequest shot;
    shot.pngPath=path;shot.frame=3;shot.fixedStep=1.f/60.f;shot.settleTimeoutFrames=900;
    captureQueued=true;
    saida::Log::info("[World E2E] photographing '",moment,"' to ",path);
    engine.captureFrameThenExit(shot);
}

void World::Impl::flyFail(const std::string& why) {
    saida::Log::error("[World E2E] FAIL fly: ",why);
    testFailed=true;engine.sceneTree().quit();
}

std::string World::Impl::flyCategory(const r1::AircraftType& t) {return t.klass=="helicopter"?"helicopter":"plane";}

bool World::Impl::smokeNextAircraft(Loaded*& found,size_t& index) {
    found=nullptr;double best=1e30;int bestRank=9;
    for(auto& [key,t]:loaded) {
        const glm::dvec3 me=t.frame.local(ecef(lon,lat,alt));
        for(size_t i=0;i<t.aircraft.size();++i) {
            const AircraftSpot& s=t.aircraft[i];
            const auto* type=r1::aircraftType(s.type);
            if(s.taken||!s.node||!type||smokeFlown.count(flyCategory(*type)))continue;
            const int rank=type->klass=="jet"?0:type->klass=="airliner"?1:2;
            const double d=std::hypot(me.x-s.local.x,me.z-s.local.z);
            if(rank<bestRank||(rank==bestRank&&d<best)){bestRank=rank;best=d;found=&t;index=i;}
        }
    }
    return found!=nullptr;
}

double World::Impl::clearRun(double heading,double reach,double step,double above) {
    double clear=0;
    for(double d=step;d<=reach;d+=step) {
        const auto q=onward(plane.lon,plane.lat,std::sin(heading*rad)*d,std::cos(heading*rad)*d);
        if(!tile(q.x,q.y)||aircraftHits(q.x,q.y,plane.alt+above,heading)||(above<1.&&onWater(q.x,q.y)))break;
        clear=d;
    }
    return clear;
}

void World::Impl::runSmokeFly(float delta) {
    const double dt=std::min(.05,double(delta));
    smokeFlyTime+=dt;
    smokeF=smokeR=smokeUp=0;
    if(smokeFlyWait) {
        Loaded* t=nullptr;size_t i=0;
        if(!smokeNextAircraft(t,i)) {
            if(smokeFlown.empty()&&smokeFlyTime<90.)return;
            if(smokeFlown.empty())return flyFail("no aircraft streamed within 90 s of the spawn");
            saida::Log::info("[World E2E] PASS fly: flew ",smokeFlown.size()," kind(s) of aircraft");
            engine.sceneTree().quit();return;
        }
        // Line up only once the neighbourhood is resident: a run measured
        // over three tiles of four misses the buildings of the fourth.
        size_t resident=0;
        for(const auto& k:ring)resident+=loaded.count(k.key());
        if(resident<ring.size()&&smokeFlyTime<45.)return;
        AircraftSpot& spot=t->aircraft[i];
        const auto* type=r1::aircraftType(spot.type);
        // Stand the pilot beside the fuselage, where a walk would bring him.
        const auto centre=geodeticOf(*t,glm::vec3(spot.local));
        const double s=std::sin(spot.heading*rad),c=std::cos(spot.heading*rad);
        bool placed=false;
        for(double along:{type->length*.3,0.,-type->length*.25})
            for(double side:{-1.,1.}) {
                if(placed)break;
                const double o=(fuselageHalf(*type)+1.5)*side;
                const auto q=advance(centre.x,centre.y,s*along+c*o,c*along-s*o);
                if(tile(q.x,q.y)&&!blocked(q.x,q.y)){lon=q.x;lat=q.y;alt=height(lon,lat);placed=true;}
            }
        if(!placed)return flyFail("no standable ground beside the "+spot.type+" ("+spot.name+")");
        saida::Node* wanted=spot.node;
        if(!enterAircraft()||!piloting()||plane.node!=wanted)
            return flyFail("could not take the controls of the "+spot.type+" the pilot stands beside");
        smokeFlyWait=false;smokeFlyTime=0;smokeFlyTop=0;
        smokeFlyStart=ecef(plane.lon,plane.lat,plane.alt);smokeFlyAlt=plane.alt;
        flyShot(plane.helicopter()?"heli-stand":"stand");
        if(plane.helicopter()) {
            smokeFlyPhase=10;
            saida::Log::info("[World E2E] flying a ",plane.type->name," (",spot.name,")");
        } else {
            // Line up as a pilot would: the longest clear run on the ground.
            smokeFlyClear=0;double heading=plane.yaw;
            for(int k=0;k<36;++k) {
                const double h=k*10.,run=clearRun(h,1500.,10.,0.);
                if(run>smokeFlyClear){smokeFlyClear=run;heading=h;}
            }
            plane.yaw=yaw=heading;
            smokeFlyPhase=1;
            saida::Log::info("[World E2E] flying a ",plane.type->name," (",spot.name,"), ",smokeFlyClear,
                             " m clear on heading ",heading);
        }
        return;
    }
    Aircraft& p=plane;
    smokeFlyTop=std::max(smokeFlyTop,p.speed);
    switch(smokeFlyPhase) {
    case 1: {  // the take-off roll
        smokeF=1.;smokeUp=p.speed>=p.type->rotate?1.:0.;
        if(p.airborne&&p.alt-smokeFlyAlt>=25.) {
            saida::Log::info("[World E2E] PASS take-off: ",p.type->name," up ",p.alt-smokeFlyAlt," m, top ",
                             smokeFlyTop," m/s, ",glm::length(ecef(p.lon,p.lat,p.alt)-smokeFlyStart)," m from the stand");
            flyShot("climb");
            smokeFlyPhase=2;smokeFlyTime=0;
        } else if(smokeFlyTime>60.) {
            flyFail("no take-off in 60 s: top "+number(smokeFlyTop,1)+" m/s for a rotation at "+
                    number(p.type->rotate,1)+", "+number(smokeFlyClear,0)+" m clear, airborne="+(p.airborne?"yes":"no"));
        }
        return;
    }
    case 2: {  // flown low at the nearest tall building: it must stop the aircraft
        const Loaded* in=nullptr;glm::dvec2 centre(0);double radius=0,best=1e30;
        for(auto& [key,t]:loaded) {
            const glm::dvec3 me=t.frame.local(ecef(p.lon,p.lat,p.alt));
            for(const auto& f:t.footprints) {
                if(f.top>1e29||f.points.size()<3)continue;
                glm::dvec2 c(0);
                for(const auto& q:f.points)c+=q;
                c/=double(f.points.size());
                double r=0;
                for(const auto& q:f.points)r=std::max(r,glm::length(q-c));
                const auto at=geodeticOf(t,glm::vec3(float(c.x),0.f,float(c.y)));
                const double tall=f.top-t.frame.local(ecef(at.x,at.y,groundAt(at.x,at.y,p.alt))).y;
                const double d=std::hypot(me.x-c.x,me.z-c.y);
                if(r<6.||tall<8.||d>=best)continue;
                best=d;in=&t;centre=c;radius=r;
            }
        }
        double chosen=-1;
        if(in) {
            const auto middle=geodeticOf(*in,glm::vec3(float(centre.x),0.f,float(centre.y)));
            const double away=radius+p.type->length*.5+p.type->span*.5+25.;
            for(int k=0;k<36&&chosen<0;++k) {
                const double h=k*10.;
                const auto start=advance(middle.x,middle.y,-std::sin(h*rad)*away,-std::cos(h*rad)*away);
                if(!tile(start.x,start.y)||onWater(start.x,start.y))continue;
                const double ground=groundAt(start.x,start.y,p.alt);
                if(aircraftHits(start.x,start.y,ground+3.,h))continue;
                chosen=h;p.lon=start.x;p.lat=start.y;p.alt=ground+3.;
            }
            if(chosen>=0)saida::Log::info("[World E2E] flying at a building ",away," m ahead, ",
                                          p.type->stall*1.3," m/s, 3 m up");
        }
        if(chosen<0) {
            saida::Log::info("[World E2E] no building with a clear approach here; the stop is not tested");
            smokeFlyPhase=4;smokeFlyTime=0;return;
        }
        p.yaw=yaw=chosen;p.pitch=0;p.roll=0;p.climb=0;p.airborne=true;
        p.speed=p.type->stall*1.3;p.lever=1;p.thrust=1;planeStopped=false;
        smokeFlyPhase=3;smokeFlyTime=0;
        return;
    }
    case 3: {  // stopped, not crashed, and not inside the building
        smokeF=1.;
        if(planeStopped&&p.speed==0.) {
            if(buildingAt(p.lon,p.lat,p.alt+.4))return flyFail("the aircraft stopped inside the building");
            saida::Log::info("[World E2E] PASS stopped by a building, no crash, ",p.alt-planeGround," m up");
            flyShot("stop");
            smokeFlyPhase=4;smokeFlyTime=0;
        } else if(smokeFlyTime>6.) {
            flyFail("flew at a building for 6 s and was never stopped");
        }
        return;
    }
    case 4: {  // down, stopped, and out
        smokeF=-1.;smokeUp=-1.;
        if(p.airborne||std::abs(p.speed)>kAircraftExitSpeed) {
            if(smokeFlyTime>40.)flyFail("the plane did not come down and stop within 40 s");
            return;
        }
        if(!leaveAircraft())return flyFail("could not step down from the landed plane");
        saida::Log::info("[World E2E] PASS plane: down, stopped and out");
        smokeFlown.insert("plane");smokeFlyPhase=0;smokeFlyWait=true;smokeFlyTime=0;
        return;
    }
    case 10: {  // spin up and climb
        smokeUp=1.;
        if(p.alt-smokeFlyAlt>=20.) {
            saida::Log::info("[World E2E] PASS helicopter climb: ",p.alt-smokeFlyAlt," m in ",smokeFlyTime," s");
            flyShot("heli");
            double run=0,heading=p.yaw;
            for(int k=0;k<36;++k){const double h=k*10.,r=clearRun(h,300.,5.,0.);if(r>run){run=r;heading=h;}}
            p.yaw=yaw=heading;
            smokeFlyStart=ecef(p.lon,p.lat,p.alt);smokeFlyPhase=11;smokeFlyTime=0;
        } else if(smokeFlyTime>15.) {
            flyFail("the helicopter climbed "+number(p.alt-smokeFlyAlt,1)+" m in 15 s");
        }
        return;
    }
    case 11: {  // forward
        smokeF=1.;
        if(smokeFlyTime>=4.) {
            const double covered=glm::length(ecef(p.lon,p.lat,p.alt)-smokeFlyStart);
            if(covered<20.&&!planeStopped)return flyFail("the helicopter covered "+number(covered,1)+" m in 4 s");
            saida::Log::info("[World E2E] PASS helicopter forward: ",covered," m, top ",smokeFlyTop," m/s");
            smokeFlyPhase=12;smokeFlyTime=0;
        }
        return;
    }
    case 12: {  // brake to a hover, and jump out in the air
        smokeF=p.speed>1.?-1.:0.;
        if(p.airborne&&std::abs(p.speed)<=1.) {
            const double up=p.alt-groundAt(p.lon,p.lat,p.alt);
            if(!leaveAircraft()||piloting())return flyFail("F did not let the pilot out "+number(up,1)+" m up");
            if(jumpOffset<5.)return flyFail("jumped from "+number(up,1)+" m up but falls only "+number(jumpOffset,1)+" m");
            saida::Log::info("[World E2E] PASS jumped from the helicopter, ",jumpOffset," m up");
            smokeFlyPhase=13;smokeFlyTime=0;
        } else if(smokeFlyTime>30.) {
            flyFail("the helicopter did not come to a hover within 30 s");
        }
        return;
    }
    case 13: {  // he lands, and the helicopter he left comes down
        const bool down=jumpOffset<=0&&(leftAircraft.empty()||!leftAircraft.back().airborne);
        if(down) {
            if(!swimming()&&blocked(lon,lat))return flyFail("the jump landed inside a building");
            flyShot("jump");
            saida::Log::info("[World E2E] PASS landed ",swimming()?"in the water":"on the ground",
                             " in ",smokeFlyTime," s, and the helicopter came down");
            smokeFlown.insert("helicopter");smokeFlyPhase=0;smokeFlyWait=true;smokeFlyTime=0;
        } else if(smokeFlyTime>20.) {
            flyFail("still falling after 20 s: "+number(jumpOffset,1)+" m up, helicopter airborne="+
                    (leftAircraft.empty()?"none":leftAircraft.back().airborne?"yes":"no"));
        }
        return;
    }
    default: return;
    }
}

void World::Impl::measureWalk() {
    const auto started=std::chrono::steady_clock::now();
    size_t nodes=0;
    engine.sceneTree().world().traverse([&](saida::Node&,const glm::mat4&){++nodes;});
    const double ms=msSince(started);
    saida::Log::info("[World perf] one bare traversal of ",nodes," nodes took ",ms," ms");
    // Where they are. A walk is paid per node, so the composition is the
    // whole optimisation brief.
    auto subtree=[](saida::Node& n){
        size_t count=0;
        std::function<void(saida::Node&)> go=[&](saida::Node& x){
            ++count;for(auto& c:x.children())go(*c);
        };
        go(n);return count;
    };
    size_t plants=0,plantNodes=0,otherNodes=0,tiles=0;
    for(auto& [key,t]:loaded) {
        ++tiles;
        size_t here=subtree(*t.node);
        for(const Plant& plant:t.vegetation){++plants;plantNodes+=subtree(*plant.node);}
        otherNodes+=here;
    }
    saida::Log::info("[World perf] ",tiles," tiles hold ",otherNodes," nodes, of which ",
                     plantNodes," belong to ",plants," plants (",
                     plants?double(plantNodes)/double(plants):0.," per plant)");
}

void World::Impl::reportChurn(const char* phase) {
    saida::Log::info("[World perf] ",phase,": ",
                     int(churnFrames>0?100.*dirtyFrames/churnFrames:0.),
                     "% of frames updated scene membership (",
                     churnFrames>0?churn/churnFrames:0.," indexed nodes per frame over ",
                     int(churnFrames)," frames)");
    churn=0;churnFrames=0;dirtyFrames=0;
}

void World::Impl::cycleSmokeCheat(double dt) {
    if(smokeMountedShotDelay>=0) {
        setAiming(true);poseArms();bikeHandlePose();
        smokeMountedShotDelay-=dt;
        if(smokeMountedShotDelay>0)return;
        smokeMountedShotDelay=-1;
        auto fail=[&](const char* reason){saida::Log::error("[World E2E] FAIL cheats: ",reason);testFailed=true;engine.sceneTree().quit();};
        const int rounds=arsenal.loaded();fire();
        if(arsenal.loaded()!=rounds-1||pistol->parent()!=playerBody||combatFilter().ignore!=r1::VehicleRuntime::body(*car)->bodyId()) {
            saida::Log::error("[World E2E] mounted shot before=",rounds," after=",arsenal.loaded(),
                " attached=",pistol->parent()==playerBody," own-body-filter=",combatFilter().ignore==r1::VehicleRuntime::body(*car)->bodyId());
            fail("the mounted pistol did not fire or ignored the wrong collider");return;
        }
        if(!leaveCar()||playerBikeRider||playerBody->parent()!=player||car->findByPath("Vehicle collider/Rider")) {
            fail("leaving a bike did not return the real body and leave an empty saddle");return;
        }
        saida::Log::info("[World E2E] PASS motorcycle empty spawn, solver mount, armed shot and empty dismount");
        const double hp=health.hp;hurtPlayer(500.,glm::vec3(1,0,0),"the test");
        if(health.take(500.)||health.hp!=hp||wasted){fail("an invincible player was hurt");return;}
        health.shield=0;
        saida::Log::info("[World E2E] PASS cheats: helicopter, sport car, city bike, motocross and jet beside the player, invincibility");
        return;
    }
    if(!smokeCheatSettled) {
        if(smokeSettleSince<0){smokeSettleSince=smokeSettleRead=clock;smokeSettleLast=privateMegabytes();return;}
        if(clock-smokeSettleRead<2.)return;
        const double now=privateMegabytes();
        smokeSettleStill=std::abs(now-smokeSettleLast)<16.?smokeSettleStill+1:0;
        smokeSettleLast=now;smokeSettleRead=clock;
        if(smokeSettleStill<2&&clock-smokeSettleSince<90.)return;
        smokeCheatSettled=true;smokeCheatMemory=smokeSettleStill>=2?now:-1.;
        saida::Log::info("[World E2E] cheats: ",smokeSettleStill>=2?"memory settled":"memory still moving",
                         " after ",int(clock-smokeSettleSince)," s at ",int(now)," MB");
        return;
    }
    if(cheatOpen) {
        if(engine.uiInteraction().focusedWebCanvas()!=ui) {
            saida::Log::error("[World E2E] FAIL cheats: the line lost the keyboard after an update (press ",smokeCheatCycles/2,")");
            testFailed=true;engine.sceneTree().quit();return;
        }
        closeCheat();
    } else openCheat();
    if(++smokeCheatCycles<2*kSmokeCheatPresses)return;
    if(cheatOpen)closeCheat();
    smokeCheatCycles=0;
    if(smokeCheatMemory<0)
        saida::Log::warn("[World E2E] PASS cheats: ",kSmokeCheatPresses," presses of T kept the keyboard;"
                         " memory not judged, the world never settled");
    else {
        const double grown=privateMegabytes()-smokeCheatMemory;
        saida::Log::info("[World E2E] ",grown<64.?"PASS":"FAIL"," cheats: ",kSmokeCheatPresses,
                         " presses of T kept the keyboard; private memory went from ",int(smokeCheatMemory),
                         " MB by ",grown>=0?"+":"",int(grown)," MB");
        if(grown>=64.){testFailed=true;engine.sceneTree().quit();return;}
    }
    smokeFinishPhase();
}

bool World::Impl::smokeCheats() {
    smokeCheated=true;
    auto fail=[&](const std::string& why){
        saida::Log::error("[World E2E] FAIL cheats: ",why);testFailed=true;engine.sceneTree().quit();return false;
    };
    auto type=[&](const std::string& word){
        openCheat();
        if(!cheatOpen||engine.uiInteraction().focusedWebCanvas()!=ui)return false;
        if(held(GLFW_KEY_W)||clicked(GLFW_MOUSE_BUTTON_LEFT))return false;
        field("cheat-input",word);
        const std::string typed=value("cheat-input");closeCheat();runCheat(typed);
        return !cheatOpen&&engine.uiInteraction().focusedWebCanvas()==nullptr;
    };
    const size_t aircraft=leftAircraft.size(),cars=parked.size();
    if(!type("buzzoff"))return fail("the line did not take and give back the keyboard");
    if(leftAircraft.size()!=std::min(aircraft+1,kLeftAircraft)||!leftAircraft.back().helicopter())
        return fail("BUZZOFF left no helicopter: "+written["text:cheat-said"]);
    if(!type("Comet ")||parked.size()!=std::min(cars+1,kAbandonedCars)||vehicleSpec(*parked.back().node).name!="sport")
        return fail("COMET parked no sport car: "+written["text:cheat-said"]);
    if(!type("moto")||vehicleSpec(*parked.back().node).name!="moto_city")
        return fail("MOTO parked no city bike: "+written["text:cheat-said"]);
    if(!type("rocket")||vehicleSpec(*parked.back().node).name!="moto_cross")
        return fail("ROCKET parked no motocross bike: "+written["text:cheat-said"]);
    if(!type("plane")||leftAircraft.back().type->name!="bizjet")return fail("PLANE left no jet: "+written["text:cheat-said"]);
    for(const auto* a:{&leftAircraft[leftAircraft.size()-2],&leftAircraft.back()})
        if(metresBetween(a->lon,a->lat,lon,lat)>45.)return fail(a->type->name+" stands beyond 45 m");
    const size_t before=leftAircraft.size();
    if(!type("hesoyam")||leftAircraft.size()!=before)return fail("an unknown word did something");
    if(!type("painkiller")||!health.invincible())return fail("PAINKILLER gave no invincibility");
    const auto cityBike=std::find_if(parked.begin(),parked.end(),[&](const Parked& p) {
        return p.node&&vehicleSpec(*p.node).name=="moto_city";
    });
    if(cityBike==parked.end())return fail("the city bike disappeared before the rider check");
    const Parked bike=*cityBike;
    lon=bike.lon;lat=bike.lat;alt=height(lon,lat,bike.alt);
    if(car&&car->findByPath("Vehicle collider/Rider"))return fail("a parked bike contains a traffic rider");
    if(bike.node->findByPath("Vehicle collider/Rider"))return fail("MOTO spawned an occupied bike");
    if(!enterCar()||!ridingBike()||playerBikeRider!=playerBody||
       playerBody->parent()!=r1::VehicleRuntime::body(*car)||car->findByPath("Vehicle collider/Rider"))
        return fail("F did not mount exactly the real player body on the bike solver");
    selectWeapon(r1::combat::Weapon::Pistol);setAiming(true);
    // Weapon selection has a real 250 ms draw cooldown. Continue through
    // ordinary frames so this check exercises it rather than bypassing it.
    smokeMountedShotDelay=.35;
    return true;
}

void World::Impl::smokeFinishPhase() {
    if(!smokeCheated) {
        if(smokeCheats())smokeCheatCycles=1;
        return;
    }
    reportChurn("whole run");
    if(hopWanted&&!hopDone) {
        // Arrive from the driver's seat while looking out of a side window.
        // A stale lookYaw made forward walking go sideways after loading.
        if(!driving()&&carParked&&carDistance()<kCarReach)enterCar();
        if(driving()) {
            lookYaw=90.;lookIdle=.4;
            smokeArrivalHeading=wrap(yaw+lookYaw);
        }
        // Teleport the way a player does: open the map, type the
        // destination, press Go. Nothing here reaches past the UI.
        hopDone=true;showMap(true);
        field("longitude",hopLon);field("latitude",hopLat);
        hopArmed=true;hopWait=0;
        return;
    }
    // Resume is on the settings' Destination tab since the globe menu:
    // reached there, as a player does.
    showMap(true);showOptions(true);ui->executeJS("showSettingsTab('destination');");testResume=true;
}

void World::Impl::sayBodies() {
    auto* physics=engine.sceneTree().world().physics();
    if(!physics)return;
    // The densest tile's static bodies, what kTileStaticBodies answers for.
    size_t trunks=0,densest=0;std::string densestKey;
    for(const auto& [key,t]:loaded) {
        if(auto* body=t.node->findByPath("Tree trunks"))trunks+=body->children().size();
        size_t statics=0;
        std::function<void(const saida::Node&)> count=[&](const saida::Node& n) {
            // A car's box is a traffic slot's body, counted in kTileMovingBodies.
            if(auto* body=dynamic_cast<const saida::StaticBodyNode*>(&n);
               body&&!body->bodyId().IsInvalid()&&body->name()!="Vehicle collider")++statics;
            for(const auto& c:n.children())count(*c);
        };
        count(*t.node);
        if(statics>densest){densest=statics;densestKey=key;}
    }
    saida::Log::info("[World physics] ",physics->bodyCount()," bodies of ",physics->capacity().bodies,
                     " (kPhysicsBodies) across ",loaded.size()," tiles, ",trunks," tree trunks among them; densest tile ",
                     densestKey," holds ",densest," static bodies of ",kTileStaticBodies," (kTileStaticBodies)");
}

bool World::Impl::trunksSolid() {
    auto* physics=engine.sceneTree().world().physics();
    size_t checked=0;
    for(const auto& [key,t]:loaded) {
        auto* body=dynamic_cast<saida::StaticBodyNode*>(t.node->findByPath("Tree trunks"));
        if(!body||body->children().empty())continue;
        // Trunks join as their trees stream: a body whose first trees
        // arrived this frame is built at the next sync, refused never.
        if(body->bodyId().IsInvalid()&&!body->bodyRefused())continue;
        auto* shape=dynamic_cast<saida::CollisionShapeNode*>(body->children().front().get());
        const glm::vec3 at(body->worldTransform()*glm::vec4(shape->offset-glm::vec3(0,1.f,0),1.f));
        bool met=false;
        if(physics)for(auto id:physics->overlapSphere(at,.32f,obstacleFilter()))met=met||id==body->bodyId();
        if(!met) {
            saida::Log::error("[World E2E] FAIL trunks: ",key,"'s first trunk is not solid (",
                              body->children().size()," trunks, body ",body->bodyRefused()?"refused":body->bodyId().IsInvalid()?"unbuilt":"built",")");
            return false;
        }
        ++checked;
    }
    saida::Log::info("[World E2E] trunks solid in ",checked," tiles");
    return true;
}

bool World::Impl::groundMatchesDrawing() {
    auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
    const Loaded* t=tile(lon,lat);if(!t||t->ocean)return true;
    if(std::find(t->water.begin(),t->water.end(),uint8_t(2))!=t->water.end()) {
        saida::Log::info("[World E2E] ground check skipped: the tile meets the sea");return true;
    }
    double worst=0,worstLon=0,worstLat=0,worstHeight=0;size_t samples=0;
    for(int i=1;i<16;++i)for(int j=1;j<16;++j) {
        const double x=t->west+(t->east-t->west)*i/16.,y=t->south+(t->north-t->south)*j/16.;
        const double h=terrainHeight(*t,x,y);
        const auto hit=physics->raycast(glm::vec3(origin.local(ecef(x,y,h+2.))),{0,-1,0},4.f,obstacleFilter());
        if(!hit.hit)continue;
        auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
        if(!node||node->name().rfind("Ground",0)!=0)continue; // a street, a roof or a trunk is above it
        const double error=std::abs(double(hit.point.y)-origin.local(ecef(x,y,h)).y);
        if(error>worst){worst=error;worstLon=x;worstLat=y;worstHeight=h;}++samples;
    }
    if(worst>.05) {
        saida::Log::error("[World E2E] FAIL ground: the height followed is ",worst,
                          " m off the ground drawn (",samples," samples) at ",worstLon,",",worstLat,
                          " followed=",worstHeight," grid=",gridHeight(*t,worstLon,worstLat),
                          " onDeck=",onDeck(*t,worstLon,worstLat));
        return false;
    }
    saida::Log::info("[World E2E] ground followed within ",worst," m of the ground drawn (",samples," samples)");
    return true;
}

bool World::Impl::seamsClosed() {
    auto coastal=[](const Loaded& t){return t.ocean||std::find(t.water.begin(),t.water.end(),uint8_t(2))!=t.water.end();};
    // A dug node bends the ground out to the next node, a grid step away.
    auto dug=[](const Loaded& t,double x,double y){
        const double step=(t.north-t.south)*kMetresPerDegree/std::max(1,t.gridSize-1);
        for(const auto& d:t.data.value("groundDigs",json::array()))
            if(std::hypot((x-d[0].get<double>())*kMetresPerDegree*std::cos(y*rad),(y-d[1].get<double>())*kMetresPerDegree)<d[2].get<double>()+step)return true;
        return false;
    };
    // A tile whose next version is already cooked is on its way out: its
    // ground was joined to neighbours that have since moved on.
    auto leaving=[&](const std::string& key,const Loaded& t){
        const auto current=service->find(t.served->cooked.tile);
        return incoming.count(key)||(current&&current->serial!=t.served->serial);
    };
    double worst=0,worstDug=0;std::string where;size_t seams=0,changing=0;
    for(const auto& [ka,a]:loaded)for(const auto& [kb,b]:loaded) {
        if(&a==&b||a.gridSize<2||b.gridSize<2||coastal(a)||coastal(b))continue;
        const bool rows=std::abs(a.north-b.south)<1e-9,sides=std::abs(a.east-b.west)<1e-9&&std::abs(a.south-b.south)<1e-9;
        if(!rows&&!sides)continue;
        const double from=rows?std::max(a.west,b.west):a.south,to=rows?std::min(a.east,b.east):a.north;
        if(to-from<1e-9)continue;
        if(leaving(ka,a)||leaving(kb,b)){++changing;continue;}
        ++seams;
        for(int k=0;k<=200;++k) {
            const double s=from+(to-from)*k/200.,x=rows?s:a.east,y=rows?a.north:s;
            const double step=std::abs(gridHeight(a,x,y)-gridHeight(b,x,y));
            if(dug(a,x,y)||dug(b,x,y)){worstDug=std::max(worstDug,step);continue;}
            if(step>worst) {
                worst=step;
                auto said=[](const Loaded& t){return t.data.value("elevationSource",std::string())+", moved "+
                                                     t.data.value("groundSeams",json::object()).dump();};
                where=ka+" ("+said(a)+") | "+kb+" ("+said(b)+") at "+std::to_string(x)+", "+std::to_string(y);
            }
        }
    }
    if(worst>.15) {
        saida::Log::error("[World E2E] FAIL seams: a ",worst," m step between ",where," (",seams," seams)");
        return false;
    }
    if(worstDug>kCarKerb) {
        saida::Log::error("[World E2E] FAIL seams: a ",worstDug," m step under a bridge dug differently by two tiles");
        return false;
    }
    saida::Log::info("[World E2E] ",seams," seams closed within ",worst," m; under a dug bridge, within ",worstDug,
                     " m; ",changing," pairs left out while a newer cook replaces one side");
    return true;
}

bool World::Impl::checkPhysics() {
    auto* physics=engine.sceneTree().world().physics();
    if(!physics||physics->refusedBodies()==physicsRefused)return false;
    physicsRefused=physics->refusedBodies();
    saida::Log::error("[World physics] ",physicsRefused," bodies refused: ",physics->bodyCount()," of ",
                      physics->capacity().bodies," in use (kPhysicsBodies) across ",loaded.size()," tiles");
    if(!smoke)return false;
    saida::Log::error("[World E2E] FAIL physics: the world refused ",physicsRefused," bodies");
    testFailed=true;engine.sceneTree().quit();return true;
}

} // namespace r1::world
