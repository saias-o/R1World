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
    std::set<Tile> busy;
    std::string error;
    Clock::time_point offlineUntil{};
    std::atomic<int> fetching{0}, cooking{0};
    uint64_t clock = 0, serial = 0;
    bool stopping = false;
    Slots overpass{2};
    // Overpass queries in flight, by the file they will write: one download
    // serves every tile that reads that file, and nobody asks for it twice.
    std::map<std::string, std::shared_future<void>> downloads;
    std::mutex downloading;
    int threads = 0;

    // Parsed observations by file: nine tiles share one neighbourhood query,
    // and reading ten megabytes of JSON nine times was most of a tile's wait.
    using Parsed = std::shared_future<std::shared_ptr<const OsmData>>;
    struct Source { Parsed data; std::filesystem::file_time_type stamp; };
    std::map<std::string, Source> sources;
    std::mutex parsing;

    explicit State(Options o)
        : options(std::move(o)), store(options.gameRoot, [this](const std::string& line) { say(line); }) {}

    // The first worker to want a file parses it; the others wait for that
    // parse rather than starting their own, and other files go on meanwhile.
    std::shared_ptr<const OsmData> source(const std::string& path) {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(path, ec);
        std::promise<std::shared_ptr<const OsmData>> promise;
        Parsed parsed;
        bool mine = false;
        {
            std::lock_guard<std::mutex> guard(parsing);
            auto it = sources.find(path);
            if (it != sources.end() && it->second.stamp == stamp) parsed = it->second.data;
            else {
                if (sources.size() >= 8) sources.erase(sources.begin());
                parsed = promise.get_future().share();
                sources[path] = {parsed, stamp};
                mine = true;
            }
        }
        if (mine) {
            try {
                promise.set_value(std::make_shared<const OsmData>(normalizeOsm(readJson(path))));
            } catch (...) {
                promise.set_exception(std::current_exception());
                std::lock_guard<std::mutex> guard(parsing);
                sources.erase(path);
            }
        }
        return parsed.get();
    }

    void say(const std::string& line) const { if (options.log) options.log(line); }

    bool online() const {
        std::lock_guard<std::mutex> guard(lock);
        return Clock::now() >= offlineUntil;
    }

    // Under `lock`: the most urgent tile nobody is cooking. A tile never
    // cooked comes before one cooked offline and waiting for its upgrade.
    std::optional<Tile> next(bool& upgrade) {
        const auto now = Clock::now();
        for (const Tile& t : wanted) {
            if (busy.count(t) || cooked.count(t)) continue;
            auto failed = failedUntil.find(t);
            if (failed != failedUntil.end() && now < failed->second) continue;
            upgrade = false;
            return t;
        }
        if (now < offlineUntil) return std::nullopt;
        for (const Tile& t : wanted) {
            auto it = cooked.find(t);
            if (it == cooked.end() || busy.count(t) || !it->second->cooked.manifest.value("offlineApproximation", false)) continue;
            auto failed = failedUntil.find(t);
            if (failed != failedUntil.end() && now < failed->second) continue;
            upgrade = true;
            return t;
        }
        return std::nullopt;
    }

    std::optional<ObservationStore::Shared> sharedFor(const Tile& tile) const {
        std::lock_guard<std::mutex> guard(lock);
        for (const auto& group : groups)
            if (std::find(group.begin(), group.end(), tile) != group.end()) return store.shared(group);
        return store.shared({tile});
    }

    // An Overpass query for `region`, written to `path`, on its own thread.
    std::shared_future<void> download(const Bounds& region, const std::string& path) {
        std::lock_guard<std::mutex> guard(downloading);
        auto it = downloads.find(path);
        if (it != downloads.end()) return it->second;
        auto promise = std::make_shared<std::promise<void>>();
        std::shared_future<void> done = promise->get_future().share();
        downloads[path] = done;
        std::thread([self = shared_from_this(), region, path, promise] {
            Counter c(self->fetching);
            self->overpass.acquire();
            try {
                self->store.fetchOsm(region, path);
                promise->set_value();
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            self->overpass.release();
            std::lock_guard<std::mutex> g(self->downloading);
            self->downloads.erase(path);
        }).detach();
        return done;
    }

    bool urgent(const Tile& tile) const {
        std::lock_guard<std::mutex> guard(lock);
        return !wanted.empty() && wanted.front() == tile;
    }

    // The tile's observations: from disk, else the network, else nothing.
    // Throws SourceUnavailable when it needs the network and has none.
    //
    // A first visit waits on the network, so the waits overlap: the
    // neighbourhood's query starts, the tile the player stands on asks for
    // itself in the second slot (one tile answers in seconds where nine take
    // many), and meanwhile this worker fetches the tile's terrain.
    Observations observe(const Tile& tile) {
        Observations in;
        in.tile = tile;
        const auto shared = sharedFor(tile);
        bool stale = false;
        auto document = store.osmPath(tile, shared, &stale);
        auto ground = store.ground(tile);
        if ((!document || stale) && online()) {
            std::optional<std::shared_future<void>> region, own;
            if (shared) region = download(shared->region, shared->path);
            if (!shared || urgent(tile)) own = download(tile.bounds(), store.tileFolder(tile) + "/osm.json");
            if (!ground) {
                Counter c(fetching);
                ground = store.fetchGround(tile);
            }
            for (auto* wait : {&own, &region}) {
                if (!*wait) continue;
                try {
                    (*wait)->get();
                    document = store.osmPath(tile, shared, &stale);
                    if (document && !stale) break;
                } catch (const std::exception& e) {
                    if (wait == &region) throw;  // the last source left says why
                    say(std::string("OSM-TILE-QUERY-FAILED ") + tile.key() + " " + e.what());
                }
            }
        } else if (!ground && online()) {
            Counter c(fetching);
            ground = store.fetchGround(tile);
        }
        if (!document || !ground) throw SourceUnavailable("the network is not answering and nothing is cached");
        in.osm = source(*document);
        in.elevations = ground->first;
        in.elevationSource = ground->second;
        return in;
    }

    void cook(const Tile& tile, bool upgrade) {
        Counter c(cooking);
        const auto started = Clock::now();
        std::shared_ptr<ServedTile> served = std::make_shared<ServedTile>();
        try {
            Observations in;
            try {
                in = observe(tile);
            } catch (const SourceUnavailable& e) {
                {
                    std::lock_guard<std::mutex> guard(lock);
                    offlineUntil = Clock::now() + kOfflinePause;
                }
                say("OFFLINE " + tile.key() + " " + e.what());
                if (upgrade) return;  // keep the approximate tile it has
                // Playable rather than missing: Natural Earth's coast, flat ground.
                in = Observations{};
                in.tile = tile;
                auto empty = std::make_shared<OsmData>();
                empty->queryVersion = 0;
                in.osm = std::move(empty);
                in.elevations = ElevationGrid{tile.bounds(), 2, {0.0, 0.0, 0.0, 0.0}};
                in.elevationSource = "flat offline approximation";
                in.offline = true;
            }
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
        served->serial = ++serial;
        const bool approximate = served->cooked.manifest.value("offlineApproximation", false);
        say(std::string(upgrade ? "UPGRADED " : approximate ? "OFFLINE-READY " : "READY ") + tile.key() + " cook_ms=" +
            std::to_string(int(served->cooked.cookMs)) + " total_ms=" +
            std::to_string(int(std::chrono::duration<double, std::milli>(Clock::now() - started).count())));
        if (error.rfind(tile.key(), 0) == 0) error.clear();
        cooked[tile] = std::move(served);
        failedUntil.erase(tile);
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
