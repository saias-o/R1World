#include "radio.hpp"
#ifndef __EMSCRIPTEN__
#include "net.hpp"
#endif
#include "common.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>

namespace r1::radio {
using json=nlohmann::json;
double distanceMetres(double lon,double lat,double otherLon,double otherLat) {
    const double a=radians(lat),b=radians(otherLat);
    const double dy=std::sin((b-a)*.5),dx=std::sin(radians(otherLon-lon)*.5);
    return 12742000.*std::asin(std::sqrt(std::clamp(dy*dy+std::cos(a)*std::cos(b)*dx*dx,0.,1.)));
}
std::vector<Station> nearest(const json& rows,double lon,double lat,size_t limit,bool httpsOnly) {
    std::vector<Station> stations;
    if(limit==0||!rows.is_array()||!std::isfinite(lon)||!std::isfinite(lat)||std::abs(lat)>90||std::abs(lon)>180)return stations;
    for(const auto& row:rows)try {
        if(!row.is_object()||row.value("lastcheckok",0)!=1||!row.at("geo_lat").is_number()||!row.at("geo_long").is_number())continue;
        Station s;
        s.lat=row.at("geo_lat");s.lon=row.at("geo_long");
        if(!std::isfinite(s.lat)||!std::isfinite(s.lon)||std::abs(s.lat)>90||std::abs(s.lon)>180)continue;
        s.uuid=row.value("stationuuid",std::string());s.name=row.value("name",std::string());
        s.url=row.value("url_resolved",std::string());
        if(s.url.empty())s.url=row.value("url",std::string());
        s.homepage=row.value("homepage",std::string());s.votes=row.value("votes",0);
        const auto codec=row.value("codec",std::string());
        if(codec!="MP3"||row.value("hls",0)!=0)continue; // Common decoder baseline on every export.
        if(httpsOnly&&s.url.rfind("https://",0)!=0)continue;
        if(s.name.empty()||s.name.size()>200||s.uuid.empty()||s.uuid.size()>80||s.url.size()>8192||
           (s.url.rfind("https://",0)!=0&&s.url.rfind("http://",0)!=0)||s.url.find_first_of("\r\n\t")!=std::string::npos)continue;
        s.distance=distanceMetres(lon,lat,s.lon,s.lat);
        stations.push_back(std::move(s));
    } catch(const json::exception&) {continue;}
    std::sort(stations.begin(),stations.end(),[](const Station& a,const Station& b) {
        if(a.distance!=b.distance)return a.distance<b.distance;
        if(a.votes!=b.votes)return a.votes>b.votes;
        return a.uuid<b.uuid;
    });
    std::set<std::string> names,urls,ids;
    std::vector<Station> result;
    for(auto& station:stations) {
        std::string name=station.name;
        for(char& c:name)c=char(std::tolower(static_cast<unsigned char>(c)));
        if(names.count(name)||urls.count(station.url)||ids.count(station.uuid))continue;
        names.insert(name);urls.insert(station.url);ids.insert(station.uuid);
        result.push_back(std::move(station));
        if(result.size()>=limit)break;
    }
    return result;
}

struct Discovery::Impl {
    double lon,lat;
    std::filesystem::path path;
    Catalogue result,stale;
    std::vector<std::string> hosts{"de1.api.radio-browser.info","nl1.api.radio-browser.info","fi1.api.radio-browser.info"};
    size_t radius=0,attempt=0;
    bool servers=true,done=false,httpsOnly=false;
    json rows;
    static constexpr std::array<int,6> radii{{25000,100000,500000,2000000,8000000,20020000}};
    void rotate() {
        const auto seed=uint32_t((lon+180.)*1000)+uint32_t((lat+90.)*1000);
        std::rotate(hosts.begin(),hosts.begin()+seed%hosts.size(),hosts.end());
    }
    void finish() {
        done=true;
        if(result.stations.empty()){result.error="No compatible geolocated station available.";return;}
        result.error.clear();
        try {
            std::filesystem::create_directories(path.parent_path());
            const auto temporary=path.string()+".part";
            {std::ofstream output(temporary);output<<json{{"version",1},{"stations",rows}};if(!output)throw std::runtime_error("radio metadata cache write");}
            std::error_code error;std::filesystem::rename(temporary,path,error);
            if(error){std::filesystem::remove(path,error);std::filesystem::rename(temporary,path);}
        }catch(const std::exception&){} // Read-only caches never prevent playback.
    }
};
Discovery::Discovery(const std::filesystem::path& game,double lon,double lat,bool httpsOnly):impl_(std::make_unique<Impl>()) {
    auto& d=*impl_;d.lon=lon;d.lat=lat;d.httpsOnly=httpsOnly;
    if(!std::isfinite(lon)||!std::isfinite(lat)||std::abs(lon)>180||std::abs(lat)>90){d.result.error="Position radio invalide.";d.done=true;return;}
    d.path=game/"cache/radio"/(std::to_string(int(std::floor((lon+180.)*10)))+"_"+std::to_string(int(std::floor((lat+90.)*10)))+".json");
    try {
        std::ifstream input(d.path);
        if(input){
            const json cached=json::parse(input);
            if(cached.value("version",0)==1){
                d.result.stations=nearest(cached.at("stations"),lon,lat,12,d.httpsOnly);
                if(!d.result.stations.empty()){
                    d.result.cached=true;
                    const auto age=std::filesystem::file_time_type::clock::now()-std::filesystem::last_write_time(d.path);
                    if(age<std::chrono::hours(24*7)){d.done=true;return;}
                    d.stale=d.result;d.result=Catalogue{};
                }
            }
        }
    }catch(const std::exception&){}
}
Discovery::~Discovery()=default;
bool Discovery::ready() const{return impl_->done;}
double Discovery::timeout() const{return impl_->servers?4.:6.;}
std::string Discovery::url() const {
    const auto& d=*impl_;if(d.done)return {};
    if(d.servers)return "https://all.api.radio-browser.info/json/servers";
    return "https://"+d.hosts[d.attempt]+"/json/stations/search?has_geo_info=true&hidebroken=true&codec=MP3"+
        std::string(d.httpsOnly?"&is_https=true":"")+"&geo_lat="+
        std::to_string(d.lat)+"&geo_long="+std::to_string(d.lon)+"&geo_distance="+std::to_string(Impl::radii[d.radius])+"&limit=100000";
}
void Discovery::accept(int status,const std::string& body,const std::string& error) {
    auto& d=*impl_;if(d.done)return;
    const auto rows=json::parse(body,nullptr,false);
    const bool ok=error.empty()&&status>=200&&status<300&&rows.is_array();
    if(d.servers){
        if(ok){
            std::vector<std::string> discovered;
            for(const auto& server:rows)try{
                const auto host=server.value("name",std::string());const std::string suffix=".api.radio-browser.info";
                if(host.size()>suffix.size()&&host.size()<100&&host.compare(host.size()-suffix.size(),suffix.size(),suffix)==0&&
                    host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-")==std::string::npos)discovered.push_back(host);
            }catch(const json::exception&){}
            if(!discovered.empty())d.hosts=std::move(discovered);
        }
        d.servers=false;d.rotate();return;
    }
    if(!ok){
        d.result.error=error.empty()?"Invalid response from the radio catalogue.":error;
        if(++d.attempt<std::min(size_t(3),d.hosts.size()))return;
        d.done=true;if(!d.stale.stations.empty())d.result=std::move(d.stale);
        else if(!d.result.stations.empty())d.result.error.clear();
        else d.result.error="Catalogue radio inaccessible : "+d.result.error;
        return;
    }
    d.rows=rows;d.result.stations=nearest(rows,d.lon,d.lat,12,d.httpsOnly);
    // Keep the successful mirror first for progressively wider searches.
    if(d.attempt)std::swap(d.hosts.front(),d.hosts[d.attempt]);
    d.attempt=0;
    if(d.result.stations.size()>=6||++d.radius==Impl::radii.size()){d.finish();return;}
}
Catalogue Discovery::take(){return impl_->done?std::move(impl_->result):Catalogue{};}
#ifndef __EMSCRIPTEN__
Catalogue discover(const std::filesystem::path& game,double lon,double lat) {
    Discovery query(game,lon,lat);
    while(!query.ready()){
        try{query.accept(200,net::requestJson(query.url(),{},{},query.timeout(),1),{});}
        catch(const std::exception& e){query.accept(0,{},e.what());}
    }
    return query.take();
}
#endif
} // namespace r1::radio
