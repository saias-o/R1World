#include "session.hpp"

namespace r1::world {
namespace {
void reportOptions(OptionsStore::Level level, const std::string& message) {
    switch(level) {
    case OptionsStore::Level::Info: saida::Log::info("[World options] ",message);break;
    case OptionsStore::Level::Warning: saida::Log::warn("[World options] ",message);break;
    case OptionsStore::Level::Error: saida::Log::error("[World options] ",message);break;
    }
}
} // namespace

bool World::Impl::testRun() const {return smoke||!worldCapture.pngPath.empty();}

void World::Impl::loadOptions() {
    const auto saved=OptionsStore(game,reportOptions).load(
        {forcedMinutes,forcedWeather,toasterMode,giQuality,false},testRun());
    forcedMinutes=saved.forcedMinutes;
    forcedWeather=saved.forcedWeather;
    toasterMode=saved.toasterMode;
    giQuality=saved.giQuality;
    // Loading a false/missing preference never forces an existing fullscreen
    // window out of that mode, matching the established startup behaviour.
    if(saved.fullscreen)engine.window().setFullscreen(true);
}

void World::Impl::saveOptions() {
    if(testRun())return;
    const SavedOptions saved{forcedMinutes,forcedWeather,toasterMode,giQuality,engine.window().isFullscreen()};
    if(!OptionsStore(game,reportOptions).save(saved)) {
        sayForcedTime("Option applied, but it could not be saved: see game.log.");
        sayForcedWeather("Option applied, but it could not be saved: see game.log.");
        sayToasterMode("Option applied, but it could not be saved: see game.log.");
        sayGIQuality("Option applied, but it could not be saved: see game.log.");
        sayFullscreen("Option applied, but it could not be saved: see game.log.");
    }
}

void World::Impl::sayForcedTime(const std::string& problem) {
    if(!problem.empty()){text("options-status",problem);return;}
    text("options-status",forcedMinutes
        ?"Everywhere in the world it is "+r1::formatClockTime(*forcedMinutes)+" local time. Time no longer passes."
        :"Real time: every place is at its current hour.");
}

void World::Impl::sayForcedWeather(const std::string& problem) {
    for(const auto mode:{r1::WeatherOverride::Real,r1::WeatherOverride::Clear,
                         r1::WeatherOverride::LightRain,r1::WeatherOverride::Storm}) {
        const std::string id=std::string("weather-")+r1::weatherOverrideKey(mode);
        if(auto* button=ui->findElementById(id))button->SetClass("selected",mode==forcedWeather);
    }
    ui->notifyJsMutation();
    text("weather-options-status",!problem.empty()?problem:
        forcedWeather==r1::WeatherOverride::Real?"Real weather: every place follows its local forecast.":
        std::string("Everywhere in the world: ")+r1::weatherOverrideLabel(forcedWeather)+". This choice stays after you travel.");
}

void World::Impl::sayToasterMode(const std::string& problem) {
    if(auto* off=ui->findElementById("toaster-off"))off->SetClass("selected",!toasterMode);
    if(auto* on=ui->findElementById("toaster-on"))on->SetClass("selected",toasterMode);
    ui->notifyJsMutation();
    text("toaster-options-status",!problem.empty()?problem:
        toasterMode?"Toaster mode on: 720p, GI off, mid/far tree LODs, closer fog."
                   :"Standard quality: normal graphics and draw distance.");
}

void World::Impl::sayFullscreen(const std::string& problem) {
    const bool fs=engine.window().isFullscreen();
    if(auto* off=ui->findElementById("fullscreen-off"))off->SetClass("selected",!fs);
    if(auto* on=ui->findElementById("fullscreen"))on->SetClass("selected",fs);
    if(auto* on2=ui->findElementById("fullscreen-on"))on2->SetClass("selected",fs);
    ui->notifyJsMutation();
    text("fullscreen-options-status",!problem.empty()?problem:
        fs?"Full screen mode on: press F11 or click Windowed to exit."
          :"Windowed mode: press F11 or click Full screen.");
}

void World::Impl::setFullscreenMode(bool enabled) {
    engine.window().setFullscreen(enabled);
    saida::Log::info("[World options] full screen ",engine.window().isFullscreen()?"enabled":"disabled");
    sayFullscreen();
    saveOptions();
}

void World::Impl::toggleFullscreenMode() {
    setFullscreenMode(!engine.window().isFullscreen());
}

void World::Impl::applyGIQuality() {
    auto& settings=engine.sceneTree().world().settings();
    settings.giEnabled=!toasterMode && giQuality>=0;
    settings.giQuality=static_cast<saida::GIQuality>(std::max(0,giQuality));
    settings.giMode=saida::GIMode::FullRealtime;
}

void World::Impl::sayGIQuality(const std::string& problem) {
    for(int q=-1;q<=4;++q)
        if(auto* button=ui->findElementById(std::string("gi-quality-")+giQualityKeys[q+1]))
            button->SetClass("selected",q==giQuality);
    ui->notifyJsMutation();
    static constexpr const char* descriptions[]={
        "Off: no indirect lighting.",
        "Auto: performance budget chosen for your graphics card.",
        "Low: half-resolution lighting, reduced detail and update budget.",
        "Medium: half-resolution lighting, more detail and faster updates.",
        "High: per-pixel lighting, smoother interpolation and faster updates.",
        "Ultra: finest voxels, per-pixel lighting, every probe updated each frame. Very high GPU cost."};
    text("gi-options-status",!problem.empty()?problem:toasterMode
        ?"GI is off in Toaster mode. Your choice is kept for Standard quality."
        :descriptions[giQuality+1]);
}

void World::Impl::setGIQuality(int quality) {
    if(quality<-1 || quality>4)return;
    giQuality=quality;
    applyGIQuality();sayGIQuality();saveOptions();
    saida::Log::info("[World options] GI quality ",giQualityKeys[giQuality+1]);
}

void World::Impl::applyToasterResolution() {
    applyGIQuality();
    if(toasterMode) {
        int w=0,h=0;
        engine.window().size(w,h);
        if(w>0&&h>0&&(w!=1280||h!=720)){standardWidth=w;standardHeight=h;}
        engine.window().setSize(1280,720);
    } else {
        int targetW=standardWidth>0?standardWidth:1600;
        int targetH=standardHeight>0?standardHeight:900;
        if(targetW==1280&&targetH==720){targetW=1600;targetH=900;}
        engine.window().setSize(targetW,targetH);
    }
}

void World::Impl::applyGpuBudget() {
    const uint64_t mib=toasterMode?512:3072;
    engine.resources().setGpuBudget(mib*1024*1024);
    saida::Log::info("[World options] GPU budget ",mib," MiB");
}

void World::Impl::applyToasterLods() {
    for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles){
        for(Plant& plant:t.vegetation){
            if(plant.lod)plant.lod->setFinestLevel(toasterMode?1:(plant.close?0:1));
        }
    }
}

void World::Impl::setToasterMode(bool enabled) {
    toasterMode=enabled;
    saida::Log::info("[World options] toaster mode ",toasterMode?"enabled":"disabled");
    applyToasterResolution();
    applyToasterLods();
    applyGpuBudget();
    if(globe)globe->toaster=toasterMode;
    applyWeather();
    sayToasterMode();
    sayGIQuality();
    sayFullscreen();
    saveOptions();
}

void World::Impl::forceWeather(r1::WeatherOverride mode) {
    forcedWeather=mode;
    saida::Log::info("[World options] weather mode ",r1::weatherOverrideKey(mode)," everywhere");
    sayForcedWeather();
    saveOptions();
    if(findSun()&&!applyWeather()) {
        saida::Log::error("[World options] scripts/sun_cycle.js refused forced weather");
        sayForcedWeather("The sky refused the chosen weather: see game.log.");
    }
    updateSnowCover();
    updateSnow(0.);
    updateRain(0.);
    text("local-conditions",weatherLabel());
}

void World::Impl::showOptions(bool show) {
    sayGIQuality();
    optionsOpen=show;
    globeDragging=false;
    style("options-screen","display",show?"block":"none");
    style("menu","display",show||!menu?"none":"block");
    if(auto* e=ui->findElementById(show?"options-back":"options"))e->Focus();
    if(!show)return;
    field("forced-time",forcedMinutes?r1::formatClockTime(*forcedMinutes):std::string());
    sayForcedTime();
    sayForcedWeather();
    sayToasterMode();
}

void World::Impl::forceTime(std::optional<int> minutes) {
    forcedMinutes=minutes;forcedSaid=false;
    saida::Log::info("[World options] ",minutes?"time forced to "+r1::formatClockTime(*minutes)+" local":std::string("real time"));
    sayForcedTime();
    saveOptions();
    applyForcedTime();
}

void World::Impl::applyTypedTime() {
    const std::string typed=value("forced-time");
    if(const auto minutes=r1::parseClockTime(typed)){forceTime(minutes);field("forced-time",r1::formatClockTime(*minutes));return;}
    saida::Log::info("[World options] refused forced time '",typed,"': not HH:MM between 00:00 and 23:59");
    sayForcedTime("Invalid time \""+typed+"\": type HH:MM, from 00:00 to 23:59 (for example 14:00).");
}

std::string World::Impl::weatherLabel() {
    if(forcedWeather!=r1::WeatherOverride::Real&&!inspectAt)
        return std::string("forced weather: ")+r1::weatherOverrideLabel(forcedWeather)+" · everywhere";
    if(!localConditions())return "Local weather unavailable";
    const auto weather=conditions.value("weather",json());
    if(!weather.is_object())return "Local weather unavailable";
    const int code=weather.value("code",-1);
    const char* state=code==0?"clear":code<=3?"cloudy":code<=48?"fog"
                      :code<=67?"rain":code<=77?"snow":code<=82?"showers":"storm";
    return std::string("forecast: ")+state+" · "+number(weather.value("temperature",0.),0)+" °C"
         +" · "+number(weather.value("cloudCover",0.),0)+" % cloud";
}

} // namespace r1::world
