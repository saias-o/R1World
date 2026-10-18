#include "check.hpp"
#include "gen/radio.hpp"
#include "motorcycle.hpp"
#include <limits>
#include <chrono>
#include <fstream>

using nlohmann::json;
namespace {
json station(const char* id,const char* name,double lon,double lat) {
    return {{"stationuuid",id},{"name",name},{"geo_long",lon},{"geo_lat",lat},
        {"url_resolved",std::string("https://radio.example/")+id},{"lastcheckok",1},{"codec","MP3"}};
}
}
TEST(Radio, nearest_is_geographic_not_api_name_or_popularity_order) {
    auto far=station("far","A famous distant radio",5.,48.);far["votes"]=1000000;
    const auto close=station("local","Local station",2.352,48.856);
    auto rows=json::array({far,close,station("near","Next town",2.45,48.86)});
    const auto found=r1::radio::nearest(rows,2.3522,48.8566);
    CHECK(found.size()==3);CHECK(found.front().uuid=="local");CHECK(found[1].uuid=="near");
    CHECK(found.front().distance<100.);CHECK(found.back().distance>100000.);
}
TEST(Radio, an_unlisted_village_gets_the_nearest_known_station) {
    const auto found=r1::radio::nearest(json::array({station("far","Capital",5.,45.),station("near","Nearby town",2.3,48.8)}),2.4,48.9);
    CHECK(found.size()==2);CHECK(found.front().uuid=="near");CHECK(found.front().distance>10000.);
}
TEST(Radio, rejects_bad_rows_and_duplicate_streams) {
    auto a=station("a","A",2.,48.);
    auto broken=station("broken","Broken",2.,48.);broken["lastcheckok"]=0;
    auto null=station("null","Unknown position",2.,48.);null["geo_lat"]=nullptr;
    auto invalid=station("bad","Invalid",2.,91.);
    auto unsupported=station("ogg","Unsupported",2.,48.);unsupported["codec"]="OGG";
    auto unsafe=station("file","Local file",2.,48.);unsafe["url_resolved"]="file:///private.wav";
    auto duplicate=a;duplicate["stationuuid"]="copy";duplicate["name"]="Mirror";
    const auto rows=json::array({a,broken,null,invalid,unsupported,unsafe,duplicate,json::object(),42});
    CHECK(r1::radio::nearest(rows,2.,48.).size()==1);
    CHECK(r1::radio::nearest(rows,2.,48.,0).empty());
    CHECK(r1::radio::nearest(rows,2.,std::numeric_limits<double>::quiet_NaN()).empty());
}
TEST(Radio, nearest_handles_date_line_and_poles) {
    NEAR(r1::radio::distanceMetres(179.9,0.,-179.9,0.),22238.985,1.);
    CHECK(r1::radio::distanceMetres(0.,89.9,180.,89.9)<22300.);
    const auto found=r1::radio::nearest(json::array({station("far","Far",170.,0.),station("near","Across date line",-179.9,0.)}),179.9,0.);
    CHECK(found.front().uuid=="near");
}
TEST(Radio, only_offers_the_codec_embedded_on_every_export) {
    auto mp3=station("mp3","Portable",2.,48.);
    auto aac=station("aac","Requires another decoder",2.,48.);aac["codec"]="AAC";
    auto hls=station("hls","Playlist",2.,48.);hls["hls"]=1;
    const auto found=r1::radio::nearest(json::array({aac,mp3,hls}),2.,48.);
    CHECK(found.size()==1);CHECK(found.front().uuid=="mp3");
}
TEST(Radio, https_exports_choose_the_nearest_secure_stream) {
    auto http=station("http","Insecure nearest",2.,48.);http["url_resolved"]="http://radio.example/stream.mp3";
    auto https=station("https","Secure nearby",2.1,48.);
    CHECK(r1::radio::nearest(json::array({http,https}),2.,48.).front().uuid=="http");
    const auto found=r1::radio::nearest(json::array({http,https}),2.,48.,12,true);
    CHECK(found.size()==1);CHECK(found.front().uuid=="https");
}
TEST(Radio, discovery_is_pollable_and_expands_until_supported_stations_exist) {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/("r1-radio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    r1::radio::Discovery query(root,2.,48.);
    CHECK(!query.ready());CHECK(query.url().find("/servers")!=std::string::npos);
    query.accept(200,"[{\"name\":\"evil.example\"},{\"name\":\"de1.api.radio-browser.info\"}]",{});
    CHECK(query.url().find("https://de1.api.radio-browser.info/")==0);
    CHECK(query.url().find("geo_distance=25000")!=std::string::npos);
    query.accept(200,"[]",{});CHECK(!query.ready());
    CHECK(query.url().find("geo_distance=100000")!=std::string::npos);
    json rows=json::array();
    for(int i=0;i<6;++i){const auto id=std::to_string(i);rows.push_back(station(id.c_str(),("Station "+id).c_str(),2.+i*.01,48.));}
    query.accept(200,rows.dump(),{});CHECK(query.ready());CHECK(query.take().stations.size()==6);
    r1::radio::Discovery cached(root,2.,48.);CHECK(cached.ready());CHECK(cached.take().cached);
    const auto path=root/"cache/radio/1820_1380.json";
    fs::last_write_time(path,fs::file_time_type::clock::now()-std::chrono::hours(24*8));
    r1::radio::Discovery stale(root,2.,48.);CHECK(!stale.ready());
    stale.accept(0,{},"offline");
    for(int i=0;i<3;++i)stale.accept(0,{},"offline");
    CHECK(stale.ready());const auto fallback=stale.take();CHECK(fallback.cached);CHECK(fallback.stations.size()==6);
    fs::remove(path);fs::remove(root/"cache/radio");fs::remove(root/"cache");fs::remove(root);
}
TEST(Radio, discovery_rejects_invalid_coordinates_without_requesting_network) {
    r1::radio::Discovery invalid({},0.,91.);CHECK(invalid.ready());CHECK(invalid.url().empty());CHECK(!invalid.take().error.empty());
}
TEST(Motorcycles, women_are_selected_at_twenty_percent_independently_of_outfit_counts) {
    size_t women=0;
    for(uint32_t seed=0;seed<20000;++seed)women+=r1::femaleMotorcycleRider(seed);
    CHECK(women>3800&&women<4200);
}
