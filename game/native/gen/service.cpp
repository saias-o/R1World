#include "service.hpp"

#include "sources.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>
#include <thread>

namespace r1 {

using Clock = std::chrono::steady_clock;

namespace {
// After the network fails, this long before it is asked again: a tile meanwhile
// is cooked from Natural Earth, and upgraded once the network answers.
constexpr auto kOfflinePause = std::chrono::seconds(60);
// A tile that failed to cook is not retried before this: the same input would
// fail the same way, and saying so once a minute is enough.
constexpr auto kFailurePause = std::chrono::seconds(60);

// Overpass allows two queries at once per client (its "slots"): the game
// takes two, never more, whoever is asking.
class Slots {
public:
    explicit Slots(int n) : free_(n) {}
    void acquire() { std::unique_lock<std::mutex> g(m_); cv_.wait(g, [&] { return free_ > 0; }); --free_; }
    void release() { { std::lock_guard<std::mutex> g(m_); ++free_; } cv_.notify_one(); }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int free_;
};

struct Counter {
    std::atomic<int>& n;
    explicit Counter(std::atomic<int>& c) : n(c) { ++n; }
    ~Counter() { --n; }
};
}  // namespace

struct WorldService::State : std::enable_shared_from_this<WorldService::State> {
    Options options;
    ObservationStore store;
    mutable std::mutex lock;
    std::condition_variable wake;
    std::vector<Tile> wanted;
    std::vector<std::vector<Tile>> groups;
    std::map<Tile, std::shared_ptr<const ServedTile>> cooked;
    std::map<Tile, uint64_t> lastWanted;
    std::map<Tile, Clock::time_point> failedUntil;
    // Independent observations waiting to upgrade each tile. Kept in memory:
    // `next` runs under the lock the game's frame takes too, and must not read disk.
    std::map<Tile, std::set<std::string>> awaiting;
    std::set<std::string> landed;
    std::map<Tile, std::pair<ElevationGrid, std::string>> quickGrounds;
    std::set<Tile> busy;
    std::string error;
    Clock::time_point offlineUntil{};
    std::atomic<int> fetching{0}, cooking{0};
    uint64_t clock = 0, serial = 0;
    bool stopping = false;
    Slots overpass{2};
    Slots quickGroundSlots{4};
    Slots detailedGroundSlots{2};
    Slots canopySlots{1};
    // Overpass queries in flight, by the file they will write: one download
    // serves every tile that reads that file, and nobody asks for it twice.
    std::set<std::string> downloads;
    std::map<std::string, Clock::time_point> sourceFailedUntil;
    std::mutex downloading;
    int threads = 0;

    // Parsed observations by file: nine tiles share one neighbourhood query,
    // and reading ten megabytes of JSON nine times was most of a tile's wait.
    using Parsed = std::shared_future<std::shared_ptr<const OsmData>>;
    struct Source { Parsed data; std::filesystem::file_time_type stamp; };
    std::map<std::string, Source> sources;
    std::mutex parsing;

    // Sea-ice windows, parsed once and shared by the tiles of their block.
    std::map<std::string, std::shared_ptr<const SeaIce>> seaIce;
    std::map<std::string, Clock::time_point> seaIceFailed;
    std::set<std::string> seaIceFetching;
    std::mutex seaIceLock;

    explicit State(Options o)
        : options(std::move(o)), store(options.gameRoot, [this](const std::string& line) { say(line); }) {}

    // The first worker to want a file parses it; the others wait for that
    // parse rather than starting their own, and other files go on meanwhile.
    std::shared_ptr<const OsmData> source(const std::string& path, const std::optional<std::string>& layer,
                                        const std::optional<std::string>& retail) {
        std::error_code ec;
        auto stamp = std::filesystem::last_write_time(path, ec);
        // The same answer with and without its aero layer are two sources.
        const std::string key = path+(layer?"|"+*layer:"")+(retail?"|"+*retail:"");
        if (layer) stamp = std::max(stamp, std::filesystem::last_write_time(*layer, ec));
        if (retail) stamp = std::max(stamp, std::filesystem::last_write_time(*retail, ec));
        std::promise<std::shared_ptr<const OsmData>> promise;
        Parsed parsed;
        bool mine = false;
        {
            std::lock_guard<std::mutex> guard(parsing);
            auto it = sources.find(key);
            if (it != sources.end() && it->second.stamp == stamp) parsed = it->second.data;
            else {
                if (sources.size() >= 8) sources.erase(sources.begin());
                parsed = promise.get_future().share();
                sources[key] = {parsed, stamp};
                mine = true;
            }
        }
        if (mine) {
            try {
                const nlohmann::json main = readJson(path);
                const auto aero=layer?readJson(*layer):nlohmann::json();
                const auto shops=retail?readJson(*retail):nlohmann::json();
                promise.set_value(std::make_shared<const OsmData>(normalizeOsm(main,layer?&aero:nullptr,retail?&shops:nullptr)));
            } catch (...) {
                promise.set_exception(std::current_exception());
                std::lock_guard<std::mutex> guard(parsing);
                sources.erase(key);
            }
        }
        return parsed.get();
    }

    void say(const std::string& line) const { if (options.log) options.log(line); }

    bool requested(const std::string& path) const {
        std::lock_guard<std::mutex> guard(lock);
        for (const Tile& tile : wanted) {
            auto it = awaiting.find(tile);
            if (it != awaiting.end() && it->second.count(path)) return true;
        }
        return false;
    }

    void watch(const Tile& tile, const std::string& path) {
        std::lock_guard<std::mutex> guard(lock);
        awaiting[tile].insert(path);
    }

    void unwatch(const Tile& tile, const std::string& path, bool deferred = false) {
        Clock::time_point retry{};
        if (deferred) {
            std::lock_guard<std::mutex> guard(downloading);
            retry = sourceFailedUntil[path];
        }
        std::lock_guard<std::mutex> guard(lock);
        auto it = awaiting.find(tile);
        if (it != awaiting.end()) {
            it->second.erase(path);
            if (it->second.empty()) awaiting.erase(it);
        }
        if (deferred) failedUntil[tile] = std::max(failedUntil[tile], retry);
    }

    // Under `lock`: prefer a newly arrived source for the visible tile, then
    // uncooked tiles, then upgrades elsewhere. A failed source never pauses a
    // different source or a tile whose observations are already on disk.
    std::optional<Tile> next(bool& upgrade) {
        const auto now = Clock::now();
        auto arrived = [&](const Tile& t) {
            auto it = awaiting.find(t);
            if (it == awaiting.end() || busy.count(t) || !cooked.count(t)) return false;
            bool any = false;
            for (auto p = it->second.begin(); p != it->second.end();) {
                if (landed.count(*p)) { p = it->second.erase(p); any = true; }
                else ++p;
            }
            if (it->second.empty()) awaiting.erase(it);
            return any;
        };
        if (!wanted.empty() && !busy.count(wanted.front()) && !cooked.count(wanted.front())) {
            auto failed = failedUntil.find(wanted.front());
            if (failed == failedUntil.end() || now >= failed->second) {
                upgrade = false;
                return wanted.front();
            }
        }
        // Arrival around the player takes precedence over speculative tiles
        // along a vehicle's forecast corridor.
        if (!groups.empty()) for (const Tile& t : groups.front()) {
            if (std::find(wanted.begin(), wanted.end(), t) == wanted.end() || !arrived(t)) continue;
            upgrade = true;
            return t;
        }
        for (const Tile& t : wanted) {
            if (busy.count(t) || cooked.count(t)) continue;
            auto failed = failedUntil.find(t);
            if (failed != failedUntil.end() && now < failed->second) continue;
            upgrade = false;
            return t;
        }
        for (const Tile& t : wanted) {
            if (!arrived(t)) continue;
            upgrade = true;
            return t;
        }
        for (const Tile& t : wanted) {
            auto it = cooked.find(t);
            if (it == cooked.end() || busy.count(t) || awaiting.count(t) ||
                !(it->second->cooked.manifest.value("offlineApproximation", false) ||
                  it->second->cooked.manifest.value("airportsPending", false))) continue;
            auto failed = failedUntil.find(t);
            if (failed != failedUntil.end() && now < failed->second) continue;
            upgrade = true;
            return t;
        }
        return std::nullopt;
    }

    std::optional<ObservationStore::Shared> sharedFor(const Tile& tile) const {
        std::lock_guard<std::mutex> guard(lock);
        return sharedLocked(tile);
    }
    // Under `lock`.
    std::optional<ObservationStore::Shared> sharedLocked(const Tile& tile) const {
        for (const auto& group : groups)
            if (std::find(group.begin(), group.end(), tile) != group.end()) return store.shared(group);
        return store.shared({tile});
    }

    void failedDownload(const std::string& path, const std::string& companion = {}) {
        const auto retry = Clock::now() + kOfflinePause;
        {
            std::lock_guard<std::mutex> guard(downloading);
            sourceFailedUntil[path] = retry;
        }
        {
            std::lock_guard<std::mutex> guard(lock);
            offlineUntil = retry;  // status for the player, never a gate for other sources
            for (auto it = awaiting.begin(); it != awaiting.end();) {
                bool affected = it->second.erase(path) != 0;
                if (!companion.empty()) affected = it->second.erase(companion) != 0 || affected;
                if (affected) failedUntil[it->first] = std::max(failedUntil[it->first], retry);
                if (it->second.empty()) it = awaiting.erase(it);
                else ++it;
            }
        }
    }

    // An Overpass query for `region`, written to `path`, on its own thread.
    // `aero` asks for the aero layer instead of the whole neighbourhood.
    bool download(const Bounds& region, const std::string& path, bool aero = false,bool retail=false) {
        std::lock_guard<std::mutex> guard(downloading);
        if (downloads.count(path)) return true;
        if (sourceFailedUntil[path] > Clock::now()) return false;
        downloads.insert(path);
        std::thread([self = shared_from_this(), region, path, aero, retail] {
            Counter c(self->fetching);
            self->overpass.acquire();
            const auto asked = Clock::now();
            try {
                // A teleport can leave many queries waiting for the two
                // Overpass slots. Do not spend a slot on an old destination.
                if (!self->requested(path)) {
                    self->say("OSM-SKIPPED " + std::filesystem::path(path).filename().string());
                } else {
                    if (retail) {
                        self->store.fetchRetail(region,path);
                    } else if (aero) {
                        self->store.fetchAero(region, path);
                    } else {
                        if (self->options.fetchOsm) self->options.fetchOsm(region, path);
                        else self->store.fetchOsm(region, path);
                    }
                    {
                        std::lock_guard<std::mutex> guard(self->lock);
                        self->landed.insert(path);
                    }
                    self->say(std::string(retail ? "RETAIL-LAYER " : aero ? "AERO-LAYER " : "OSM ") + std::filesystem::path(path).filename().string() + " in " +
                              std::to_string(int(std::chrono::duration<double>(Clock::now() - asked).count())) + " s");
                }
            } catch (const std::exception& e) {
                self->say(std::string(retail ? "RETAIL-LAYER-FAILED " : aero ? "AERO-LAYER-FAILED " : "OSM-QUERY-FAILED ") +
                          std::filesystem::path(path).filename().string() + " " + e.what());
                self->failedDownload(path);
            }
            self->overpass.release();
            {
                std::lock_guard<std::mutex> g(self->downloading);
                self->downloads.erase(path);
            }
            self->wake.notify_all();
        }).detach();
        return true;
    }

    // What a tile waits on for its canopy; a band converts many at once.
    static std::string canopyKey(const Tile& tile) { return "canopy:" + tile.key(); }

    // One band of the canopy source, converted into every tile it covers
    // (gen/canopy): the tiles along the same row that were waiting cook again.
    bool downloadCanopy(const Tile& tile) {
        const std::string key = canopyKey(tile);
        const std::string band = "canopy-band:" + std::to_string(tile.row) + ":" +
                                 std::to_string(int(std::floor((tile.center().x + 180.0) / (360.0 / 512))));
        std::lock_guard<std::mutex> guard(downloading);
        if (downloads.count(band)) return true;
        if (sourceFailedUntil[band] > Clock::now()) return false;
        downloads.insert(band);
        std::thread([self = shared_from_this(), tile, key, band] {
            Counter c(self->fetching);
            self->canopySlots.acquire();
            if (self->requested(key)) {
                const auto asked = Clock::now();
                try {
                    const auto converted = fetchCanopyBand(tile);
                    size_t trees = 0;
                    for (const auto& [t, canopy] : converted) {
                        storeCanopy(self->store.root(), t, canopy);
                        trees += size_t(canopy.count(Canopy::Tree));
                    }
                    {
                        std::lock_guard<std::mutex> g(self->lock);
                        for (const auto& [t, canopy] : converted) self->landed.insert(canopyKey(t));
                    }
                    self->say("CANOPY row " + std::to_string(tile.row) + ": " + std::to_string(converted.size()) +
                              " tiles, " + std::to_string(trees) + " tree cells in " +
                              std::to_string(int(std::chrono::duration<double>(Clock::now() - asked).count())) + " s");
                } catch (const std::exception& e) {
                    self->say("CANOPY-FAILED " + tile.key() + " " + e.what());
                    {
                        std::lock_guard<std::mutex> g(self->downloading);
                        self->sourceFailedUntil[band] = Clock::now() + kOfflinePause;
                    }
                    self->failedDownload(key);
                }
            }
            self->canopySlots.release();
            {
                std::lock_guard<std::mutex> g(self->downloading);
                self->downloads.erase(band);
            }
            self->wake.notify_all();
        }).detach();
        return true;
    }

    // A square degree's surveyed summits: one small Overpass answer serves
    // every tile in it, and every tile within a summit's reach of its edge.
    static std::string peaksKey(const PeakCell& cell) { return "peaks:" + cell.file(); }

    bool downloadPeaks(const PeakCell& cell) {
        const std::string key = peaksKey(cell);
        std::lock_guard<std::mutex> guard(downloading);
        if (downloads.count(key)) return true;
        if (sourceFailedUntil[key] > Clock::now()) return false;
        downloads.insert(key);
        std::thread([self = shared_from_this(), cell, key] {
            Counter c(self->fetching);
            self->overpass.acquire();
            if (self->requested(key)) {
                try {
                    const auto peaks = self->store.fetchPeaks(cell);
                    {
                        std::lock_guard<std::mutex> g(self->lock);
                        self->landed.insert(key);
                    }
                    self->say("PEAKS " + cell.file() + ": " + std::to_string(peaks.size()) + " surveyed summits");
                } catch (const std::exception& e) {
                    self->say("PEAKS-FAILED " + cell.file() + " " + e.what());
                    self->failedDownload(key);
                }
            }
            self->overpass.release();
            {
                std::lock_guard<std::mutex> g(self->downloading);
                self->downloads.erase(key);
            }
            self->wake.notify_all();
        }).detach();
        return true;
    }

    static std::string groundPath(const Tile& tile, const ObservationStore& store) {
        return store.tileFolder(tile) + "/ground-elevation.json";
    }

    // Publish a quick terrain sample first; the surveyed ground can take much
    // longer and must not hold back streets or the minimap.
    bool downloadGround(const Tile& tile) {
        const std::string path = groundPath(tile, store);
        const std::string quick = path + ".quick";
        std::lock_guard<std::mutex> guard(downloading);
        if (downloads.count(path)) return true;
        if (sourceFailedUntil[path] > Clock::now()) return false;
        downloads.insert(path);
        std::thread([self = shared_from_this(), tile, path, quick] {
            Counter c(self->fetching);
            self->quickGroundSlots.acquire();
            if (self->requested(path)) {
                try {
                    auto ground = self->options.quickGround ? self->options.quickGround(tile) : self->store.quickGround(tile);
                    {
                        std::lock_guard<std::mutex> guard(self->lock);
                        if (std::find(self->wanted.begin(), self->wanted.end(), tile) != self->wanted.end()) {
                            self->quickGrounds[tile] = std::move(ground);
                            self->landed.insert(quick);
                        }
                    }
                    self->wake.notify_all();
                } catch (const std::exception& e) {
                    self->say("QUICK-GROUND-FAILED " + tile.key() + " " + e.what());
                }
            }
            self->quickGroundSlots.release();
            self->detailedGroundSlots.acquire();
            if (self->requested(path)) {
                try {
                    if (self->options.fetchGround) self->options.fetchGround(tile);
                    else self->store.fetchGround(tile);
                    {
                        std::lock_guard<std::mutex> guard(self->lock);
                        self->quickGrounds.erase(tile);
                        self->landed.insert(quick);
                        self->landed.insert(path);
                    }
                    self->say("GROUND " + tile.key() + " ready");
                } catch (const std::exception& e) {
                    self->say("GROUND-FAILED " + tile.key() + " " + e.what());
                    self->failedDownload(path, quick);
                }
            }
            self->detailedGroundSlots.release();
            {
                std::lock_guard<std::mutex> g(self->downloading);
                self->downloads.erase(path);
            }
            self->wake.notify_all();
        }).detach();
        return true;
    }

    bool urgent(const Tile& tile) const {
        std::lock_guard<std::mutex> guard(lock);
        return !wanted.empty() && wanted.front() == tile;
    }

    // The sea ice a polar tile is cooked with. On disk it is used as it is.
    // Otherwise the climatology answers at once, and says it is inferred;
    // on a first visit (`mayFetch`) the reading is fetched meanwhile, on a
    // thread of its own, and the tile is cooked again when it lands -- the
    // aero layer's path. A slow server never holds up an arrival, and a
    // place already visited never touches the network (CLAUDE.md §7).
    std::shared_ptr<const SeaIce> seaIceFor(const Tile& tile, const ElevationGrid& ground, bool mayFetch) {
        const P2 c = tile.center();
        if (std::abs(c.y) < 60) return nullptr;
        bool seaLevel = false;
        for (double h : ground.values) seaLevel |= std::abs(h) < 0.5;
        if (!seaLevel) return nullptr;
        if (!seaIceGridCovers(c.x, c.y)) return std::make_shared<const SeaIce>(inferredSeaIce(c.y));
        const std::string file = seaIceWindow(c.x, c.y).file();
        std::lock_guard<std::mutex> guard(seaIceLock);
        auto it = seaIce.find(file);
        if (it != seaIce.end()) return it->second;
        try {
            if (const auto doc = store.seaIce(c.x, c.y)) {
                auto parsed = std::make_shared<const SeaIce>(seaIceFrom(*doc));
                seaIce[file] = parsed;
                return parsed;
            }
        } catch (const std::exception& e) {
            say(std::string("SEA-ICE-UNREADABLE ") + file + " " + e.what());
        }
        const auto failed = seaIceFailed.find(file);
        const bool resting = failed != seaIceFailed.end() && Clock::now() < failed->second;
        if (mayFetch && !resting) {
            {
                std::lock_guard<std::mutex> g(lock);
                awaiting[tile].insert("seaice:" + file);
            }
            if (seaIceFetching.insert(file).second) {
                std::thread([self = shared_from_this(), c, file] {
                    Counter counting(self->fetching);
                    bool ok = false;
                    try {
                        const auto doc = self->store.fetchSeaIce(c.x, c.y);
                        self->say("SEA-ICE " + file + " " + doc.value("date", std::string()));
                        ok = true;
                    } catch (const std::exception& e) {
                        self->say(std::string("SEA-ICE-UNAVAILABLE ") + e.what() + " (the climatology stays)");
                    }
                    {
                        std::lock_guard<std::mutex> g(self->seaIceLock);
                        self->seaIceFetching.erase(file);
                        if (!ok) self->seaIceFailed[file] = Clock::now() + kOfflinePause;
                    }
                    {
                        std::lock_guard<std::mutex> g(self->lock);
                        const std::string source = "seaice:" + file;
                        if (ok) self->landed.insert(source);
                        else for (auto it = self->awaiting.begin(); it != self->awaiting.end();) {
                            it->second.erase(source);
                            if (it->second.empty()) it = self->awaiting.erase(it);
                            else ++it;
                        }
                    }
                    self->wake.notify_all();
                }).detach();
            }
        }
        // A reading on its way, whoever asked for it: this tile is cooked
        // again when it lands, like the one that asked.
        if (seaIceFetching.count(file)) {
            std::lock_guard<std::mutex> g(lock);
            awaiting[tile].insert("seaice:" + file);
        }
        return std::make_shared<const SeaIce>(inferredSeaIce(c.y));
    }

    // OSM and relief are independent. Every cook uses whatever has arrived;
    // missing sources continue in the background and wake a new cook as soon
    // as each one lands. An old OSM answer is usable while its update arrives.
    Observations observe(const Tile& tile) {
        Observations in;
        in.tile = tile;
        const auto shared = sharedFor(tile);
        bool stale = false;
        auto document = store.osmPath(tile, shared, &stale);
        auto ground = store.ground(tile);
        const bool firstVisit = !document || stale || !ground;
        if (!document || stale) {
            const std::string path = shared ? shared->path : store.tileFolder(tile) + "/osm.json";
            watch(tile, path);
            if (!download(shared ? shared->region : tile.bounds(), path)) unwatch(tile, path, true);
        }
        if (!ground) {
            {
                std::lock_guard<std::mutex> guard(lock);
                auto quick = quickGrounds.find(tile);
                if (quick != quickGrounds.end()) ground = quick->second;
            }
            const std::string path = groundPath(tile, store);
            watch(tile, path);
            if (!ground) watch(tile, path + ".quick");
            if (!downloadGround(tile)) {
                unwatch(tile, path, true);
                unwatch(tile, path + ".quick");
            }
            in.groundPending = true;
        }
        if (document) {
            bool needed = false;
            const auto layer = store.aeroPath(tile, shared, *document, needed);
            if (needed && !layer) {
                in.airportsPending = true;
                const auto target = store.aeroTarget(tile, shared);
                watch(tile, target.path);
                if (!download(target.region, target.path, true)) unwatch(tile, target.path, true);
            }
            const auto retail=store.retailPath(tile,shared,*document);
            if(options.enrichRetail&&store.queryVersion(*document)<kOsmQueryVersion&&(!retail||!store.retailCurrent(*retail))) {
                const auto target=store.retailTarget(tile,shared);watch(tile,target.path);
                if(!download(target.region,target.path,false,true))unwatch(tile,target.path,true);
            }
            in.osm = source(*document, layer, retail);
            in.osmExtent = store.regionOf(tile, shared, *document);
        } else {
            in.osm = std::make_shared<const OsmData>();
            in.offline = true;
            in.provisional = true;
        }
        if (ground) {
            in.elevations = ground->first;
            in.elevationSource = ground->second;
        } else {
            in.elevations = ElevationGrid{tile.bounds(), 2, {0.0, 0.0, 0.0, 0.0}};
            in.elevationSource = "temporary flat ground (relief pending)";
        }
        in.around = store.groundAround(tile);
        in.seaIce = seaIceFor(tile, in.elevations, firstVisit);
        try {
            in.canopy = storedCanopy(store.root(), tile);
        } catch (const std::exception& e) {
            say("CANOPY-UNREADABLE " + tile.key() + " " + e.what());
        }
        if (!in.canopy && options.fetchCanopy) {
            in.canopyPending = true;
            watch(tile, canopyKey(tile));
            if (!downloadCanopy(tile)) unwatch(tile, canopyKey(tile), true);
        }
        for (const PeakCell& cell : peakCells(tile.bounds())) {
            std::optional<std::vector<Peak>> peaks;
            try {
                peaks = store.peaks(cell);
            } catch (const std::exception& e) {
                say("PEAKS-UNREADABLE " + cell.file() + " " + e.what());
            }
            if (peaks) {
                in.peaks.insert(in.peaks.end(), peaks->begin(), peaks->end());
            } else if (options.fetchPeaks) {
                // The tile is cooked now on the relief as it is, and again
                // when its summits land; a failed list never holds it back.
                in.peaksPending = true;
                watch(tile, peaksKey(cell));
                if (!downloadPeaks(cell)) unwatch(tile, peaksKey(cell));
            }
        }
        std::sort(in.peaks.begin(), in.peaks.end(), [](const Peak& a, const Peak& b) { return a.id < b.id; });
        return in;
    }

    void cook(const Tile& tile, bool upgrade) {
        Counter c(cooking);
        const auto started = Clock::now();
        std::shared_ptr<ServedTile> served = std::make_shared<ServedTile>();
        try {
            Observations in = observe(tile);
            in.targetVertices = options.tileVertexTarget;
            served->cooked = cookTile(in);
            if (options.prepare) options.prepare(*served);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> guard(lock);
            failedUntil[tile] = Clock::now() + kFailurePause;
            error = tile.key() + ": " + e.what();
            say("ERROR " + error);
            return;
        }
        std::lock_guard<std::mutex> guard(lock);
        // The aero layer landed and there is nothing aeronautical here: the
        // tile the game has mounted is already right, and mounting it again
        // would only be a flash for nothing.
        const auto previous = cooked.find(tile);
        auto relevant = [](const CookedTile& t) {
            const auto a = t.manifest.find("airports");
            return a != t.manifest.end() && a->value("relevant", false);
        };
        if (upgrade && previous != cooked.end() && previous->second->cooked.manifest.value("airportsPending", false) &&
            served->cooked.manifest.at("retail")==previous->second->cooked.manifest.at("retail") &&
            served->cooked.manifest.at("interiors")==previous->second->cooked.manifest.at("interiors") &&
            !relevant(served->cooked) && !relevant(previous->second->cooked))
            return;
        served->serial = ++serial;
        const bool approximate = served->cooked.manifest.value("offlineApproximation", false);
        const bool osmPending = served->cooked.manifest.value("provisional", false);
        const bool groundPending = served->cooked.manifest.value("groundPending", false);
        say(std::string(upgrade ? "UPGRADED " : osmPending ? "OSM-PENDING-READY " :
                        groundPending ? "GROUND-PENDING-READY " : "READY ") + tile.key() +
            " osm_pending=" + std::to_string(int(osmPending)) + " ground_pending=" + std::to_string(int(groundPending)) + " cook_ms=" +
            std::to_string(int(served->cooked.cookMs)) + " total_ms=" +
            std::to_string(int(std::chrono::duration<double, std::milli>(Clock::now() - started).count())));
        if (error.rfind(tile.key(), 0) == 0) error.clear();
        cooked[tile] = std::move(served);
        if (!approximate && !cooked[tile]->cooked.manifest.value("airportsPending", false)) failedUntil.erase(tile);
        // Keep what is wanted and what was wanted last; forget the oldest.
        while (cooked.size() > options.keep) {
            auto oldest = cooked.end();
            for (auto it = cooked.begin(); it != cooked.end(); ++it) {
                if (std::find(wanted.begin(), wanted.end(), it->first) != wanted.end()) continue;
                if (oldest == cooked.end() || lastWanted[it->first] < lastWanted[oldest->first]) oldest = it;
            }
            if (oldest == cooked.end()) break;
            cooked.erase(oldest);
        }
    }

    static void run(std::shared_ptr<State> self) {
        for (;;) {
            Tile tile;
            bool upgrade = false;
            {
                std::unique_lock<std::mutex> guard(self->lock);
                std::optional<Tile> found;
                // Woken by a new request or by a finished cook; the timeout
                // is for the offline pause and the failure pause running out.
                self->wake.wait_for(guard, std::chrono::seconds(1), [&] {
                    return self->stopping || (found = self->next(upgrade)).has_value();
                });
                if (self->stopping) return;
                if (!found) continue;
                tile = *found;
                self->busy.insert(tile);
            }
            self->cook(tile, upgrade);
            {
                std::lock_guard<std::mutex> guard(self->lock);
                self->busy.erase(tile);
            }
            self->wake.notify_all();
        }
    }
};

WorldService::WorldService(Options options) : state_(std::make_shared<State>(std::move(options))) {
    int threads = state_->options.threads;
    if (threads <= 0) threads = std::max(1, std::min(6, int(std::thread::hardware_concurrency()) - 2));
    state_->threads = threads;
    for (int i = 0; i < threads; ++i) std::thread(State::run, state_).detach();
    state_->say("world service: " + std::to_string(threads) + " cooking threads");
}

WorldService::~WorldService() {
    {
        std::lock_guard<std::mutex> guard(state_->lock);
        state_->stopping = true;
    }
    state_->wake.notify_all();
}

void WorldService::want(std::vector<Tile> priority, std::vector<std::vector<Tile>> groups) {
    {
        std::lock_guard<std::mutex> guard(state_->lock);
        state_->wanted = std::move(priority);
        state_->groups = std::move(groups);
        ++state_->clock;
        for (const Tile& t : state_->wanted) state_->lastWanted[t] = state_->clock;
        // A destination change must release observations queued for places
        // which are no longer relevant. Otherwise old Overpass and elevation
        // requests monopolise the network for minutes after a teleport.
        const std::set<Tile> current(state_->wanted.begin(), state_->wanted.end());
        for (auto it = state_->awaiting.begin(); it != state_->awaiting.end();)
            if (!current.count(it->first)) it = state_->awaiting.erase(it);
            else ++it;
        for (auto it = state_->quickGrounds.begin(); it != state_->quickGrounds.end();)
            if (!current.count(it->first)) it = state_->quickGrounds.erase(it);
            else ++it;
        std::set<std::string> awaitedPaths;
        for (const auto& [tile, paths] : state_->awaiting) awaitedPaths.insert(paths.begin(), paths.end());
        for (auto it = state_->landed.begin(); it != state_->landed.end();)
            if (!awaitedPaths.count(*it)) it = state_->landed.erase(it);
            else ++it;
    }
    state_->wake.notify_all();
}

std::shared_ptr<const ServedTile> WorldService::find(const Tile& tile) const {
    std::lock_guard<std::mutex> guard(state_->lock);
    auto it = state_->cooked.find(tile);
    return it == state_->cooked.end() ? nullptr : it->second;
}

WorldService::Status WorldService::status() const {
    std::lock_guard<std::mutex> guard(state_->lock);
    return {state_->error, Clock::now() < state_->offlineUntil, state_->cooking.load(), state_->fetching.load(),
            state_->threads};
}

}  // namespace r1
