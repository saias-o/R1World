#include "session.hpp"

namespace r1::world {
std::string World::Impl::number(double n,int precision) {std::ostringstream s;s<<std::fixed<<std::setprecision(precision)<<n;return s.str();}

void World::Impl::style(const std::string& id,const std::string& key,const std::string& value) {
    auto& last=written["style:"+id+":"+key];
    if(last==value)return;
    if(auto* e=ui->findElementById(id)){e->SetProperty(key,value);ui->notifyJsMutation();last=value;}
}

void World::Impl::text(const std::string& id,const std::string& s) {
    auto& last=written["text:"+id];
    if(last==s)return;
    // setElementText escapes a straight apostrophe to &apos;, which the
    // interface then shows as written ("l&apos;eau"): French is written
    // with the typographic one anyway.
    std::string shown=s;
    for(size_t at;(at=shown.find('\''))!=std::string::npos;)shown.replace(at,1,"’");
    if(ui->setElementText(id,shown))last=s;
    if(id=="status")style("status","display",s.empty()?"none":"block");
}

void World::Impl::miniText(const std::string& id,const std::string& s) {
    auto& last=written["mini:text:"+id];
    if(last==s)return;
    std::string shown=s;
    for(size_t at;(at=shown.find('\''))!=std::string::npos;)shown.replace(at,1,"’");
    if(minimapUi->setElementText(id,shown))last=s;
}

void World::Impl::layoutMinimap() {
    const float scale=ui->screenSize().x/1440.f;
    const glm::vec2 edge=ui->screenPosition();
    const glm::vec3 position(edge.x+32.f*scale,edge.y+(900.f-28.f-344.f)*scale,0.f);
    auto& transform=minimapUi->transform();
    if(glm::distance(transform.position,position)>.1f)transform.position=position;
    if(std::abs(transform.scale.x-scale)>.001f)transform.scale=glm::vec3(scale);
}

void World::Impl::updateMinimap() {
    layoutMinimap();
    std::vector<const r1::MiniMapTile*> tiles;
    tiles.reserve(loaded.size());
    for(const auto& [key,entry]:loaded)tiles.push_back(&entry.served->cooked.minimap);
    const std::string zone=localConditions()?conditions.value("timezone",std::string()):"";
    const auto policeMarkers=policeMapRml();
    if(written["mini:police"]!=policeMarkers&&minimapUi->setElementRml("mini-police",policeMarkers))written["mini:police"]=policeMarkers;
    miniText("mini-time",localClock());
    miniText("mini-place",r1::miniMapPlace(tiles,zoneCountries,countryNames,zone,lon,lat));
    if(minimapDrawnRevision!=minimapRevision||metresBetween(minimapLon,minimapLat,lon,lat)>8.) {
        const std::string roads=r1::miniMapRoadRml(tiles,lon,lat);
        auto& last=written["mini:roads"];
        if(last!=roads&&minimapUi->setElementRml("mini-roads",roads))last=roads;
        minimapLon=lon;minimapLat=lat;minimapDrawnRevision=minimapRevision;
    }
    const double heading=driving()?carYaw:sailing()?boat.yaw:piloting()?plane.yaw:yaw;
    const std::string rotation="rotate("+number(heading,0)+"deg)";
    auto& previous=written["mini:heading"];
    if(previous!=rotation)if(auto* marker=minimapUi->findElementById("mini-player")) {
        marker->SetProperty("transform",rotation);minimapUi->notifyJsMutation();previous=rotation;
    }
}

std::string World::Impl::value(const std::string& id) {
    auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));return e?e->GetValue():"";
}

void World::Impl::field(const std::string& id,const std::string& value) {
    auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));
    if(!e||e->GetValue()==value)return;
    e->SetValue(value);
    ui->notifyJsMutation();
}

void World::Impl::field(const std::string& id,double x) {field(id,number(x));}

void World::Impl::showCityChoices(const std::vector<r1::PlaceChoice>& choices,const std::string& feedback) {
    cityChoices=choices;
    style("city-results","display",choices.empty()&&feedback.empty()?"none":"block");
    style("city-feedback","display",feedback.empty()?"none":"block");
    if(!feedback.empty())text("city-feedback",feedback);
    for(size_t i=0;i<5;++i) {
        const std::string id="city-choice-"+std::to_string(i);
        style(id,"display",i<choices.size()?"block":"none");
        if(auto* e=ui->findElementById(id))e->SetAttribute("data-available",i<choices.size()?"true":"false");
        if(i<choices.size())text(id,choices[i].label);
    }
}

void World::Impl::updateCityLookup() {
    if(menu) {
        std::string query=value("city-query");
        const auto first=query.find_first_not_of(" \t\r\n");
        query=first==std::string::npos?"":query.substr(first,query.find_last_not_of(" \t\r\n")-first+1);
        if(query!=cityInput) {
            cityInput=query;cityChangedAt=clock;touchGlobe();
            showCityChoices({},query.size()<2?"":"Searching…");
        }
    }
    if(cityFuture.valid()&&cityFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        const std::string completed=cityInFlight;
        cityInFlight.clear();
        try {
            auto choices=cityFuture.get();
            if(cityCache.size()>=64)cityCache.clear();
            cityCache[completed]=choices;
            if(cityInput==completed)showCityChoices(choices,choices.empty()?"No city found.":"");
        } catch(const std::exception& e) {
            saida::Log::info("[World places] search unavailable: ",e.what());
            if(cityInput==completed)showCityChoices({},"Search unavailable. Pick a spot on the globe or in Settings → Destination.");
        }
    }
    if(!menu)return;
    const std::string& query=cityInput;
    if(query.size()<2||query==citySubmitted||cityFuture.valid()||clock-cityChangedAt<.45||
       clock-cityLastRequestAt<1.0)return;
    citySubmitted=query;
    if(const auto cached=cityCache.find(query);cached!=cityCache.end()) {
        showCityChoices(cached->second,cached->second.empty()?"No city found.":"");
        return;
    }
    cityInFlight=query;cityLastRequestAt=clock;
    cityFuture=std::async(std::launch::async,[query]{return r1::searchPlaceChoices(query);});
}

void World::Impl::testClick(const std::string& id,bool release) {
    if(auto* e=ui->findElementById(id))e->GetContext()->Update();
    auto* element=ui->findElementById(id);
    if(!element)throw std::runtime_error("Missing UI target "+id);
    auto p=element->GetAbsoluteOffset();
    ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Move,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::None);
    ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Down,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::Left);
    if(release)ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Up,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::Left);
}

void World::Impl::select(double x,double y) {
    pickLon=wrap(x);pickLat=std::clamp(y,-90.,90.);
    field("latitude",pickLat);field("longitude",pickLon);
    if(globe)globe->selected={pickLon,pickLat};
    updateGlobePin();
}

void World::Impl::updateGlobePin() {
    if(!globe)return;
    const auto pixel=globe->view.project(pickLon,pickLat,ui->width(),ui->height());
    const bool visible=pixel&&pixel->x>16&&pixel->x<ui->width()-16&&pixel->y>200&&pixel->y<ui->height()-130;
    style("pin","display",visible?"block":"none");
    if(visible){style("pin","left",number(pixel->x,2)+"px");style("pin","top",number(pixel->y,2)+"px");}
}

void World::Impl::updateMenuIntro() {
    if(!smoke&&menuIntro.takeStart()) {
        saida::AudioSettings startupSound;
        startupSound.loop=false;
        startupSound.spatialized=false;
        const auto id=saida::AudioManager::get().play("assets/audio/menu/life.mp3",startupSound);
        if(id==saida::kInvalidAudioID)saida::Log::error("[World menu] startup sound life.mp3 could not play");
        else {
            menuIntro.start();
            saida::Log::info("[World menu] startup sound life.mp3 started once after the 3-second delay");
        }
    }
    const auto subtitle=menuIntro.subtitle();
    text("startup-subtitle",std::string(subtitle));
    style("startup-subtitle","display",!subtitle.empty()&&!pending?"block":"none");
}

void World::Impl::updateMenuMusic(double dt) {
    if(smoke)return;
    auto& audio=saida::AudioManager::get();
    // On through the arrival's zoom, giving way to the wind of the fall.
    const bool flying=drop.phase==Drop::Zoom&&drop.clock.seconds<r1::arrival::kZoomSeconds-2.;
    const float target=menu||flying?1.f:0.f;
    if(target>0&&menuMusic==saida::kInvalidAudioID) {
        saida::AudioSettings settings;settings.loop=true;settings.volume=0;
        menuMusic=audio.play(kMenuMusic,settings);
        if(menuMusic==saida::kInvalidAudioID) {
            if(!menuMusicSaid)saida::Log::error("[World menu] music ",kMenuMusic," could not play; the menu stays silent");
            menuMusicSaid=true;return;
        }
    }
    if(menuMusic==saida::kInvalidAudioID)return;
    const float step=float(std::min(dt,.1)/kMenuMusicFade);
    menuMusicGain=target>menuMusicGain?std::min(target,menuMusicGain+step):std::max(target,menuMusicGain-step);
    if(menuMusicGain>0&&!audio.isPlaying(menuMusic))audio.resume(menuMusic);
    audio.setVolume(menuMusic,kMenuMusicVolume*menuMusicGain);
    if(menuMusicGain<=0&&audio.isPlaying(menuMusic))audio.pause(menuMusic);
}

void World::Impl::touchGlobe(){globeIdle=0;globeSpinning=false;}

void World::Impl::spinGlobe(double dt) {
    if(smoke||std::getenv("R1WORLD_MENU_SMOKE")){globeSpinning=false;return;}
    globeIdle+=dt;
    const bool wholeEarth=zoom<=1&&globe->view.zoom<1.01;
    if(!wholeEarth||globeDragging||optionsOpen){if(!wholeEarth)globeSpinning=false;return;}
    if(!globeSpinning&&globeIdle>=kGlobeIdleSeconds)globeSpinning=true;
    if(globeSpinning)globeTarget.lon=wrap(globeTarget.lon+kGlobeSpinDegreesPerSecond*std::min(dt,.1));
}

void World::Impl::updateGlobe(double dt) {
    SAIDA_PROFILE_SCOPE("World/Globe");
    updateMenuMusic(dt);
    updateMenuIntro();
    if(!globe)return;
    globe->visible=menu||dropGlobe();
    if(!globe->visible)return;
    int w=1,h=1;engine.window().size(w,h);
    const auto origin=ui->screenPosition(),size=ui->screenSize();
    globe->viewport={origin.x/w,origin.y/h,size.x/w,size.y/h};
    if(!menu) {
        // The arrival flies it (World::Impl::flyGlobe), drawn as satellite
        // imagery only, fading out into the world.
        flyGlobe();renderMapTiles();
        return;
    }
    globe->opacity=1;globe->vignette=1;
    spinGlobe(dt);
    const double blend=1.-std::exp(-std::min(dt,.1)*10.);
    auto& view=globe->view;
    view.lon=wrap(view.lon+wrap(globeTarget.lon-view.lon)*blend);
    view.lat+=(globeTarget.lat-view.lat)*blend;
    view.zoom=std::exp(std::log(view.zoom)+(std::log(zoom)-std::log(view.zoom))*blend);
    // The Sun moves 0.25 degrees a minute; once a second is smooth.
    if((subsolarAge+=dt)>=1.&&findSun()) {
        json point;
        if(sunScript->callExport("subsolarPoint",json::array(),point)==saida::ScriptCallStatus::Succeeded&&point.is_object()) {
            globe->subsolar={point.value("lon",0.),point.value("lat",0.)};subsolarAge=0;
        } else if(!subsolarRefused) {
            subsolarRefused=true;saida::Log::warn("[World menu] sun_cycle.js gave no subsolar point; the globe keeps its last light");
        }
    }
    updateGlobePin();
    renderMapTiles();
}

void World::Impl::showMap(bool show) {
    if(show&&cheatOpen)closeCheat();
    // The map opened while the globe still flies to a place not yet
    // ready calls the arrival off; once he is in the air it waits there
    // for Resume.
    if(show&&drop.phase!=Drop::None&&!drop.armed) {
        saida::Log::info("[World arrival] called off ",drop.clock.seconds," s after Go: the map is back");
        pending=false;endDrop();
    }
    if(show&&drop.phase!=Drop::None) {
        if(arrivalUi)arrivalUi->setEnabled(false);
        auto& audio=saida::AudioManager::get();
        for(auto id:{windSound,flutterSound})if(id!=saida::kInvalidAudioID&&audio.isPlaying(id))audio.pause(id);
    }
    menu=show;style("menu","display",show?"block":"none");style("hud","display",show?"none":"block");
    optionsOpen=false;style("options-screen","display","none");
    if(globe)globe->visible=show;
    minimapUi->setEnabled(!show);
    if(performanceUi)performanceUi->setEnabled(performanceDebug&&!show);
    style("resume","display",playing?"inline-block":"none");
    engine.window().setCursorCaptured(!show);
    if(!show)applyDropUi();
}

int World::Impl::mapZoomLevel() const {return globe?globe->view.tileLevel(ui->height()):3;}

void World::Impl::fillLayer(MenuTileCache& cache,r1::GlobeLayer& layer,int z,int coarsest,int finest,
                   const r1::GlobeView& target,int targetZ) {
    const auto& view=globe->view;
    const double w=ui->width(),h=ui->height();
    layer.z=z;layer.coarsest=coarsest;
    // Below the first detail level, only prefetch the destination. Asking
    // for level 7 over the whole Earth queued thousands of invisible tiles
    // in front of the arrival's actual images.
    layer.visible=view.tileLevel(h)>=coarsest?view.tiles(w,h,z):std::vector<r1::MapTile>{};
    layer.visible.erase(std::remove_if(layer.visible.begin(),layer.visible.end(),[&](const auto& t){return cache.missingForGood(t);}),layer.visible.end());
    const double n=double(1<<z);
    const double cx=(view.lon+180.)/360.*n;
    const double cy=(1.-std::asinh(std::tan(std::clamp(view.lat,-r1::MapView::mercatorLimit,r1::MapView::mercatorLimit)*r1::kPi/180.))/r1::kPi)*.5*n;
    auto distance=[&](const r1::MapTile& t){const double dx=std::remainder(t.x+.5-cx,n),dy=t.y+.5-cy;return dx*dx+dy*dy;};
    std::sort(layer.visible.begin(),layer.visible.end(),[&](const auto& a,const auto& b){return distance(a)<distance(b);});
    std::vector<r1::MapTile> wanted=layer.visible;
    std::set<std::tuple<int,int,int>> queued;
    for(const auto& t:wanted)queued.insert({t.z,t.x,t.y});
    const size_t limit=wanted.size()+r1::globeTileBudget(toasterMode).prefetchTiles;
    auto add=[&](std::vector<r1::MapTile> more){
        for(const auto& t:more)if(wanted.size()<limit&&queued.insert({t.z,t.x,t.y}).second)wanted.push_back(t);
    };
    // The arrival only prefetches along its path, not unused neighbours.
    const bool passing=dropGlobe();
    if(passing) {
        // Prepare the next camera views, including their GPU uploads,
        // while the current view is displayed. The landing tile alone
        // does not cover the intermediate levels of a ten-second flight.
        for(double ahead:{.75,1.5}) {
            const auto pose=drop.flight.at(std::min(drop.clock.seconds+ahead,r1::arrival::kZoomSeconds));
            r1::GlobeView next{pose.lon,pose.lat,zoomForAltitude(pose.altitude)};
            const int nextZ=next.tileLevel(h);
            if(nextZ>=coarsest)add(next.tiles(w,h,std::min(nextZ,finest)));
        }
    }
    if(targetZ>=coarsest&&targetZ<=finest)add(target.tiles(w,h,targetZ));
    // Adjacent menu levels are useful if already on disk, but reopening a
    // cached view must not trigger speculative network traffic.
    auto downloads=wanted;
    if(!passing&&!layer.visible.empty()&&z+1<=finest)add(view.tiles(w,h,z+1,.5));
    if(!passing&&!layer.visible.empty()&&z-1>=coarsest)add(view.tiles(w,h,z-1));
    layer.cached.clear();
    std::set<std::tuple<int,int,int>> seen;
    auto keep=[&](const r1::MapTile& t){
        if(cache.ready(t)&&seen.insert({t.z,t.x,t.y}).second)layer.cached.push_back({t,cache.path(t)});
    };
    for(const auto& t:layer.visible)keep(t);
    for(const auto& t:layer.visible)
        for(int up=1;z-up>=coarsest;++up)keep({z-up,t.x>>up,t.y>>up});
    for(const auto& t:wanted)keep(t);
    cache.setVisible(std::move(downloads));
}

void World::Impl::renderMapTiles() {
    SAIDA_PROFILE_SCOPE("World/MapTiles");
    if(!globe)return;
    const auto& view=globe->view;
    const double h=ui->height();
    const int level=view.tileLevel(h);
    r1::GlobeView target=view;target.lon=globeTarget.lon;target.lat=globeTarget.lat;target.zoom=zoom;
    // Flying to the drop zone, what it lands on is fetched first.
    const bool arriving=dropGlobe();
    if(arriving){target.lon=drop.plannedLon;target.lat=drop.plannedLat;target.zoom=zoomForAltitude(drop.plannedAbove/r1::kRMean);}
    const int targetLevel=target.tileLevel(h);
    size_t missing=0;bool unavailable=false;
    auto count=[&](MenuTileCache& cache,const r1::GlobeLayer& layer){
        for(const auto& t:layer.visible)if(!cache.ready(t))++missing;
        unavailable=unavailable||cache.unavailable();
    };
    // Imagery once the 8K day map runs out of pixels, or as soon as the
    // wheel is heading there.
    if(std::max(level,targetLevel)>=r1::kImageryFirstLevel) {
        fillLayer(imageryTiles,globe->imagery,std::clamp(level,r1::kImageryFirstLevel,r1::kImageryLastLevel),
                  r1::kImageryFirstLevel,r1::kImageryLastLevel,target,std::min(targetLevel,r1::kImageryLastLevel));
        if(level<r1::kImageryFirstLevel)globe->imagery.z=-1; // fetched ahead, not drawn yet
        else count(imageryTiles,globe->imagery);
    } else {globe->imagery={};imageryTiles.setVisible({});}
    // The arrival shows the Earth as seen from above: imagery, no map.
    const bool osm=view.zoom>=r1::kOsmZoom&&!arriving;
    if(!arriving&&std::max(view.zoom,zoom)>=r1::kOsmZoom/2) {
        fillLayer(menuTiles,globe->map,level,std::max(0,level-6),19,target,targetLevel);
        globe->map.opacity=float(std::clamp(std::log2(view.zoom/r1::kOsmZoom),0.,1.));
        if(!osm)globe->map.z=-1;
        else count(menuTiles,globe->map);
    } else {globe->map={};menuTiles.setVisible({});}
    style("map-detail-status","display",missing?"block":"none");
    if(missing)text("map-detail-status",unavailable?"Detailed map unavailable offline":"Loading the detailed map…");
    const bool imagery=level>=r1::kImageryFirstLevel;
    style("map-attribution","display",osm||imagery?"block":"none");
    if(osm||imagery)text("map-attribution",osm?"© OpenStreetMap contributors":
        "Sentinel-2 cloudless – s2maps.eu by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016)");
}

void World::Impl::zoomMap(double z) {
    zoom=std::clamp(z,1.,131072.);
    mapCenterLon=pickLon;mapCenterLat=pickLat;
    globeTarget.lon=pickLon;globeTarget.lat=pickLat;
    renderMapTiles();
    select(pickLon,pickLat);
}

void World::Impl::ProcessEvent(Rml::Event& event) {
    auto id=event.GetCurrentElement()->GetId();
    const auto type=event.GetType();
    if(type=="mousedown"||type=="mousescroll"||type=="dblclick")touchGlobe();
    if(id=="map"||id=="menu") {
        if(!globe||optionsOpen||!menu)return;
        const double x=event.GetParameter<float>("mouse_x",0),y=event.GetParameter<float>("mouse_y",0);
        if(type=="mousedown"&&event.GetParameter<int>("button",0)==0) {
            globeDragging=globe->view.pick(x,y,ui->width(),ui->height()).has_value();
            globeMoved=false;globePress={x,y};globeDragView=globe->view;
        } else if(type=="mousemove"&&globeDragging) {
            const glm::dvec2 movement=glm::dvec2(x,y)-globePress;
            globeMoved=globeMoved||glm::length(movement)>4.;
            if(globeMoved) {
                const double degrees=globeDragView.altitude()*2.*r1::GlobeView::tangent/ui->height()/rad;
                globeTarget.lon=wrap(globeDragView.lon-movement.x*degrees/std::max(.08,std::cos(globeDragView.lat*rad)));
                globeTarget.lat=std::clamp(globeDragView.lat+movement.y*degrees,-89.8,89.8);
                globe->view.lon=globeTarget.lon;globe->view.lat=globeTarget.lat;
                updateGlobePin();
            }
        } else if(type=="mouseup"&&globeDragging) {
            globeDragging=false;
            if(!globeMoved)if(auto hit=globe->view.pick(x,y,ui->width(),ui->height())) {
                select(hit->lon,hit->lat);field("city-query",std::string());showCityChoices({});
                warming=true;if(!smoke&&!std::getenv("R1WORLD_MENU_SMOKE"))request(pickLon,pickLat);
            }
        } else if(type=="mousescroll") {
            const double wheel=event.GetParameter<float>("wheel_delta_y",0);
            if(wheel<0)if(auto hit=globe->view.pick(x,y,ui->width(),ui->height())) {
                globeTarget.lon=hit->lon;globeTarget.lat=hit->lat;
            }
            zoom=std::clamp(zoom*std::pow(1.6,-wheel),1.,131072.);
            event.StopPropagation();
        } else if(type=="dblclick") {
            if(auto hit=globe->view.pick(x,y,ui->width(),ui->height())) {
                select(hit->lon,hit->lat);zoomMap(zoom*2.);
            }
        }
        return;
    }
    const bool press=event.GetType()=="mousedown";
    if(!press&&id==pressedId){pressedId.clear();return;}
    if(press)pressedId=id;
    if(id=="go")go();
    else if(id=="options")showOptions(true);
    else if(id=="options-back")showOptions(false);
    else if(id=="forced-apply")applyTypedTime();
    else if(id=="forced-clear"){forceTime(std::nullopt);field("forced-time",std::string());}
    else if(id.rfind("forced-preset-",0)==0)forceTime(std::stoi(id.substr(14))*60);
    else if(id=="weather-real")forceWeather(r1::WeatherOverride::Real);
    else if(id=="weather-clear")forceWeather(r1::WeatherOverride::Clear);
    else if(id=="weather-light-rain")forceWeather(r1::WeatherOverride::LightRain);
    else if(id=="weather-storm")forceWeather(r1::WeatherOverride::Storm);
    else if(id=="toaster-off")setToasterMode(false);
    else if(id=="toaster-on")setToasterMode(true);
    else if(id.rfind("gi-quality-",0)==0) {
        for(int q=-1;q<=4;++q)if(id==std::string("gi-quality-")+giQualityKeys[q+1])setGIQuality(q);
    }
    else if(id=="fullscreen-off")setFullscreenMode(false);
    else if(id=="fullscreen-on")setFullscreenMode(true);
    else if(id=="fullscreen")toggleFullscreenMode();
    else if(id=="resume"&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}
    else if(id=="destination-go") {
        // The typed coordinates, from the Destination tab: leave the
        // settings only when they hold; go() says why when they do not.
        if(go())showOptions(false);
    }
    else if(id=="street-map")zoomMap(std::max(zoom,32768.));
    else if(id=="reset-map")zoomMap(1);
    else if(id.rfind("city-choice-",0)==0) {
        const int choice=id.back()-'0';
        if(choice>=0&&choice<int(cityChoices.size())) {
            const auto selected=cityChoices[size_t(choice)];
            select(selected.lon,selected.lat);zoomMap(std::max(zoom,2048.));
            field("city-query",selected.label);
            cityInput=citySubmitted=selected.label;
            showCityChoices({});
            warming=true;if(!smoke)request(pickLon,pickLat);
        }
    }
    else {std::map<std::string,glm::dvec2> places{{"paris",{2.3522,48.8566}},{"tokyo",{139.7671,35.6812}},{"newyork",{-73.9855,40.758}},{"lawrence",{-95.2436,38.9585}},{"cape",{18.4241,-33.9249}},{"sydney",{151.2093,-33.8688}},{"pole",{0.,90.}}};
        if(places.count(id)){auto p=places.at(id);select(p.x,p.y);zoomMap(zoom);
            field("city-query",std::string());showCityChoices({});
            warming=true;request(pickLon,pickLat);}}
}

bool World::Impl::refreshInterface() {
    if(generation!=ui->documentGeneration()||listeners.empty()) {
        generation=ui->documentGeneration();listeners.clear();
        written.clear();
        for(auto id:{"go","destination-go","resume","street-map","reset-map","paris","tokyo","newyork","lawrence","cape","sydney","pole",
                     "city-choice-0","city-choice-1","city-choice-2","city-choice-3","city-choice-4",
                     "options","options-back","forced-apply","forced-clear",
                     "weather-real","weather-clear","weather-light-rain","weather-storm",
                     "toaster-off","toaster-on",
                     "gi-quality-off","gi-quality-auto","gi-quality-low","gi-quality-medium","gi-quality-high","gi-quality-ultra",
                     "fullscreen-off","fullscreen","fullscreen-on",
                     "forced-preset-8","forced-preset-12","forced-preset-17","forced-preset-22"})
            if(auto* e=ui->findElementById(id)){e->AddEventListener("click",this);
                e->AddEventListener("mousedown",this);listeners.push_back(e);}
        for(auto id:{"map","menu"})if(auto* e=ui->findElementById(id)) {
            if(std::string(id)=="map")for(auto type:{"mousedown","mousescroll","dblclick"})e->AddEventListener(type,this);
            for(auto type:{"mousemove","mouseup"})e->AddEventListener(type,this);
            listeners.push_back(e);
        }
        if(!listeners.empty()) {
            select(pickLon,pickLat);
            const bool menuSmoke=std::getenv("R1WORLD_MENU_SMOKE")!=nullptr;
            if((smoke||menuSmoke)&&!smokeZoom())return false;
            if(menuSmoke){engine.sceneTree().quit();return false;}
            // A headset has no mouse for the globe: go straight to --spawn.
            if(vr&&!smoke){go();saida::Log::info("[World VR] menu skipped, travelling to ",pickLon,", ",pickLat);}
            if(smoke) {
                double sx=pickLon,sy=pickLat;
                // A press and release on the globe picks the point under
                // it, through the same projection the globe is drawn with.
                {
                    const double cx=ui->width()*.5,cy=ui->height()*r1::GlobeView::centreY;
                    if(auto* map=ui->findElementById("map"))map->GetContext()->Update();
                    const auto expected=globe->view.pick(cx,cy,ui->width(),ui->height());
                    ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Move,int(cx),int(cy),saida::WebCanvasNode::MouseButton::None);
                    ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Down,int(cx),int(cy),saida::WebCanvasNode::MouseButton::Left);
                    ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Up,int(cx),int(cy),saida::WebCanvasNode::MouseButton::Left);
                    if(!expected||std::abs(std::remainder(pickLon-expected->lon,360.))>1e-3||std::abs(pickLat-expected->lat)>1e-3){
                        saida::Log::error("[World E2E] FAIL globe click picks ",pickLon,", ",pickLat);testFailed=true;engine.sceneTree().quit();return false;
                    }
                }
                select(sx,sy);
                showOptions(true);ui->executeJS("showSettingsTab('destination');");
                testClick("street-map",false);
                ui->executeJS("showSettingsTab('time');");showOptions(false);
                if(zoom<32768.){
                    saida::Log::error("[World E2E] FAIL street view zoom ",zoom);testFailed=true;engine.sceneTree().quit();return false;
                }
                zoomMap(1);
                showCityChoices({{"Paris","Paris · France",2.3522,48.8566}});
                testClick("city-choice-0",false);
                if(std::abs(pickLon-2.3522)>1e-5||std::abs(pickLat-48.8566)>1e-5||
                   value("longitude")!=number(2.3522)||value("latitude")!=number(48.8566)) {
                    saida::Log::error("[World E2E] FAIL city suggestion did not fill destination");
                    testFailed=true;engine.sceneTree().quit();return false;
                }
                field("city-query",std::string());select(sx,sy);
                // The press alone must start the journey.
                goCount=0;testClick("go",false);
                if(!pending||goCount!=1){saida::Log::error("[World E2E] FAIL Go press, pending=",pending," calls=",goCount);testFailed=true;engine.sceneTree().quit();return false;}
                if(smokeDive) {
                    // The press starts the arrival and the menu goes with it.
                    if(drop.phase!=Drop::Zoom){saida::Log::error("[World E2E] FAIL Go did not start the arrival");testFailed=true;engine.sceneTree().quit();return false;}
                    saida::Log::info("[World E2E] map click and Go passed: the globe flies to the drop zone");
                    return false;
                }
                // A full press+release must still act exactly once.
                testClick("go");
                if(goCount!=2){saida::Log::error("[World E2E] FAIL Go press+release ran ",goCount," times");testFailed=true;engine.sceneTree().quit();return false;}
                saida::Log::info("[World E2E] map click and Go passed (press-only and press+release)");
                // Last: the screen it hides is still laid out over the menu
                // until the next frame, as after any menu change (hopArmed).
                if(!smokeOptions())return false;
            }
        }
    }
    return true;
}

} // namespace r1::world
