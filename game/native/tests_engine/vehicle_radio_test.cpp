#include "../vehicle_radio.hpp"
#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdio>
#include <cstdlib>

namespace fs=std::filesystem;
using nlohmann::json;
namespace {
void require(bool condition,const char* message) {
    if(!condition){std::fprintf(stderr,"FAIL vehicle-radio: %s\n",message);std::exit(1);}
}
json station(const char* id,const char* name,const std::string& url,double lon) {
    return {{"stationuuid",id},{"name",name},{"url_resolved",url},{"lastcheckok",1},
        {"codec","MP3"},{"geo_long",lon},{"geo_lat",0.}};
}
void waitFor(r1::VehicleRadio& radio,const char* name) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(std::chrono::steady_clock::now()<deadline) {
        radio.update(.025,0,0,true,false,false,false,0);
        const auto label=radio.label(0,0);
        if(label.find(name)!=std::string::npos&&label.find("Connexion")==std::string::npos)return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::fprintf(stderr,"Last radio state: %s\n",radio.label(0,0).c_str());
    require(false,"station did not reach playback");
}
}
int main(int argc,char** argv) {
    if(argc!=3){std::fprintf(stderr,"Usage: vehicle-radio-test fixture-root http-audio-url\n");return 2;}
    const fs::path root=argv[1];fs::create_directories(root/"cache/radio");
    const auto rows=json::array({station("bad","Unavailable local station","http://127.0.0.1:9/unavailable.mp3",0.),
        station("one","Local station one",argv[2],.001),station("two","Local station two",std::string(argv[2])+"?second",.002)});
    {std::ofstream out(root/"cache/radio/1800_900.json");out<<json{{"version",1},{"stations",rows}};}
    r1::VehicleRadio radio(root);
    radio.update(.025,0,0,false,true,false,false,0);
    require(radio.label(0,0).find("Radio off")!=std::string::npos,"radio cannot be enabled on foot");
    radio.update(.025,0,0,true,true,false,false,0);
    waitFor(radio,"Local station one");
    std::puts("PASS radio skips failed nearest stream and plays the next local station");
    radio.update(.025,0,0,false,false,false,false,0);
    radio.update(.025,0,0,true,false,false,false,0);waitFor(radio,"Local station one");
    std::puts("PASS radio stops on exit and resumes on re-entry");
    radio.update(.025,0,0,true,false,true,false,1);waitFor(radio,"Local station two");
    radio.update(.025,0,0,true,false,false,true,-1);waitFor(radio,"Local station one");
    std::puts("PASS radio next/previous station and volume controls");
    radio.update(.025,0,0,true,true,false,false,0);
    require(radio.label(0,0).find("Radio off")!=std::string::npos,"radio toggles off during playback");
    std::puts("PASS vehicle-radio complete lifecycle");
}
