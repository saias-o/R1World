#include "session.hpp"

namespace r1::world {
bool World::Impl::findSun() {
    if(!sunScript) {
        auto* node=engine.sceneTree().firstInGroup("sun");
        if(!node)return false;
        for(auto& b:node->behaviours())
            if(auto* script=dynamic_cast<saida::ScriptBehaviour*>(b.get()))sunScript=script;
    }
    return sunScript!=nullptr;
}

bool World::Impl::tellSun() {
    if(!findSun())return false;
    nlohmann::json result;
    if(!worldCapture.pngPath.empty()) {
        if(sunScript->callExport("setInspectionMode",inspectAt?json::array({true,*inspectAt}):json::array({true}),result)
                !=saida::ScriptCallStatus::Succeeded)return false;
    }
    if(sunScript->callExport("setObserver",json::array({lon,lat,alt}),result)
           !=saida::ScriptCallStatus::Succeeded)return false;
    if(!result.is_boolean()||!result.get<bool>())return false;
    return applyWeather();
}

void World::Impl::tellPlanet() {
    const double p=lat*rad,l=lon*rad,w=1.0-r1::kE2*std::sin(p)*std::sin(p);
    const double radius=std::sqrt(r1::kA/std::sqrt(w)*r1::kA*(1.0-r1::kE2)/(w*std::sqrt(w)));
    const glm::dvec3 normal(std::cos(p)*std::cos(l),std::cos(p)*std::sin(l),std::sin(p));
    auto& settings=engine.sceneTree().world().settings();
    settings.fogPlanetRadius=float(radius);
    settings.fogPlanetCentre=glm::vec3(origin.local(ecef(lon,lat,0.)-normal*radius));
}

void World::Impl::moveSun() {
    tellPlanet();
    // A failure here is a scene that keeps lighting the wrong hemisphere
    // rather than a crash, so it has to be said out loud once.
    if(tellSun()||sunReported)return;
    sunReported=true;
    saida::Log::error("[World] the sun cycle did not accept the observer or weather; "
                      "the light stays on the scene's opening instant");
}

bool World::Impl::gameTime(double& out) {
    if(!sunScript)return false;
    json result;
    if(sunScript->callExport("gameTime",json::array(),result)!=saida::ScriptCallStatus::Succeeded
       ||!result.is_number())return false;
    out=result.get<double>();
    return true;
}

bool World::Impl::localConditions() const {
    return conditions.is_object()&&conditions.contains("lon")&&conditions.contains("lat")
        &&r1::weatherLocal(conditions.value("lon",1000.),conditions.value("lat",1000.),lon,lat);
}

World::Impl::Weather World::Impl::effectiveWeather() const {
    auto sample=r1::resolveWeather(observedWeather,localConditions(),forcedWeather,
        inspectAt?std::optional<std::array<double,3>>(inspectWeather):std::nullopt);
    if(inspectTemperature){sample.temperature=*inspectTemperature;sample.temperatureKnown=true;}
    return sample;
}

double World::Impl::number(const json& j,const char* key,double fallback) {
    auto it=j.find(key);return it!=j.end()&&it->is_number()?it->get<double>():fallback;
}

void World::Impl::readConditions() {
    auto latest=sky->latest();
    if(!latest||latest==conditionsDoc)return;
    conditionsDoc=latest;conditions=*latest;
    const auto w=conditions.value("weather",json());
    observedWeather=Weather{};
    if(w.is_object()) {
        observedWeather.known=true;
        observedWeather.temperature=number(w,"temperature",0.);
        observedWeather.temperatureKnown=w.contains("temperature")&&w["temperature"].is_number()&&
            std::isfinite(observedWeather.temperature);
        observedWeather.cover=std::clamp(number(w,"cloudCover",0.)/100.,0.,1.);
        observedWeather.rain=r1::liquidRainMmPerHour(w);
        observedWeather.visibility=number(w,"visibility",0.);
        observedWeather.windSpeed=std::max(0.,number(w,"windSpeed",0.));
        observedWeather.windFrom=number(w,"windFrom",0.);
        observedWeather.snowfall=std::max(0.,number(w,"snowfall",0.));
        observedWeather.snowDepth=std::max(0.,number(w,"snowDepth",0.));
        observedWeather.code=int(number(w,"code",-1.));
    }
    applyWeather();
}

bool World::Impl::applyWeather() {
    if(!sunScript)return false;
    fogFar=farPack.node!=nullptr;
    fogToaster=toasterHaze();
    const auto weather=effectiveWeather();
    double seen=weather.visibility>0&&(inspectAt||weather.visibility<kVisibilityCeiling)?weather.visibility:0.;
    if(fogToaster) {
        constexpr double kToasterVisibility=650.;
        seen=seen>0.?std::min(seen,kToasterVisibility):kToasterVisibility;
    }
    const double rain=weather.rain;
    const double cloud=r1::rainCloudCover(weather.cover,weather.code,rain);
    json result;
    // From the menu, before any tile is resident, the ground is not known
    // yet: the observer's altitude stands in, and tellSun() sends the real
    // ground at the spawn. Asking `height` there threw "Missing terrain".
    const double ground=tile(lon,lat)?height(lon,lat):alt;
    const bool accepted=sunScript->callExport("setWeather",json::array({cloud,rain,seen,ground}),result)
        ==saida::ScriptCallStatus::Succeeded&&result.is_boolean()&&result.get<bool>();
    if(inspectAt&&accepted)saida::Log::info("[World inspection] cloud=",cloud," rain_mm_h=",rain," visibility_m=",seen);
    return accepted;
}

glm::vec3 World::Impl::weatherWind() const {
    const auto weather=effectiveWeather();
    const double to=(weather.windFrom+180.)*rad;
    const glm::dvec3 local(std::sin(to)*weather.windSpeed,0.,-std::cos(to)*weather.windSpeed);
    // Bearings belong to the observer's tangent frame, including at poles;
    // render coordinates belong to the last floating origin instead.
    return glm::vec3(glm::transpose(origin.basis)*Frame(lon,lat,alt).basis*local);
}

void World::Impl::clearRain() {
    if(rainfall)rainfall->setEnabled(false);
    rainState.reset();rainShelterAge=1e9;rainSheltered=false;
    auto& settings=engine.sceneTree().world().settings();
    settings.rainIntensity=0.f;settings.wetness=0.f;settings.stormCloudCover=0.f;
}

void World::Impl::updateRain(double dt) {
    const auto weather=effectiveWeather();
    const bool active=playing&&!pending&&weather.known;
    const double rate=weather.rain;
    rainState.update(dt,rate,active);
    auto& settings=engine.sceneTree().world().settings();
    settings.rainIntensity=float(rainState.intensity);
    settings.wetness=float(rainState.wetness);
    const double storm=active&&rate>0.?r1::rainCloudCover(weather.cover,weather.code,rate):0.;
    settings.stormCloudCover+=float((storm-settings.stormCloudCover)*(1.-std::exp(-std::max(0.,dt)/2.)));
    if(storm==0.&&settings.stormCloudCover<.0001f)settings.stormCloudCover=0.f;
    if(!active){clearRain();return;}
    const glm::vec3 up=glm::normalize(glm::vec3(glm::transpose(origin.basis)*
        (ecef(lon,lat,alt+1.)-ecef(lon,lat,alt))));
    settings.rainUpDirection=up;
    const glm::vec3 eye=camera->transform().position;
    rainShelterAge+=dt;
    if(rate>0.&&(rainShelterAge>.15||glm::length(eye-rainShelterEye)>1.f)) {
        rainShelterAge=0.;rainShelterEye=eye;
        auto* physics=engine.sceneTree().world().physics();
        rainSheltered=physics&&physics->raycast(eye+up*.1f,up,120.f,obstacleFilter()).hit;
    }
    if(rate<=0.||rainSheltered||menu) {
        if(rainfall)rainfall->setEnabled(false);
        return;
    }
    if(daylightAge>.5&&sunScript) {
        json result;daylightAge=0.;
        if(sunScript->callExport("daylight",json::array(),result)==saida::ScriptCallStatus::Succeeded&&result.is_number())
            daylightNow=std::clamp(result.get<double>(),0.,1.);
    }
    if(!rainfall) {
        auto node=std::make_unique<saida::ParticleSystemNode>();node->setName("rainfall");
        node->effectClass=saida::ParticleSystemNode::EffectClass::Rain;node->applyEffectPreset();
        rainfall=static_cast<saida::ParticleSystemNode*>(engine.sceneTree().world().addChild(std::move(node)));
        saida::Log::info("[World weather] liquid rain, ",rate," mm/h");
    }
    const glm::vec3 wind=weatherWind();
    const glm::vec3 drift=wind*.55f;
    rainfall->setEnabled(true);
    rainfall->maxParticles=1800;rainfall->lifetime=1.1f;
    rainfall->shape=saida::ParticleSystemNode::Shape::Box;rainfall->boxExtents={14.f,2.f,14.f};
    rainfall->radius=14.f;rainfall->spawnRate=float(std::clamp(90.+rate*140.,90.,1500.));
    rainfall->startSize=.035f;rainfall->stretch=22.f;rainfall->endSizeScale=1.f;
    rainfall->startColor={.75f,.81f,.88f,.38f};rainfall->endColor={.75f,.81f,.88f,.08f};
    rainfall->emissive=float(.035+.7*daylightNow);rainfall->drag=0.f;
    rainfall->gravity=-up*1.5f;
    rainfall->useInitialVelocity=true;rainfall->initialVelocity=-up*12.f+drift;
    rainfall->transform().position=eye+up*9.f-drift*.65f;
}

void World::Impl::keepStanding() {
    checkStanding=false;
    if(!playing||pending||driving||sailing||piloting||swimming||dropFlying()||!tile(lon,lat))return;
    // The finer survey (IGN) may have replaced the quick one under him.
    // A breakwater the feet stand on is not in the grid: keep what holds him.
    if(jumpOffset<=0) {
        const double ground=height(lon,lat,alt);
        alt=feet->isOnFloor()&&alt>ground?alt:ground;
    }
    if(!footBlocked(lon,lat,alt))return;
    double x,y;
    if(!freeSpot(lon,lat,40.,x,y))return;
    saida::Log::info("[World] a building arrived where the player stood: moved to ",x,", ",y);
    lon=x;lat=y;alt=height(lon,lat);jumpOffset=jumpVelocity=0;
    text("stream-status","The buildings have arrived: here you are in the street.");
}

saida::Material* World::Impl::snowMaterial(bool doubleSided) {
    const r1::Swatch& s=r1::palette().snow;
    return material(r1::surfaceMaterial(s.name,s.color,s.roughness,std::string("snow"),doubleSided));
}

bool World::Impl::snowable(const std::string& name) {
    const bool ground=name.rfind("Ground \xE2\x80\x94 ",0)==0,roof=name.rfind("Roofs \xE2\x80\x94 ",0)==0;
    if(!ground&&!roof)return false;
    for(const char* keep:{"Water","Snow","Glacier","Frosted","sea ice","Melt pond","Young grey"})
        if(name.find(keep)!=std::string::npos)return false;
    return true;
}

void World::Impl::updateSnowCover() {
    const auto weather=effectiveWeather();
    const bool want=weather.known&&weather.snowDepth>=.03;
    for(auto& [key,t]:loaded) {
        if(!t.geography)continue;
        if(t.snowed&&!want) {
            for(auto& [node,own]:t.bare)node->setMaterial(own);
            t.bare.clear();t.snowed=false;t.snowSeen=0;
            continue;
        }
        if(!want)continue;
        if(!t.snowed){t.snowed=true;t.snowSeen=0;}
        const auto& children=t.geography->children();
        for(;t.snowSeen<children.size();++t.snowSeen) {
            auto* mesh=dynamic_cast<saida::MeshNode*>(children[t.snowSeen].get());
            if(!mesh||!snowable(mesh->name())||!mesh->material())continue;
            t.bare.push_back({mesh,mesh->material()});
            mesh->setMaterial(snowMaterial(mesh->material()->desc().doubleSided));
        }
    }
}

saida::ParticleSystemNode* World::Impl::emitter(const char* name) {
    auto n=std::make_unique<saida::ParticleSystemNode>();
    n->setName(name);
    n->effectClass=saida::ParticleSystemNode::EffectClass::Snow;
    n->applyEffectPreset();
    return static_cast<saida::ParticleSystemNode*>(engine.sceneTree().world().addChild(std::move(n)));
}

void World::Impl::updateSnow(double dt) {
    const auto weather=effectiveWeather();
    const bool here=playing&&!pending&&weather.known;
    const bool falling=here&&((weather.code>=71&&weather.code<=77)||weather.code==85||weather.code==86||weather.snowfall>.02);
    const bool lying=onSeaIce(lon,lat)||weather.snowDepth>.05;
    const bool blowing=here&&lying&&weather.windSpeed>6.;
    daylightAge+=dt;
    if((falling||blowing)&&daylightAge>.5&&sunScript) {
        json result;daylightAge=0;
        if(sunScript->callExport("daylight",json::array(),result)==saida::ScriptCallStatus::Succeeded&&result.is_number())
            daylightNow=std::clamp(result.get<double>(),0.,1.);
    }
    // The wind blows towards where it does not come from; x east, z south.
    const glm::vec3 wind=weatherWind();
    const glm::vec3 eye=camera->transform().position;
    const float light=float(.04+.9*daylightNow);
    if(falling) {
        if(!snowfall){snowfall=emitter("snowfall");saida::Log::info("[World weather] snow falling, ",weather.snowfall," cm/h");}
        snowfall->setEnabled(true);
        snowfall->maxParticles=2400;snowfall->lifetime=9.f;snowfall->radius=22.f;
        snowfall->spawnRate=float(std::clamp(160.+420.*weather.snowfall,160.,700.));
        snowfall->drag=.35f;
        // Terminal drift = acceleration / drag: the flakes move with the wind.
        snowfall->gravity=wind*snowfall->drag+glm::vec3(0,-.45f,0);
        snowfall->emissive=light;
        snowfall->transform().position=eye+glm::vec3(0,9.f,0)-wind*4.f;
    } else if(snowfall)snowfall->setEnabled(false);
    if(blowing) {
        if(!drift){drift=emitter("drifting snow");saida::Log::info("[World weather] snow drifting, wind ",weather.windSpeed," m/s");}
        drift->setEnabled(true);
        drift->maxParticles=1600;drift->lifetime=1.8f;drift->radius=14.f;
        drift->spawnRate=float(std::clamp((weather.windSpeed-6.)*140.,40.,900.));
        drift->startSize=.035f;drift->startSpeed=.3f;drift->stretch=3.f;drift->drag=1.5f;
        drift->startColor=glm::vec4(.95f,.97f,1.f,.45f);drift->endColor=glm::vec4(.95f,.97f,1.f,0.f);
        drift->gravity=wind*drift->drag+glm::vec3(0,-.04f,0);
        drift->emissive=light;
        const glm::vec3 feet=player->transform().position;
        drift->transform().position=glm::vec3(eye.x,feet.y+.3f,eye.z)-wind*.8f;
    } else if(drift)drift->setEnabled(false);
}

int World::Impl::utcOffset(bool& known) {
    known=localConditions()&&conditions.value("timeSource","")=="zone";
    return known?conditions.value("utcOffsetSeconds",0):int(std::round(lon/15.))*3600;
}

std::string World::Impl::localClock() {
    double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
    bool known=false;const int offset=utcOffset(known);
    const std::time_t local=std::time_t(unixSeconds)+offset;
    std::tm parts{};gmtime_s(&parts,&local);
    std::ostringstream out;out<<std::put_time(&parts,"%H:%M");
    return out.str()+(known?" · local time":" · estimated time zone")+(forcedMinutes?" (forced)":"");
}

void World::Impl::applyForcedTime() {
    std::optional<double> wanted;
    if(forcedMinutes) {
        bool known=false;const int offset=utcOffset(known);
        wanted=r1::forcedInstant(double(std::time(nullptr)),offset,*forcedMinutes);
    }
    if(wanted==forcedSent||!findSun())return;
    json result;
    const bool accepted=sunScript->callExport("setForcedTime",json::array({wanted?json(*wanted):json(nullptr)}),result)
            ==saida::ScriptCallStatus::Succeeded&&result.is_boolean()&&result.get<bool>();
    if(!accepted) {
        if(!forcedSaid) {
            saida::Log::error("[World options] scripts/sun_cycle.js refused setForcedTime(",
                              wanted?number(*wanted,0):std::string("null"),")");
            sayForcedTime("The Sun refused the forced time: see game.log.");
            forcedSaid=true;
        }
        return;
    }
    forcedSent=wanted;forcedSaid=false;
}

} // namespace r1::world
