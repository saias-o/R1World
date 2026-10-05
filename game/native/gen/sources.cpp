#include "sources.hpp"

#include "net.hpp"
#include "relief.hpp"
#include "seaice.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../engine/third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

namespace fs = std::filesystem;

namespace r1 {

nlohmann::json readJson(const std::string& path) {
    // Whole file, then parse: nlohmann reading a stream is several times slower.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::string text(size_t(f.tellg()), ' ');
    f.seekg(0);
    f.read(text.data(), std::streamsize(text.size()));
    return nlohmann::json::parse(text);
}

namespace {

// Written whole, then moved into place: a reader never sees half a file.
void writeJson(const std::string& path, const nlohmann::json& value) {
    fs::create_directories(fs::path(path).parent_path());
    const std::string tmp = path + ".tmp" + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()));
    { std::ofstream f(tmp, std::ios::binary); f << value.dump() << "\n"; }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(path, ec); fs::rename(tmp, path); }
}

// Python's repr of a float: the shortest text that reads back the same.
std::string pyFloat(double v) {
    std::string s = nlohmann::json(v).dump();
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

bool sameBounds(const nlohmann::json& j, const Bounds& b) {
    return j.at("south").get<double>() == b.south && j.at("west").get<double>() == b.west &&
           j.at("north").get<double>() == b.north && j.at("east").get<double>() == b.east;
}

ElevationGrid gridFrom(const nlohmann::json& doc, const Bounds& b) {
    ElevationGrid g;
    g.bounds = b;
    g.size = doc.at("size").get<int>();
    for (const auto& row : doc.at("values")) for (const auto& v : row) g.values.push_back(v.get<double>());
    if (int(g.values.size()) != g.size * g.size) throw std::runtime_error("elevation grid of the wrong size");
    return g;
}

nlohmann::json boundsJson(const Bounds& b) {
    return {{"south", b.south}, {"west", b.west}, {"north", b.north}, {"east", b.east}};
}

const char* kOverpass[] = {"https://overpass-api.de/api/interpreter",
                           "https://maps.mail.ru/osm/tools/overpass/api/interpreter"};
constexpr int kEndpoints = int(sizeof kOverpass / sizeof kOverpass[0]);
// A mirror that just failed is asked last for five minutes: every query
// otherwise paid its failure again before reaching the one that answers.
std::atomic<int64_t> gDownUntil[kEndpoints] = {};
int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::vector<int> endpointOrder() {
    std::vector<int> order(kEndpoints);
    for (int i = 0; i < kEndpoints; ++i) order[size_t(i)] = i;
    const int64_t now = nowSeconds();
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return (gDownUntil[a].load() > now) < (gDownUntil[b].load() > now);
    });
    return order;
}
void markDown(int endpoint) { gDownUntil[endpoint] = nowSeconds() + 300; }
}  // namespace

std::string ObservationStore::tileFolder(const Tile& tile) const {
    return root_ + "/cache/world/" + tile.key(kVersion);
}

std::optional<std::string> ObservationStore::find(const Tile& tile, const char* name) const {
    for (int version = kVersion; version >= kFirstVersion; --version) {
        const std::string path = root_ + "/cache/world/" + tile.key(version) + "/" + name;
        std::error_code ec;
        if (fs::exists(path, ec)) return path;
    }
    return std::nullopt;
}

std::optional<ObservationStore::Shared> ObservationStore::shared(const std::vector<Tile>& group) const {
    if (group.empty()) return std::nullopt;
    Bounds r{1e300, 1e300, -1e300, -1e300};
    for (const Tile& t : group) {
        const Bounds b = t.bounds();
        r.south = std::min(r.south, b.south); r.west = std::min(r.west, b.west);
        r.north = std::max(r.north, b.north); r.east = std::max(r.east, b.east);
    }
    if (r.east - r.west >= 0.15) return std::nullopt;
    // The key is the one the Python worker hashed: json.dumps(asdict(region), sort_keys=True).
    const std::string text = "{\"east\": " + pyFloat(r.east) + ", \"north\": " + pyFloat(r.north) +
                             ", \"south\": " + pyFloat(r.south) + ", \"west\": " + pyFloat(r.west) + "}";
    return Shared{r, root_ + "/cache/world/sources/" + sha256Hex(text).substr(0, 20) + ".json"};
}

std::vector<std::string> ObservationStore::candidates(const Tile& tile, const std::optional<Shared>& current,
                                                     std::map<std::string, Bounds>* regions,
                                                     const std::string& sibling) const {
    std::vector<std::string> out;
    std::error_code ec;
    auto add = [&](std::string path, const Bounds& region) {
        if (!sibling.empty()) path = path.substr(0, path.size() - 5) + sibling;
        if (fs::exists(path, ec) && std::find(out.begin(), out.end(), path) == out.end()) {
            out.push_back(path);
            if (regions) (*regions)[path] = region;
        }
    };
    if (current) add(current->path, current->region);
    // A group is the three rows around a centre row, each taking the three
    // columns around the column its own ring puts the player's longitude in
    // (world.cpp `nearby`). Between two column boundaries of those rows the
    // group cannot change, so one longitude per interval visits every group.
    const Bounds b = tile.bounds();
    const double width = b.east - b.west;
    for (int centre = tile.row - 1; centre <= tile.row + 1; ++centre) {
        if (centre < 0 || centre >= kRows) continue;
        std::vector<double> cuts{b.west - 2 * width, b.east + 2 * width};
        for (int row = centre - 1; row <= centre + 1; ++row) {
            const int n = columns(std::max(0, std::min(kRows - 1, row)));
            for (int k = int(std::floor((b.west - 2 * width + 180.0) / 360.0 * n));
                 k <= int(std::ceil((b.east + 2 * width + 180.0) / 360.0 * n)); ++k)
                cuts.push_back(-180.0 + 360.0 * k / n);
        }
        std::sort(cuts.begin(), cuts.end());
        for (size_t i = 0; i + 1 < cuts.size(); ++i) {
            const double lon = (cuts[i] + cuts[i + 1]) / 2;
            if (cuts[i + 1] - cuts[i] < 1e-12 || lon < b.west - 2 * width || lon > b.east + 2 * width) continue;
            std::vector<Tile> group;
            for (int row = centre - 1; row <= centre + 1; ++row) {
                const int r = std::max(0, std::min(kRows - 1, row)), n = columns(r);
                const int c = int(std::floor((wrap(lon) + 180.0) / 360.0 * n));
                for (int dc = -1; dc <= 1; ++dc) group.push_back({r, (c + dc + n) % n});
            }
            if (std::find(group.begin(), group.end(), tile) == group.end()) continue;
            if (const auto s = shared(group)) add(s->path, s->region);
        }
    }
    // A tile asked for on its own, outside any group, had a query of its own.
    if (const auto s = shared({tile})) add(s->path, s->region);
    if (auto path = find(tile, "osm.json")) add(*path, tile.bounds());
    return out;
}

std::optional<Bounds> ObservationStore::regionOf(const Tile& tile, const std::optional<Shared>& shared,
                                                 const std::string& path) const {
    std::map<std::string, Bounds> regions;
    candidates(tile, shared, &regions);
    auto it = regions.find(path);
    if (it == regions.end()) return std::nullopt;
    return it->second;
}

namespace {
// The question an answer on disk replied to, read from its last bytes: the
// Python worker wrote its keys sorted, so `r1QueryVersion` is near the end,
// and parsing ten megabytes to learn one number is what this avoids.
int versionOf(const std::string& path, const char* key) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const std::streamoff size = f.tellg();
    const std::streamoff take = std::min<std::streamoff>(size, 4096);
    f.seekg(size - take);
    std::string tail(size_t(take), ' ');
    f.read(tail.data(), take);
    const auto at = tail.rfind("\"" + std::string(key) + "\"");
    if (at == std::string::npos) return 1;
    const auto colon = tail.find(':', at);
    return colon == std::string::npos ? 1 : std::atoi(tail.c_str() + colon + 1);
}
int queryVersionOf(const std::string& path) { return versionOf(path, "r1QueryVersion"); }
}  // namespace

int ObservationStore::queryVersion(const std::string& path) { return queryVersionOf(path); }
bool ObservationStore::retailCurrent(const std::string& layer) { return versionOf(layer, "r1RetailVersion") >= kRetailLayerVersion; }

bool ObservationStore::cached(const Tile& tile, const std::optional<Shared>& shared) const {
    return !candidates(tile, shared).empty() && (find(tile, "ground-elevation.json") || find(tile, "elevation.json"));
}

std::optional<nlohmann::json> ObservationStore::osm(const Tile& tile, const std::optional<Shared>& shared, bool* stale) const {
    const auto path = osmPath(tile, shared, stale);
    if (!path) return std::nullopt;
    return readJson(*path);
}

std::optional<std::string> ObservationStore::osmPath(const Tile& tile, const std::optional<Shared>& shared, bool* stale) const {
    const auto paths = candidates(tile, shared);
    if (paths.empty()) return std::nullopt;
    std::string best;
    std::uintmax_t bestSize = 0;
    bool bestCurrent = false;
    std::error_code ec;
    for (const auto& path : paths) {
        const int version = queryVersionOf(path);
        if (version < kOsmBaseVersion) continue;
        // An answer to the current question first (it carries the aero
        // layer and the country), then the widest.
        const bool current = version >= kOsmQueryVersion;
        const auto size = fs::file_size(path, ec);
        if (best.empty() || (current && !bestCurrent) || (current == bestCurrent && size > bestSize)) {
            best = path; bestSize = size; bestCurrent = current;
        }
    }
    if (stale) *stale = best.empty();
    return best.empty() ? paths.front() : best;
}

namespace {
std::pair<ElevationGrid, std::string> terrainTiles(const Bounds& b, const std::string& root, bool network);
void writeGround(const std::string& path, const ElevationGrid& g, const std::string& source);
std::function<double(double, double)> imageSampler(const std::string& root, int zoom, bool network);
}  // namespace

std::optional<std::pair<ElevationGrid, std::string>> ObservationStore::ground(const Tile& tile) const {
    const Bounds b = tile.bounds();
    if (auto path = find(tile, "ground-elevation.json")) {
        const auto doc = readJson(*path);
        if (!sameBounds(doc.at("bounds"), b)) throw std::runtime_error("Ground elevation cache bounds mismatch");
        auto grid = gridFrom(doc, b);
        auto source = doc.at("source").get<std::string>();
        // Read before the ground was read finely: seven samples a side, each
        // the nearest pixel. The images it was read from are on disk, and the
        // finer read needs nothing else -- no network for a place visited.
        if (source.rfind("Mapzen", 0) == 0 && grid.size < kTerrainMeshSize) {
            try {
                std::tie(grid, source) = terrainTiles(b, root_, false);
                writeGround(tileFolder(tile) + "/ground-elevation.json", grid, source);
            } catch (const SourceUnavailable& e) {
                if (log_) log_("GROUND-COARSE " + tile.key() + " kept: " + e.what());
            }
        }
        return std::make_pair(std::move(grid), std::move(source));
    }
    // The GLO-90 grid an older worker fetched and never converted.
    if (auto path = find(tile, "elevation.json")) {
        const auto doc = readJson(*path);
        if (sameBounds(doc.at("bounds"), b))
            return std::make_pair(gridFrom(doc, b), std::string("Copernicus DEM GLO-90 via Open-Meteo (fallback)"));
    }
    return std::nullopt;
}

std::vector<ElevationGrid> ObservationStore::groundAround(const Tile& tile) const {
    std::vector<ElevationGrid> out;
    const double width = tile.bounds().east - tile.bounds().west;
    for (int dr = -1; dr <= 1; ++dr) {
        const int row = tile.row + dr;
        if (row < 0 || row >= kRows) continue;
        const double lat = -90.0 + (row + 0.5) * kStep;
        for (int dc = -1; dc <= 1; ++dc) {
            const Tile n = tileAt(tile.center().x + dc * width, lat);
            if (n == tile) continue;
            try {
                if (auto g = ground(n)) out.push_back(std::move(g->first));
            } catch (const std::exception&) {
                // An unreadable neighbour: the bridge is solved on this
                // tile's own ground, never a refused tile.
            }
        }
    }
    return out;
}

std::string ObservationStore::osmQuery(const Bounds& b) {
    char bbox[160], wide[160];
    std::snprintf(bbox, sizeof bbox, "%.8f,%.8f,%.8f,%.8f", b.south, b.west, b.north, b.east);
    // Runways five kilometres around: a terminal's stands are a mile or two
    // from the runways that say what the airport can receive (gen/airports).
    const double dLat = 0.045, dLon = 0.045 / std::max(0.2, std::cos(radians((b.south + b.north) / 2)));
    // Overpass refuses a longitude past +-180 (HTTP 400): at the antimeridian
    // and near the poles, where a tile spans a third of the planet, the wider
    // box stops at it.
    std::snprintf(wide, sizeof wide, "%.8f,%.8f,%.8f,%.8f", std::max(-90.0, b.south - dLat), std::max(-180.0, b.west - dLon),
                  std::min(90.0, b.north + dLat), std::min(180.0, b.east + dLon));
    // The surveyed summits come in the wider box: the tiles around are raised
    // to them too (gen/peaks.hpp), and they are a few hundred bytes.
    // Rank 10 is a list of point features, as narrow as it is on purpose:
    // `node[amenity]` alone would bring every bank and restaurant.
    // Every statement carries its own box: a global `[bbox]` would also cut
    // the runway statement's wider one down to the neighbourhood.
    std::string query = R"([out:json][timeout:90];
(
  way[building]{B};
  way[highway]{B};
  way[landuse]{B};
  way[natural]{B};
  way[leisure~"park|garden|golf_course|pitch"]{B};
  way[waterway]{B};
  way[water]{B};
  way[amenity=grave_yard]{B};
  way[amenity=parking]{B};
  way[shop]{B};
  node[shop]{B};
  way[amenity=fuel]{B};
  node[amenity=fuel]{B};
  node[entrance]{B};
  nwr[amenity~"^(police|school|kindergarten|college|university|hospital|clinic|doctors|prison|place_of_worship|townhall|courthouse|bank|fire_station|restaurant|cafe|bar|pub)$"]{B};
  nwr[office]{B};
  nwr[craft]{B};
  node[military=gendarmerie]{B};
  way[man_made~"^(pier|breakwater|groyne|quay)$"]{B};
  way[leisure=marina]{B};
  way[harbour]{B};
  way[aeroway~"^(aerodrome|runway|taxiway|taxilane|apron|helipad|parking_position|stopway)$"]{B};
  way[aeroway=runway]{W};
  way[military]{B};
  node[aeroway~"^(helipad|parking_position)$"]{B};
  node[leisure=marina]{B};
  node[harbour]{B};
  node["seamark:type"~"^(harbour|mooring|light_major|light_minor|landmark)$"]{B};
  node[natural=tree]{B};
  node[highway~"^(street_lamp|bus_stop|crossing|traffic_signals)$"]{B};
  node[amenity~"^(bench|fountain|waste_basket|drinking_water|post_box|telephone|clock)$"]{B};
  node[emergency=fire_hydrant]{B};
  node[power~"^(tower|pole)$"]{B};
  node[man_made~"^(water_tower|windmill|lighthouse|mast)$"]{B};
  node[natural~"^(rock|stone)$"]{B};
  node[natural=peak][ele]{W};
  node[natural=volcano][ele]{W};
  node[traffic_calming]{B};
  node[highway~"^(give_way|stop)$"]{B};
  node[traffic_sign]{B};
);
out body;
>;
out skel qt;
is_in{C}->.here;
area.here["admin_level"="2"]["ISO3166-1"];
out tags;)";
    // The country is asked of the neighbourhood's centre: the highway code
    // the predictive model reads (gen/predict.cpp) is measured, not drawn.
    char centre[80];
    std::snprintf(centre, sizeof centre, "%.8f,%.8f", (b.south + b.north) / 2, (b.west + b.east) / 2);
    for (const auto& [key, box] : {std::pair<std::string, std::string>{"{B}", bbox}, {"{W}", wide}, {"{C}", centre}})
        for (size_t at; (at = query.find(key)) != std::string::npos;) query.replace(at, key.size(), "(" + box + ")");
    return query;
}

nlohmann::json ObservationStore::fetchOsm(const Bounds& b, const std::string& path) const {
    std::optional<nlohmann::json> stale;
    std::error_code ec;
    if (fs::exists(path, ec)) {
        auto doc = readJson(path);
        if (doc.value("r1QueryVersion", 1) >= kOsmBaseVersion) return doc;
        stale = std::move(doc);
    }
    const std::string query = osmQuery(b);
    const std::string body = "data=" + net::urlEncode(query);
    std::string failures;
    for (const int i : endpointOrder()) {
        const char* endpoint = kOverpass[i];
        try {
            auto doc = nlohmann::json::parse(net::requestJson(endpoint, body, "application/x-www-form-urlencoded"));
            doc["r1QueryVersion"] = kOsmQueryVersion;
            writeJson(path, doc);
            return doc;
        } catch (const std::exception& e) {
            markDown(i);
            failures += std::string(failures.empty() ? "" : " | ") + endpoint + ": " + e.what();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    // An older question's answer beats no world at all, and says so.
    if (stale) return *stale;
    throw SourceUnavailable("all Overpass endpoints failed: " + failures);
}

std::string ObservationStore::aeroSibling(const std::string& mainPath) {
    const std::string ext = ".json";
    const bool json = mainPath.size() > ext.size() && mainPath.compare(mainPath.size() - ext.size(), ext.size(), ext) == 0;
    return (json ? mainPath.substr(0, mainPath.size() - ext.size()) : mainPath) + ".aero.json";
}

std::optional<std::string> ObservationStore::aeroPath(const Tile& tile, const std::optional<Shared>& shared,
                                                      const std::string& mainPath, bool& needed) const {
    needed = queryVersionOf(mainPath) < kOsmAeroVersion;
    if (!needed) return std::nullopt;
    const auto layers = candidates(tile, shared, nullptr, ".aero.json");
    if (!layers.empty()) return layers.front();
    return std::nullopt;
}

ObservationStore::Shared ObservationStore::aeroTarget(const Tile& tile, const std::optional<Shared>& shared) const {
    if (shared) return {shared->region, aeroSibling(shared->path)};
    return {tile.bounds(), tileFolder(tile) + "/osm.aero.json"};
}

nlohmann::json ObservationStore::fetchAero(const Bounds& b, const std::string& path) const {
    char bbox[160], wide[160];
    std::snprintf(bbox, sizeof bbox, "%.8f,%.8f,%.8f,%.8f", b.south, b.west, b.north, b.east);
    const double dLat = 0.045, dLon = 0.045 / std::max(0.2, std::cos(radians((b.south + b.north) / 2)));
    std::snprintf(wide, sizeof wide, "%.8f,%.8f,%.8f,%.8f", std::max(-90.0, b.south - dLat), std::max(-180.0, b.west - dLon),
                  std::min(90.0, b.north + dLat), std::min(180.0, b.east + dLon));
    // The statements version 6 added to the main question, and only those.
    const std::string query = std::string("[out:json][timeout:60];\n(\n") +
        "  way[aeroway~\"^(aerodrome|runway|taxiway|taxilane|apron|helipad|parking_position|stopway)$\"](" + bbox + ");\n" +
        "  way[aeroway=runway](" + wide + ");\n" +
        "  way[military](" + bbox + ");\n" +
        "  node[aeroway~\"^(helipad|parking_position)$\"](" + bbox + ");\n" +
        ");\nout body;\n>;\nout skel qt;";
    const std::string body = "data=" + net::urlEncode(query);
    std::string failures;
    for (const int i : endpointOrder()) {
        const char* endpoint = kOverpass[i];
        try {
            auto doc = nlohmann::json::parse(net::requestJson(endpoint, body, "application/x-www-form-urlencoded"));
            doc["r1AeroVersion"] = 1;
            writeJson(path, doc);
            return doc;
        } catch (const std::exception& e) {
            markDown(i);
            failures += std::string(failures.empty() ? "" : " | ") + endpoint + ": " + e.what();
        }
    }
    throw SourceUnavailable("the aero layer: all Overpass endpoints failed: " + failures);
}

std::optional<nlohmann::json> ObservationStore::seaIce(double lon, double lat) const {
    const std::string path = root_ + "/cache/world/seaice/" + seaIceWindow(lon, lat).file();
    std::error_code ec;
    if (!fs::exists(path, ec)) return std::nullopt;
    return readJson(path);
}

std::optional<std::string> ObservationStore::retailPath(const Tile& tile,const std::optional<Shared>& shared,
                                                       const std::string& mainPath) const {
    if(queryVersionOf(mainPath)>=kOsmRetailVersion)return std::nullopt;
    // A layer of the current version first; an older one is still read
    // while the current one is fetched (it has the shops, not the stations).
    const auto layers=candidates(tile,shared,nullptr,".retail.json");
    for(const auto& layer:layers)if(retailCurrent(layer))return layer;
    if(!layers.empty())return layers.front();
    return std::nullopt;
}
ObservationStore::Shared ObservationStore::retailTarget(const Tile& tile,const std::optional<Shared>& shared) const {
    if(shared)return {shared->region,shared->path.substr(0,shared->path.size()-5)+".retail.json"};
    return {tile.bounds(),tileFolder(tile)+"/osm.retail.json"};
}
nlohmann::json ObservationStore::fetchRetail(const Bounds& b,const std::string& path) const {
    std::error_code ec;if(fs::exists(path,ec)&&retailCurrent(path))return readJson(path);
    char bbox[160];std::snprintf(bbox,sizeof bbox,"(%.8f,%.8f,%.8f,%.8f)",b.south,b.west,b.north,b.east);
    const std::string box=bbox;
    const std::string query="[out:json][timeout:45];(way[amenity=parking]"+box+";way[shop]"+box+
        ";node[shop]"+box+";node[entrance]"+box+";way[amenity=fuel]"+box+";node[amenity=fuel]"+box+
        ";nwr[amenity~\"^(police|school|kindergarten|college|university|hospital|clinic|doctors|prison|place_of_worship|townhall|courthouse|bank|fire_station|restaurant|cafe|bar|pub)$\"]"+box+
        ";nwr[office]"+box+";nwr[craft]"+box+";node[military=gendarmerie]"+box+";);out body;>;out skel qt;";
    std::string failures;
    for(int i:endpointOrder())try {
        auto doc=nlohmann::json::parse(net::requestJson(kOverpass[i],"data="+net::urlEncode(query),"application/x-www-form-urlencoded"));
        doc["r1RetailVersion"]=kRetailLayerVersion;writeJson(path,doc);return doc;
    } catch(const std::exception& e){markDown(i);failures+=std::string(failures.empty()?"":" | ")+kOverpass[i]+": "+e.what();}
    throw SourceUnavailable("retail observations unavailable: "+failures);
}

std::function<double(double, double)> ObservationStore::terrainSampler(int zoom, bool network) const {
    return imageSampler(root_, zoom, network);
}

std::vector<Peak> ObservationStore::peaks(const Tile& tile, const OsmData& osm, std::string* origin) const {
    if (osm.queryVersion >= kOsmPeaksVersion) {
        if (origin) *origin = "the neighbourhood's answer";
        return peaksFromFeatures(osm.features);
    }
    // An older answer did not ask: a place visited while the summits were
    // asked by square degree kept them so. Nothing asks Overpass again.
    std::vector<Peak> out;
    int kept = 0, cells = 0;
    std::error_code ec;
    for (const PeakCell& cell : peakCells(tile.bounds())) {
        ++cells;
        const std::string path = root_ + "/cache/world/peaks/" + cell.file();
        if (!fs::exists(path, ec)) continue;
        try {
            const auto peaks = peaksFromDocument(readJson(path));
            out.insert(out.end(), peaks.begin(), peaks.end());
            ++kept;
        } catch (const std::exception& e) {
            if (log_) log_("PEAKS-UNREADABLE " + cell.file() + " " + e.what());
        }
    }
    std::sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.id < b.id; });
    if (origin)
        *origin = kept == cells ? "square degrees on disk"
                  : kept      ? "square degrees on disk, some missing"
                              : "not asked: an answer older than question " + std::to_string(kOsmPeaksVersion);
    return out;
}

nlohmann::json ObservationStore::fetchSeaIce(double lon, double lat) const {
    const SeaIceWindow window = seaIceWindow(lon, lat);
    nlohmann::json doc;
    try {
        doc = seaIceDocument(nlohmann::json::parse(net::requestJson(window.url(), {}, {}, 90, 2)));
    } catch (const std::exception& e) {
        throw SourceUnavailable(std::string("sea ice (NOAA PolarWatch ASCAT): ") + e.what());
    }
    writeJson(root_ + "/cache/world/seaice/" + window.file(), doc);
    return doc;
}

double terrariumHeight(double lon, double lat, int zoom, const std::function<const unsigned char*(int, int)>& rgb) {
    const int images = 1 << zoom, pixels = images * 256;
    // Pixel centres sit half a pixel in from each image's edge.
    const double px = (lon + 180.0) / 360.0 * pixels - 0.5;
    const double py = std::clamp((1.0 - std::asinh(std::tan(radians(lat))) / M_PI) * 0.5 * pixels - 0.5,
                                 0.0, double(pixels) - 1.0);
    const int x0 = int(std::floor(px)), y0 = int(std::floor(py));
    const double fx = px - x0, fy = py - y0;
    auto at = [&](int x, int y) {
        x = ((x % pixels) + pixels) % pixels;  // the date line is not an edge
        y = std::clamp(y, 0, pixels - 1);
        const unsigned char* image = rgb(x >> 8, y >> 8);
        const size_t i = size_t(((y & 255) * 256 + (x & 255)) * 3);
        // Red 0 is no data (the format's heights start at -32768 m).
        if (image[i] == 0) return std::nan("");
        return image[i] * 256.0 + image[i + 1] + image[i + 2] / 256.0 - 32768.0;
    };
    const double low = at(x0, y0) * (1.0 - fx) + at(x0 + 1, y0) * fx;
    const double high = at(x0, y0 + 1) * (1.0 - fx) + at(x0 + 1, y0 + 1) * fx;
    return low * (1.0 - fy) + high * fy;
}

std::vector<unsigned char> terrariumPixels(const std::string& png) {
    int width = 0, height = 0, channels = 0;
    auto* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), int(png.size()),
                                         &width, &height, &channels, 3);
    if (!pixels || width != kTerrariumSize || height != kTerrariumSize) {
        stbi_image_free(pixels);
        throw SourceUnavailable("Mapzen terrain tile is not a 256px PNG");
    }
    std::vector<unsigned char> rgb(pixels, pixels + kTerrariumSize * kTerrariumSize * 3);
    stbi_image_free(pixels);
    return rgb;
}

std::string terrariumUrl(int zoom, int x, int y) {
    return "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/" + std::to_string(zoom) + "/" +
           std::to_string(x) + "/" + std::to_string(y) + ".png";
}

namespace {
// The public AWS Terrain Tiles archive is a global, already tiled DEM. A
// single 256px image covers many of our small world tiles, so one download
// replaces dozens of point requests to Open-Meteo during a teleport. Keep the
// source image on disk too: a return visit needs no network at all.
struct TerrainImage { std::vector<unsigned char> rgb; };

// `network` false: the image on disk or nothing, for a place already visited.
std::shared_ptr<const TerrainImage> terrainImage(const std::string& root, int zoom, int x, int y, bool network) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const TerrainImage>> memory;
    const std::string key = root + "/cache/world/terrain/" + std::to_string(zoom) + "/" + std::to_string(x) + "/" +
                            std::to_string(y) + ".png";
    std::lock_guard<std::mutex> guard(mutex);
    if (auto it = memory.find(key); it != memory.end()) return it->second;

    auto decode = [](const std::string& bytes) -> std::shared_ptr<const TerrainImage> {
        auto image = std::make_shared<TerrainImage>();
        image->rgb = terrariumPixels(bytes);
        return image;
    };
    // A tile's 41 x 41 samples straddle up to four images, and a teleport
    // reads a neighbourhood's worth of them.
    auto keep = [&](std::shared_ptr<const TerrainImage> image) {
        if (memory.size() >= 48) memory.erase(memory.begin());
        memory[key] = image;
        return image;
    };

    std::error_code ec;
    if (fs::exists(key, ec)) {
        try {
            std::ifstream input(key, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            return keep(decode(bytes));
        } catch (const std::exception&) {
            fs::remove(key, ec);  // a truncated download is never a cache hit
        }
    }
    if (!network) throw SourceUnavailable("Mapzen terrain tile z" + std::to_string(zoom) + " is not on disk");

    const auto response = net::request("GET", terrariumUrl(zoom, x, y), {}, {}, 12.0);
    if (response.status != 200) throw SourceUnavailable("Mapzen terrain tile: HTTP " + std::to_string(response.status));
    auto image = decode(response.body);
    try {
        fs::create_directories(fs::path(key).parent_path());
        const std::string tmp = key + ".tmp" + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()));
        { std::ofstream output(tmp, std::ios::binary); output.write(response.body.data(), std::streamsize(response.body.size())); }
        fs::rename(tmp, key, ec);
        if (ec) fs::remove(tmp, ec);
    } catch (const std::exception&) {
        // The fetched image is still valid for this session when the cache is
        // read-only. The next visit will retry the download.
    }
    return keep(image);
}

// The zoom the ground is read at: 13 is 19 m a pixel at the equator, finer
// than the 41 x 41 grid needs nowhere and coarser than what the archive holds
// (SRTM's 30 m) nowhere either. Beyond it the images are the same data
// resampled: a summit gains nothing past 13 (Lion's Head 630 m at 13, 14
// and 15; 605 m at 12).
constexpr int kTerrainZoom = 13;

std::string mapzenSource(int zoom) {
    return "Mapzen Terrain Tiles z" + std::to_string(zoom) + " (SRTM and open DEM via AWS)";
}

ElevationGrid mapzenGround(const Bounds& b, const std::string& root, int zoom, bool network) {
    if (b.south < -85.0 || b.north > 85.0) throw SourceUnavailable("Mapzen Mercator terrain stops at 85 degrees");
    const auto rgb = [&](int x, int y) { return terrainImage(root, zoom, x, y, network)->rgb.data(); };
    const int size = kTerrainMeshSize;
    ElevationGrid grid{b, size, {}};
    for (int row = 0; row < size; ++row) for (int col = 0; col < size; ++col) {
        const double h = terrariumHeight(b.west + (b.east - b.west) * col / (size - 1),
                                         b.south + (b.north - b.south) * row / (size - 1), zoom, rgb);
        if (!std::isfinite(h)) throw SourceUnavailable("Mapzen terrain tile has no elevation here");
        grid.values.push_back(h);
    }
    return grid;
}

// The finest Terrain Tiles ground there is: zoom 13, else the zoom 12 every
// place visited before the finer read kept on disk.
std::pair<ElevationGrid, std::string> terrainTiles(const Bounds& b, const std::string& root, bool network) {
    std::string failures;
    for (const int zoom : {kTerrainZoom, 12}) {
        try {
            return {mapzenGround(b, root, zoom, network), mapzenSource(zoom)};
        } catch (const SourceUnavailable& e) {
            failures += std::string(failures.empty() ? "" : " | ") + e.what();
        }
    }
    throw SourceUnavailable(failures);
}

// The images of zoom 11 and coarser ring: resampled from finer data, they
// overshoot by thousands of metres where a cliff meets the sea floor (Rio, by
// Vidigal: 4 109 m at zoom 11 where zoom 13 reads 137 and Copernicus 418). A
// 3 x 3 median takes the ringing out and leaves relief broader than a pixel
// (70 m and more there) as it was.
constexpr int kRingingZoom = 11;

std::vector<unsigned char> withoutRinging(const std::vector<unsigned char>& rgb) {
    auto height = [&](int x, int y) {
        const size_t i = size_t((y * 256 + x) * 3);
        return rgb[i] * 256.0 + rgb[i + 1] + rgb[i + 2] / 256.0;  // + 32768, kept encoded
    };
    std::vector<unsigned char> out(rgb);
    std::array<double, 9> around{};
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            if (rgb[size_t((y * 256 + x) * 3)] == 0) continue;  // no data stays no data
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int px = x + dx, py = y + dy;
                    if (px < 0 || py < 0 || px > 255 || py > 255 || rgb[size_t((py * 256 + px) * 3)] == 0) continue;
                    around[size_t(n++)] = height(px, py);
                }
            std::nth_element(around.begin(), around.begin() + n / 2, around.begin() + n);
            const double v = around[size_t(n / 2)];
            const size_t i = size_t((y * 256 + x) * 3);
            out[i] = (unsigned char)(int(v) / 256);
            out[i + 1] = (unsigned char)(int(v) % 256);
            out[i + 2] = (unsigned char)std::clamp(int((v - std::floor(v)) * 256.0), 0, 255);
        }
    return out;
}

// One caller's images of one zoom: each fetched or read once, then held.
std::function<double(double, double)> imageSampler(const std::string& root, int zoom, bool network) {
    auto held = std::make_shared<std::map<std::pair<int, int>, std::vector<unsigned char>>>();
    auto failed = std::make_shared<std::map<std::pair<int, int>, std::string>>();
    return [root, zoom, network, held, failed](double lon, double lat) {
        const auto rgb = [&](int x, int y) -> const unsigned char* {
            const auto key = std::make_pair(x, y);
            if (auto it = failed->find(key); it != failed->end()) throw SourceUnavailable(it->second);
            auto& image = (*held)[key];
            if (image.empty()) {
                try {
                    const auto source = terrainImage(root, zoom, x, y, network);
                    image = zoom <= kRingingZoom ? withoutRinging(source->rgb) : source->rgb;
                } catch (const std::exception& e) {
                    // A ring samples an image thousands of times. One failed
                    // read per sampler is enough; a new sampler retries it.
                    (*failed)[key] = e.what();
                    throw;
                }
            }
            return image.data();
        };
        const double h = terrariumHeight(lon, lat, zoom, rgb);
        if (!std::isfinite(h)) throw SourceUnavailable("Mapzen terrain tile has no elevation here");
        return h;
    };
}

// Copernicus GLO-90 through Open-Meteo, 7x7: its 90 m is all there is.
ElevationGrid copernicus(const Bounds& b, double timeout, int attempts) {
    const int size = 7;
    std::vector<P2> points;
    for (int r = 0; r < size; ++r)
        for (int c = 0; c < size; ++c)
            points.push_back({b.west + (b.east - b.west) * c / (size - 1), b.south + (b.north - b.south) * r / (size - 1)});
    ElevationGrid g{b, size, {}};
    for (size_t start = 0; start < points.size(); start += 100) {
        std::string lat, lon;
        char buf[64];
        for (size_t i = start; i < std::min(points.size(), start + 100); ++i) {
            std::snprintf(buf, sizeof buf, "%s%.8f", lat.empty() ? "" : ",", points[i].y); lat += buf;
            std::snprintf(buf, sizeof buf, "%s%.8f", lon.empty() ? "" : ",", points[i].x); lon += buf;
        }
        nlohmann::json doc;
        try {
            // The model has no value at the pole itself and says `nan`,
            // which JSON has no word for: it is read as a missing sample.
            std::string text = net::requestJson("https://api.open-meteo.com/v1/elevation?latitude=" + lat + "&longitude=" + lon, {}, {}, timeout, attempts);
            for (size_t at; (at = text.find("nan")) != std::string::npos;) text.replace(at, 3, "null");
            doc = nlohmann::json::parse(text);
        } catch (const std::exception& e) {
            throw SourceUnavailable(std::string("elevation service unavailable: ") + e.what());
        }
        const auto& values = doc["elevation"];
        if (!values.is_array() || values.size() != std::min(points.size(), start + 100) - start)
            throw SourceUnavailable("Open-Meteo returned an incomplete elevation batch");
        for (const auto& v : values) g.values.push_back(v.is_number() ? v.get<double>() : std::nan(""));
    }
    // A missing sample takes the nearest sample that has one.
    for (int i = 0; i < size * size; ++i) {
        if (std::isfinite(g.values[size_t(i)])) continue;
        double best = 1e300, value = std::nan("");
        for (int j = 0; j < size * size; ++j) {
            if (!std::isfinite(g.values[size_t(j)])) continue;
            const double d = std::hypot(double(i / size - j / size), double(i % size - j % size));
            if (d < best) { best = d; value = g.values[size_t(j)]; }
        }
        if (!std::isfinite(value)) throw SourceUnavailable("Open-Meteo has no elevation anywhere in the tile");
        g.values[size_t(i)] = value;
    }
    return g;
}

void writeGround(const std::string& path, const ElevationGrid& g, const std::string& source) {
    nlohmann::json rows = nlohmann::json::array();
    for (int r = 0; r < g.size; ++r) {
        nlohmann::json row = nlohmann::json::array();
        for (int c = 0; c < g.size; ++c) row.push_back(g.at(r, c));
        rows.push_back(row);
    }
    writeJson(path, {{"bounds", boundsJson(g.bounds)}, {"size", g.size}, {"values", rows}, {"source", source}});
}

bool ignEligible(const Bounds& b) { return -5.5 <= b.west && b.east <= 9.8 && 41.2 <= b.south && b.north <= 51.2; }
}  // namespace

std::vector<unsigned char> terrariumReliefPixels(const std::string& png, int zoom) {
    auto pixels = terrariumPixels(png);
    return zoom <= kRingingZoom ? withoutRinging(pixels) : pixels;
}

std::pair<ElevationGrid, std::string> ObservationStore::fetchGround(const Tile& tile) const {
    if (auto disk = ground(tile)) return *disk;
    const Bounds b = tile.bounds();
    const std::string path = tileFolder(tile) + "/ground-elevation.json";
    // IGN's bare-earth survey where France might be; every sample validated.
    const bool eligible = ignEligible(b);
    if (eligible) {
        const int size = 41;
        std::string lons, lats;
        char buf[64];
        for (int r = 0; r < size; ++r)
            for (int c = 0; c < size; ++c) {
                std::snprintf(buf, sizeof buf, "%s%.10f", lons.empty() ? "" : "|", b.west + (b.east - b.west) * c / (size - 1));
                lons += buf;
                std::snprintf(buf, sizeof buf, "%s%.10f", lats.empty() ? "" : "|", b.south + (b.north - b.south) * r / (size - 1));
                lats += buf;
            }
        const nlohmann::json payload = {{"lon", lons}, {"lat", lats}, {"resource", "ign_rge_alti_wld"},
                                        {"delimiter", "|"}, {"zonly", "true"}};
        try {
            const auto doc = nlohmann::json::parse(net::requestJson(
                "https://data.geopf.fr/altimetrie/1.0/calcul/alti/rest/elevation.json", payload.dump(), "application/json", 20, 2));
            const auto& values = doc.at("elevations");
            if (!values.is_array() || int(values.size()) != size * size) throw std::runtime_error("IGN returned incomplete or no-data terrain");
            nlohmann::json rows = nlohmann::json::array();
            ElevationGrid g{b, size, {}};
            for (int r = 0; r < size; ++r) {
                nlohmann::json row = nlohmann::json::array();
                for (int c = 0; c < size; ++c) {
                    const auto& v = values[size_t(r * size + c)];
                    if (!v.is_number()) throw std::runtime_error("IGN returned incomplete or no-data terrain");
                    const double h = v.get<double>();
                    if (!std::isfinite(h) || h <= -1000 || h > 9000) throw std::runtime_error("IGN returned incomplete or no-data terrain");
                    row.push_back(h);
                    g.values.push_back(h);
                }
                rows.push_back(row);
            }
            const std::string source = "IGN RGE ALTI bare-earth terrain via Geoplateforme";
            writeJson(path, {{"bounds", boundsJson(b)}, {"size", size}, {"values", rows}, {"source", source}});
            return {g, source};
        } catch (const std::exception& e) {
            if (log_) log_(std::string("ELEVATION-FALLBACK ") + e.what());
        }
    }
    ElevationGrid g;
    std::string source;
    try {
        std::tie(g, source) = terrainTiles(b, root_, true);
    } catch (const std::exception& e) {
        if (log_) log_(std::string("ELEVATION-FALLBACK ") + e.what());
        g = copernicus(b, 120.0, 3);
        source = "Copernicus DEM GLO-90 via Open-Meteo (fallback)";
    }
    writeGround(path, g, source);
    return {g, source};
}

std::optional<std::pair<ElevationGrid, std::string>> ObservationStore::offlineGround(const Bounds& b) const {
    // Nothing answers: the installed layer, never written to the cache, so
    // the finer ground replaces it as soon as a source answers again.
    auto grid = installedGround(b, root_);
    if (!grid) return std::nullopt;
    if (log_) log_(std::string("ELEVATION-INSTALLED ") + kInstalledReliefSource);
    return std::make_pair(std::move(*grid), std::string(kInstalledReliefSource));
}

std::pair<ElevationGrid, std::string> ObservationStore::quickGround(const Tile& tile) const {
    const Bounds b = tile.bounds();
    ElevationGrid g;
    std::string source;
    try {
        std::tie(g, source) = terrainTiles(b, root_, true);
    } catch (const std::exception& e) {
        if (log_) log_(std::string("ELEVATION-FALLBACK ") + e.what());
        g = copernicus(b, 4.0, 1);
        source = "Copernicus DEM GLO-90 via Open-Meteo (fallback)";
    }
    // Outside France this is the ground `fetchGround` would have fetched: it
    // is kept. Inside, IGN's finer survey still comes with the upgrade.
    if (!ignEligible(b)) writeGround(tileFolder(tile) + "/ground-elevation.json", g, source);
    return {g, source};
}

// ── SHA-256, for the shared queries' file names ─────────────────────────────

std::string sha256Hex(const std::string& data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    const uint64_t bits = uint64_t(data.size()) * 8;
    msg += char(0x80);
    while (msg.size() % 64 != 56) msg += char(0);
    for (int i = 7; i >= 0; --i) msg += char((bits >> (i * 8)) & 0xff);
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(msg[chunk + i * 4])) << 24) | (uint32_t(uint8_t(msg[chunk + i * 4 + 1])) << 16) |
                   (uint32_t(uint8_t(msg[chunk + i * 4 + 2])) << 8) | uint32_t(uint8_t(msg[chunk + i * 4 + 3]));
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::ostringstream out;
    for (uint32_t v : h) { char buf[9]; std::snprintf(buf, sizeof buf, "%08x", v); out << buf; }
    return out.str();
}

}  // namespace r1
