#include "player_profile.hpp"
#include "../atomic_file.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace r1::world {
using json=nlohmann::json;
namespace fs=std::filesystem;
const char* categoryKey(PhotoCategory c) {
    switch(c){case PhotoCategory::Animal:return "animal";case PhotoCategory::Monument:return "monument";
        case PhotoCategory::Event:return "event";default:return "world";}
}
const char* categoryLabel(PhotoCategory c) {
    switch(c){case PhotoCategory::Animal:return "Animaux";case PhotoCategory::Monument:return "Monuments";
        case PhotoCategory::Event:return "Insolite";default:return "Monde";}
}
namespace {
PhotoCategory parseCategory(const std::string& key) {
    for(auto c:{PhotoCategory::World,PhotoCategory::Animal,PhotoCategory::Monument,PhotoCategory::Event})
        if(key==categoryKey(c))return c;
    throw std::runtime_error("unknown photo category");
}
bool safeId(const std::string& id) {
    return !id.empty()&&id.size()<=80&&id.find_first_not_of("0123456789-")==std::string::npos;
}
void validate(const Photograph& p) {
    if(!safeId(p.id)||p.file!=p.id+".png"||p.title.size()>512||p.place.size()>512||p.takenUtc.size()>64||
       !std::isfinite(p.longitude)||std::abs(p.longitude)>180||!std::isfinite(p.latitude)||std::abs(p.latitude)>90||
       !std::isfinite(p.altitude)||!std::isfinite(p.fovDegrees)||p.fovDegrees<1||p.fovDegrees>=179)
        throw std::runtime_error("invalid photograph metadata");
    if(p.animals.size()>1000||(!p.animals.empty()&&p.category!=PhotoCategory::Animal))throw std::runtime_error("invalid photo animals");
    std::set<std::string> species;
    for(const auto& a:p.animals) {
        if(a.species.empty()||a.species.size()>96||a.species.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=std::string::npos||
           a.name.empty()||a.name.size()>256||!species.insert(a.species).second)throw std::runtime_error("invalid photo species");
    }
}
int integer(const json& value,int low,int high) {
    if(!value.is_number_integer()||value<low||value>high)throw std::runtime_error("invalid inventory quantity");
    return value.get<int>();
}
}
bool hasPhotoSpecies(const Photograph& photo,const std::string& species) {
    return species.empty()||std::any_of(photo.animals.begin(),photo.animals.end(),[&](const auto& a){return a.species==species;});
}
bool Wallet::credit(int64_t n) { if(n<0||n>kMaximum-balance_)return false;balance_+=n;return true; }
bool Wallet::spend(int64_t n) { if(n<0||n>balance_)return false;balance_-=n;return true; }
bool PlayerProfile::addItem(const std::string& id,int n) {
    if(id.empty()||id.size()>80||n<=0||n>999999)return false;
    const auto it=items.find(id);const int before=it==items.end()?0:it->second;
    if(before>999999-n)return false;items[id]=before+n;return true;
}
bool PlayerProfile::consumeItem(const std::string& id,int n) {
    auto it=items.find(id);if(n<=0||it==items.end()||it->second<n)return false;
    it->second-=n;if(!it->second)items.erase(it);return true;
}
PlayerProfile PlayerProfileStore::load(std::string& error) {
    error.clear();writable_=true;
    PlayerProfile profile;
    const auto path=directory_/"player.json";
    try {
        if(!fs::exists(path))return profile;
        std::ifstream in(path);if(!in)throw std::runtime_error("cannot open "+path.string());
        const auto j=json::parse(in);
        if(!j.is_object()||!j.at("version").is_number_integer()||(j.at("version")!=1&&j.at("version")!=2))
            throw std::runtime_error("unsupported player save version");
        const int version=j.at("version");
        const auto& balance=j.at("moneyCents");
        if(!balance.is_number_integer()||balance<0||balance>Wallet::kMaximum||!profile.wallet.credit(balance.get<int64_t>()))
            throw std::runtime_error("invalid wallet balance");
        const auto& items=j.at("items");if(!items.is_object())throw std::runtime_error("invalid items");
        profile.items.clear();
        for(auto i=items.begin();i!=items.end();++i)
            if(!profile.addItem(i.key(),integer(i.value(),1,999999)))throw std::runtime_error("invalid item");
        const auto& a=j.at("arsenal");
        profile.arsenal.held=combat::Weapon(integer(a.at("held"),0,2));
        for(size_t i=0;i<2;++i) {
            profile.arsenal.loaded[i]=integer(a.at("loaded").at(i),0,i?combat::kRocket.magazine:combat::kPistol.magazine);
            profile.arsenal.reserve[i]=integer(a.at("reserve").at(i),0,combat::Arsenal::kMostReserve);
        }
        if(!combat::Arsenal::valid(profile.arsenal))throw std::runtime_error("invalid arsenal");
        const auto& photos=j.at("photos");if(!photos.is_array())throw std::runtime_error("invalid portfolio");
        std::set<std::string> ids;
        for(const auto& v:photos) {
            Photograph p;
            p.id=v.at("id");p.file=v.at("file");p.title=v.at("title");p.takenUtc=v.at("takenUtc");p.place=v.at("place");
            p.category=parseCategory(v.at("category"));p.longitude=v.at("longitude");p.latitude=v.at("latitude");p.altitude=v.at("altitude");
            if(version>=2) {
                p.fovDegrees=v.at("fovDegrees").get<float>();
                const auto& animals=v.at("animals");
                if(!animals.is_array())throw std::runtime_error("invalid photo animals");
                for(const auto& a:animals)p.animals.push_back({a.at("species"),a.at("name")});
            }
            validate(p);if(!ids.insert(p.id).second)throw std::runtime_error("duplicate photo id");
            profile.photos.push_back(std::move(p));
        }
        return profile;
    } catch(const std::exception& e) {error=e.what();writable_=false;return {};}
}
bool PlayerProfileStore::save(const PlayerProfile& p,std::string& error) const {
    error.clear();
    try {
        if(!writable_)throw std::runtime_error("player save is protected after a load error");
        if(!combat::Arsenal::valid(p.arsenal))throw std::runtime_error("invalid arsenal");
        json photos=json::array();std::set<std::string> ids;
        for(const auto& v:p.photos) {
            validate(v);if(!ids.insert(v.id).second)throw std::runtime_error("duplicate photo id");
            json animals=json::array();
            for(const auto& a:v.animals)animals.push_back({{"species",a.species},{"name",a.name}});
            photos.push_back({{"id",v.id},{"file",v.file},{"title",v.title},{"takenUtc",v.takenUtc},{"place",v.place},
                {"category",categoryKey(v.category)},{"longitude",v.longitude},{"latitude",v.latitude},{"altitude",v.altitude},
                {"fovDegrees",v.fovDegrees},{"animals",std::move(animals)}});
        }
        for(const auto& [id,n]:p.items)if(id.empty()||id.size()>80||n<=0||n>999999)throw std::runtime_error("invalid item");
        json j={{"version",2},{"moneyCents",p.wallet.balance()},{"items",p.items},
            {"arsenal",{{"held",int(p.arsenal.held)},{"loaded",p.arsenal.loaded},{"reserve",p.arsenal.reserve}}},{"photos",std::move(photos)}};
        atomicWrite(directory_/"player.json",j.dump(2));return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
fs::path PlayerProfileStore::photoPath(const Photograph& p) const {validate(p);return directory_/"photos"/p.file;}
Photograph PlayerProfileStore::newPhoto() const {
    static std::atomic<unsigned> serial{0};
    const auto now=std::chrono::system_clock::now();
    Photograph p;
    p.id=std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count())+"-"+std::to_string(++serial);
    p.file=p.id+".png";
    const auto time=std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc,&time);
#else
    gmtime_r(&time,&utc);
#endif
    std::ostringstream date;date<<std::put_time(&utc,"%Y-%m-%d %H:%M:%S UTC");p.takenUtc=date.str();
    return p;
}
} // namespace r1::world
