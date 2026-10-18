#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>
#include <memory>

namespace r1::radio {
struct Station {
    std::string uuid,name,url,homepage;
    double lon=0,lat=0,distance=0;
    int votes=0;
};
double distanceMetres(double lon,double lat,double otherLon,double otherLat);
// Reject broken/ungeolocated/unsupported rows, deduplicate and rank by real
// great-circle distance, including across the date line and near the poles.
std::vector<Station> nearest(const nlohmann::json& rows,double lon,double lat,size_t limit=12,bool httpsOnly=false);
struct Catalogue {
    std::vector<Station> stations;
    std::string error;
    bool cached=false;
};
// Poll-driven discovery also works in single-threaded browser exports.
class Discovery {
public:
    Discovery(const std::filesystem::path& game,double lon,double lat,bool httpsOnly=false);
    ~Discovery();
    std::string url() const;
    double timeout() const;
    void accept(int status,const std::string& body,const std::string& error);
    bool ready() const;
    Catalogue take();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Worker-thread API. Cached observations are reusable offline; live audio is
// always streamed on demand and is never downloaded into the geographic cache.
Catalogue discover(const std::filesystem::path& game,double lon,double lat);
} // namespace r1::radio
