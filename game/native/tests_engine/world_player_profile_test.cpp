#include "world/player_profile.hpp"
#include <nlohmann/json.hpp>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>

using namespace r1::world;
namespace fs=std::filesystem;
using nlohmann::json;
int main() {
    const auto root=fs::temp_directory_path()/("r1-profile-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    struct Cleanup {fs::path root;~Cleanup(){std::error_code error;fs::remove_all(root,error);}} cleanup{root};
    PlayerProfileStore store(root);std::string error;
    auto p=store.load(error);assert(error.empty()&&p.photos.empty()&&p.wallet.balance()==0&&p.items.at("camera")==1);
    assert(!fs::exists(root/"player.json"));
    assert(!p.wallet.spend(1)&&!p.wallet.credit(-1)&&!p.wallet.spend(-1));
    assert(p.wallet.credit(12345)&&p.wallet.spend(345)&&p.wallet.balance()==12000);
    assert(!p.wallet.credit(std::numeric_limits<int64_t>::max()));
    assert(p.addItem("map",2)&&p.consumeItem("map")&&p.items.at("map")==1);
    assert(!p.consumeItem("map",2)&&!p.consumeItem("camera",-1)&&!p.addItem("",1));
    assert(!p.addItem("map",999999)&&p.consumeItem("map")&&!p.items.count("map"));
    auto photo=store.newPhoto();photo.title="Éléphant · découverte";photo.place="Paris";
    photo.category=PhotoCategory::Animal;photo.longitude=2.35;photo.latitude=48.85;photo.altitude=34;
    photo.fovDegrees=24;photo.animals={{"elephant","Éléphant"},{"gull","Mouette"}};
    assert(photo.id!=store.newPhoto().id);assert(store.photoPath(photo).parent_path()==root/"photos");
    fs::create_directories(root/"photos");std::ofstream(store.photoPath(photo))<<"test image";
    p.photos.push_back(photo);
    r1::combat::Arsenal arsenal;arsenal.select(r1::combat::Weapon::Pistol);arsenal.update(1.);
    assert(arsenal.pull()==r1::combat::Arsenal::Trigger::Fired);
    p.arsenal=arsenal.snapshot();assert(store.save(p,error)&&error.empty());
    auto saved=store.load(error);assert(error.empty()&&saved.wallet.balance()==12000&&saved.arsenal==arsenal.snapshot());
    assert(saved.photos.size()==1&&saved.photos[0].title==photo.title&&saved.photos[0].category==PhotoCategory::Animal);
    assert(saved.items==p.items);
    assert(saved.photos[0].fovDegrees==24&&saved.photos[0].animals.size()==2);
    assert(hasPhotoSpecies(saved.photos[0],"elephant")&&hasPhotoSpecies(saved.photos[0],"gull")&&!hasPhotoSpecies(saved.photos[0],"cat"));
    arsenal.disarm();assert(arsenal.restore(saved.arsenal));
    assert(arsenal.loaded(r1::combat::Weapon::Pistol)==16&&arsenal.reserve(r1::combat::Weapon::RocketLauncher)==12);
    arsenal.select(r1::combat::Weapon::RocketLauncher);
    assert(arsenal.loaded()==1&&arsenal.reserve()==12);
    arsenal.disarm();p.arsenal=arsenal.snapshot();assert(store.save(p,error));
    saved=store.load(error);assert(saved.arsenal.loaded[0]==0&&saved.arsenal.reserve[1]==0);
    const auto read=[&](){std::ifstream in(root/"player.json");return json::parse(in);};
    const auto good=read();
    assert(good["version"]==2);
    auto legacy=good;legacy["version"]=1;
    for(auto& photo:legacy["photos"]){photo.erase("animals");photo.erase("fovDegrees");}
    {std::ofstream out(root/"player.json");out<<legacy.dump();}
    const auto migrated=store.load(error);
    assert(error.empty()&&migrated.photos.size()==1&&migrated.photos[0].animals.empty()&&migrated.photos[0].file==p.photos[0].file);
    assert(migrated.wallet.balance()==p.wallet.balance()&&migrated.arsenal==p.arsenal&&store.save(migrated,error));
    assert(read()["version"]==2);
    for(int variant=0;variant<14;++variant) {
        auto bad=good;
        switch(variant) {
            case 0:bad["version"]=99;break;
            case 1:bad["moneyCents"]=-1;break;
            case 2:bad["moneyCents"]=1.5;break;
            case 3:bad["arsenal"]["loaded"][0]=18;break;
            case 4:bad["photos"][0]["file"]="../../outside.png";break;
            case 5:bad["photos"].push_back(bad["photos"][0]);break;
            case 6:bad["photos"][0]["latitude"]=91;break;
            case 7:bad["photos"][0]["category"]="unknown";break;
            case 8:bad["items"]["camera"]=0;break;
            case 9:bad["arsenal"]["held"]=256;break;
            case 10:bad["photos"][0]["animals"].push_back(bad["photos"][0]["animals"][0]);break;
            case 11:bad["photos"][0]["animals"][0]["species"]="../cat";break;
            case 12:bad["photos"][0]["fovDegrees"]=0;break;
            case 13:bad["photos"][0]["category"]="world";break;
        }
        {std::ofstream out(root/"player.json");out<<bad.dump();}
        store.load(error);assert(!error.empty()&&!store.writable());
        assert(!store.save(p,error)&&read()==bad); // damaged/future saves stay untouched
    }
    {std::ofstream out(root/"player.json");out<<good.dump();}
    store.load(error);assert(error.empty()&&store.writable());
    auto unsafe=photo;unsafe.id="../outside";unsafe.file=unsafe.id+".png";
    bool refused=false;try{store.photoPath(unsafe);}catch(...){refused=true;}assert(refused);
    p.photos.push_back(photo);assert(!store.save(p,error)&&read()==good); // invalid update cannot replace the last save
    PlayerProfileStore unwritable(root/"player.json");assert(!unwritable.save(saved,error)&&!error.empty());
    std::cout<<"PASS player profile: wallet, items, ammunition, portfolio, round trip, protected corruption and write failures\n";
}
