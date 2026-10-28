// Persistence contract only: no Engine, window or user cache is accessed.
#include "world/options_store.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using r1::world::OptionsStore;
using r1::world::SavedOptions;

int main() {
    const auto root=fs::temp_directory_path()/
        ("r1world-options-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    struct Cleanup {
        fs::path path;
        ~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}
    } cleanup{root};
    std::vector<std::pair<OptionsStore::Level,std::string>> messages;
    OptionsStore store(root,[&](auto level,const auto& message){messages.emplace_back(level,message);});
    const auto file=root/"cache/options.json";
    const auto write=[&](const std::string& content){std::ofstream out(file);out<<content;assert(out.good());};
    const auto read=[&](){std::ifstream in(file);return std::string(std::istreambuf_iterator<char>(in),{});};

    const auto defaults=store.load();
    assert(!defaults.forcedMinutes && defaults.forcedWeather==r1::WeatherOverride::Real);
    assert(!defaults.toasterMode && !defaults.fullscreen && defaults.giQuality==0);
    assert(!fs::exists(file));
    SavedOptions desired{14*60+30,r1::WeatherOverride::Storm,true,4,true};
    assert(store.save(desired,true));
    assert(!fs::exists(root/"cache")); // Tests must not create even an empty player cache.
    assert(store.save(desired));
    auto saved=store.load();
    assert(saved.forcedMinutes==desired.forcedMinutes && saved.forcedWeather==desired.forcedWeather);
    assert(saved.toasterMode && saved.fullscreen && saved.giQuality==4);
    auto document=json::parse(read());
    assert(document["forcedTime"]=="14:30");
    document["futureOption"]={{"nested",json::array({1,2,3})}};
    write(document.dump());
    desired.forcedMinutes.reset();desired.giQuality=-1;desired.fullscreen=false;
    assert(store.save(desired));
    auto updated=json::parse(read());
    assert(!updated.contains("forcedTime") && updated["giQuality"]==-1 && updated["fullscreen"]==false);
    assert(updated["futureOption"]==document["futureOption"]);

    const auto before=read();
    const auto timestamp=fs::last_write_time(file);
    const auto ignored=store.load({},true);
    assert(!ignored.forcedMinutes && !ignored.toasterMode && ignored.giQuality==0);
    assert(store.save({},true));
    assert(read()==before && fs::last_write_time(file)==timestamp);

    messages.clear();
    write(R"({"forcedTime":13,"forcedWeather":"unknown","toasterMode":"yes","fullscreen":1,"giQuality":5})");
    const SavedOptions current{75,r1::WeatherOverride::Clear,true,2,true};
    saved=store.load(current);
    assert(saved.forcedMinutes==75 && saved.forcedWeather==r1::WeatherOverride::Clear);
    assert(saved.toasterMode && saved.fullscreen && saved.giQuality==2);
    assert(messages.size()==5);
    for(const auto& message:messages)assert(message.first==OptionsStore::Level::Error);
    write(R"({"forcedTime":"25:00","giQuality":1.5,"forcedWeather":"light-rain","fullscreen":false})");
    saved=store.load(current);
    assert(!saved.forcedMinutes && saved.giQuality==2);
    assert(saved.forcedWeather==r1::WeatherOverride::LightRain && !saved.fullscreen);
    write(R"({"forcedTime":"7h05","giQuality":0})");
    assert(store.load().forcedMinutes==7*60+5);

    for(const auto malformed:{"{broken", "[]"}) {
        write(malformed);messages.clear();
        saved=store.load(current);
        assert(saved.forcedMinutes==current.forcedMinutes && saved.giQuality==2);
        assert(messages.size()==1 && messages[0].first==OptionsStore::Level::Error);
        assert(store.save(desired));
        assert(messages[1].first==OptionsStore::Level::Warning);
        assert(json::parse(read()).is_object());
    }
    // A blocked directory reports the write failure to the caller and logger.
    const auto blocked=root/"blocked";
    fs::create_directory(blocked);
    {std::ofstream out(blocked/"cache");out<<"not a directory";}
    bool reported=false;
    OptionsStore unwritable(blocked,[&](auto level,const auto& message){
        reported|=level==OptionsStore::Level::Error && message.find("could not write")!=std::string::npos;
    });
    assert(!unwritable.save(desired) && reported);
    std::cout<<"PASS options store: round trip, validation, unknown keys, test isolation and I/O failures\n";
}
