#include "sea.hpp"

#include "harbours.hpp"
#include "inflate.hpp"
#include "net.hpp"
#include "palette.hpp"
#include "sources.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace r1 {

namespace {
using Clock = std::chrono::steady_clock;

double now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// A calendar date from days since 1970-01-01 (proleptic Gregorian, UTC).
struct Civil { int year, month, day, yday, weekday; };  // weekday: Monday 0, as Python's
Civil civil(int64_t days) {
    const int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const int day = int(doy - (153 * mp + 2) / 5 + 1), month = int(mp < 10 ? mp + 3 : mp - 9);
    const int year = int(int64_t(yoe) + era * 400 + (month <= 2));
    static const int before[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int yday = before[month - 1] + day + (leap && month > 2);
    return {year, month, day, yday, int(((days % 7) + 7 + 3) % 7)};
}
int64_t daysOf(int year, int month, int day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = unsigned(year - era * 400);
    const unsigned doy = (153 * unsigned(month + (month > 2 ? -3 : 9)) + 2) / 5 + unsigned(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}
std::string isoDate(int64_t days) {
    const Civil c = civil(days);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", c.year, c.month, c.day);
    return buf;
}

void writeAtomically(const std::string& path, const std::string& text) {
    fs::create_directories(fs::path(path).parent_path());
    const std::string tmp = path + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); f << text; }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(path, ec); fs::rename(tmp, path, ec); }
}

// ── the prior ───────────────────────────────────────────────────────────────
const char* kKinds[] = {"commercial", "fishing", "passenger", "leisure"};
constexpr int kKindCount = 4;
const double kSpeed[] = {13.0, 6.0, 16.0, 6.0};   // cruising knots
const double kMoving[] = {0.75, 0.6, 0.8, 0.5};    // share under way
const std::vector<std::pair<const char*, double>> kHulls[] = {
    {{"container_a", 3}, {"container_b", 3}, {"cargo_a", 2}, {"cargo_b", 2}},
    {{"fishing", 1}},
    {{"ferry", 1}},
    {{"sail_a", 3}, {"sail_b", 3}, {"speed_a", 1}, {"speed_c", 1}, {"speed_e", 1}}};
constexpr double kRadius = 6000.0, kSlot = 1800.0, kShrink = 2.0, kLiveWindow = 3 * 3600.0, kKnot = 0.514444;
constexpr size_t kMaxShips = 40;

// The shipped base, read one block at a time (four kept).
class Prior {
public:
    explicit Prior(const std::string& path) : path_(path) {
        std::ifstream f(path, std::ios::binary);
        char magic[8];
        f.read(magic, 8);
        if (!f || std::string(magic, 5) != "R1SEA") throw std::runtime_error(path + ": not a sea prior");
        uint32_t n = 0;
        f.read(reinterpret_cast<char*>(&n), 4);
        std::string meta(n, ' ');
        f.read(meta.data(), n);
        const auto m = nlohmann::json::parse(meta);
        lon0 = m.at("lon0"); lat0 = m.at("lat0"); cell = m.at("cell");
        rows = m.at("rows"); cols = m.at("cols"); block = m.at("block");
        floor_ = m.at("floor"); perOctave = m.at("perOctave"); noAxis = m.at("noAxis");
        by = (rows + block - 1) / block; bx = (cols + block - 1) / block;
        index_.resize(size_t(by * bx));
        f.read(reinterpret_cast<char*>(index_.data()), std::streamsize(index_.size() * 8));
        dataAt_ = f.tellg();
    }
    double lon0, lat0, cell, floor_, perOctave;
    int rows, cols, block, noAxis, by, bx;

    bool cellOf(double lon, double lat, int& row, int& col) const {
        const double r = (lat0 - lat) / cell, c = pymod(lon - lon0, 360.0) / cell;
        row = int(r); col = int(c);
        return r > -1 && 0 <= row && row < rows && 0 <= col && col < cols;
    }
    P2 centre(int row, int col) const { return {lon0 + (col + 0.5) * cell, lat0 - (row + 0.5) * cell}; }

    // (mean ships per kind, lane bearing or NaN) of a cell.
    void at(int row, int col, double means[kKindCount], double& axis) {
        const std::vector<uint8_t>* blob = blockOf((row / block) * bx + col / block);
        axis = std::nan("");
        if (!blob) { for (int k = 0; k < kKindCount; ++k) means[k] = 0; return; }
        const size_t kk = size_t((row % block) * block + col % block), plane = size_t(block * block);
        for (int k = 0; k < kKindCount; ++k) {
            const int q = (*blob)[size_t(k) * plane + kk];
            means[k] = q == 0 ? 0.0 : floor_ * std::pow(2.0, (q - 1) / perOctave);
        }
        const int a = (*blob)[size_t(kKindCount) * plane + kk];
        if (a != noAxis) axis = a * 180.0 / 255.0;
    }

private:
    std::string path_;
    std::vector<std::array<uint32_t, 2>> index_;
    std::streamoff dataAt_ = 0;
    std::vector<std::pair<int, std::unique_ptr<std::vector<uint8_t>>>> blocks_;

    const std::vector<uint8_t>* blockOf(int i) {
        for (auto& [id, blob] : blocks_) if (id == i) return blob.get();
        const auto [offset, length] = index_[size_t(i)];
        std::unique_ptr<std::vector<uint8_t>> blob;
        if (length) {
            std::ifstream f(path_, std::ios::binary);
            f.seekg(dataAt_ + std::streamoff(offset));
            std::vector<uint8_t> packed(length);
            f.read(reinterpret_cast<char*>(packed.data()), length);
            blob = std::make_unique<std::vector<uint8_t>>(inflateZlib(packed.data(), packed.size()));
        }
        if (blocks_.size() >= 4) blocks_.erase(blocks_.begin());
        blocks_.push_back({i, std::move(blob)});
        return blocks_.back().second.get();
    }
};

// ── the day ─────────────────────────────────────────────────────────────────
double dayFactor(int kind, double lon, double lat, double gameUnix) {
    const int64_t days = int64_t(std::floor(gameUnix / 86400.0));
    const Civil c = civil(days);
    const double seconds = gameUnix - days * 86400.0;
    const double hourUtc = std::floor(seconds / 3600.0), minute = std::floor(std::fmod(seconds, 3600.0) / 60.0);
    const int peak = lat >= 0 ? 196 : 15;  // mid-July / mid-January
    double amplitude = kind == 3 ? 0.8 : kind == 2 ? 0.3 : kind == 1 ? 0.1 : 0.0;
    if (std::abs(lat) < 23.0) amplitude *= 0.25;
    const double season = 1.0 + amplitude * std::cos(2 * kPi * (c.yday - peak) / 365.25);
    const double hour = pymod(hourUtc + minute / 60.0 + lon / 15.0, 24.0);  // local solar
    const bool day = 7.0 <= hour && hour < 20.0;
    const double diurnal = kind == 3 ? (day ? 1.7 : 0.25) : kind == 2 ? (day ? 1.25 : 0.6) : 1.0;
    const double weekday = kind == 3 ? (c.weekday >= 5 ? 1.35 : 0.86) : 1.0;
    return season * diurnal * weekday;
}

double seaEase(int kind, double hs) {
    if (kind == 3) return std::exp(-std::pow(hs / 1.5, 2));
    if (kind == 1) return 1.0 / (1.0 + std::pow(hs / 3.0, 4));
    if (kind == 2) return 1.0 / (1.0 + std::pow(hs / 5.0, 6));
    return 1.0 / (1.0 + std::pow(hs / 10.0, 8));
}

// Relative to an ordinary 1 m sea: a gale empties the marinas, not the lanes.
double weatherFactor(int kind, const std::optional<double>& hs, const std::optional<double>& wind) {
    if (!hs) return 1.0;
    double f = seaEase(kind, *hs) / seaEase(kind, 1.0);
    if (kind == 3 && wind && *wind > 10.0) f *= std::exp(-(*wind - 10.0) / 4.0);
    return std::min(f, 1.5);
}

int square(double lon, double lat) {
    return int(std::floor(lat) + 90) * 360 + int(pymod(std::floor(lon) + 180, 360));
}

// ── what was learned ────────────────────────────────────────────────────────
// The local base the online sync grows: one entry per (cell, kind) ever
// observed, one per (1-degree square, day) of weather, and the ships seen in
// the last week. Small by construction, and a JSON file.
struct Learned {
    std::string path;
    nlohmann::json doc = {{"cells", nlohmann::json::object()}, {"weather", nlohmann::json::object()},
                          {"vessels", nlohmann::json::object()}, {"meta", nlohmann::json::object()}};
    mutable std::mutex lock;

    explicit Learned(std::string p) : path(std::move(p)) {
        try {
            std::ifstream f(path, std::ios::binary);
            if (f) {
                auto loaded = nlohmann::json::parse(f);
                for (const char* k : {"cells", "weather", "vessels", "meta"})
                    if (loaded.contains(k)) doc[k] = loaded[k];
            }
        } catch (const std::exception&) {}  // a torn file is a fresh base, said by the sync log
    }
    void save() const {
        std::lock_guard<std::mutex> g(lock);
        writeAtomically(path, doc.dump());
    }
    std::optional<std::array<double, 7>> cell(int id, int kind) const {
        std::lock_guard<std::mutex> g(lock);
        auto it = doc["cells"].find(std::to_string(id) + ":" + std::to_string(kind));
        if (it == doc["cells"].end()) return std::nullopt;
        std::array<double, 7> out{};
        for (size_t i = 0; i < 7; ++i) out[i] = (*it)[i].get<double>();
        return out;
    }
    // (hs, wind, source) for a day: that day, else the month's mean here.
    void weather(double lon, double lat, int64_t day, std::optional<double>& hs, std::optional<double>& wind,
                 std::string& source) const {
        std::lock_guard<std::mutex> g(lock);
        const std::string sq = std::to_string(square(lon, lat)) + ":";
        const auto& w = doc["weather"];
        auto it = w.find(sq + std::to_string(day));
        auto read = [](const nlohmann::json& v) { return v.is_number() ? std::optional<double>(v.get<double>()) : std::nullopt; };
        if (it != w.end()) { hs = read((*it)[0]); wind = read((*it)[1]); source = "jour"; return; }
        const int month = civil(day).month;
        double sumH = 0, sumW = 0;
        int n = 0, nw = 0;
        for (auto e = w.begin(); e != w.end(); ++e) {
            if (e.key().rfind(sq, 0) != 0) continue;
            if (civil(std::stoll(e.key().substr(sq.size()))).month != month) continue;
            sumH += e.value()[0].get<double>(); ++n;
            if (e.value()[1].is_number()) { sumW += e.value()[1].get<double>(); ++nw; }
        }
        if (n) { hs = sumH / n; if (nw) wind = sumW / nw; source = "climatologie"; return; }
        source = "neutre";
    }
};

// A Poisson draw; a normal one past thirty, as Python did.
int poisson(double lam, PyRandom& rng) {
    if (lam <= 0) return 0;
    if (lam > 30) return std::max(0, int(pyround(rng.gauss(lam, std::sqrt(lam)))));
    const double limit = std::exp(-lam);
    int k = 0;
    double p = 1.0;
    for (;;) {
        p *= rng.random();
        if (p <= limit) return k;
        ++k;
    }
}

P2 offset(double lon, double lat, double east, double north) {
    return {lon + east / (kMetresPerDegree * std::max(0.05, std::cos(radians(lat)))), lat + north / kMetresPerDegree};
}

uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

struct Ship { std::string id; int kind; double lon, lat, heading, speed; std::optional<double> length; uint64_t seed; };
}  // namespace

// ── the sea ─────────────────────────────────────────────────────────────────

struct SeaService::State {
    std::string game;
    std::function<void(const std::string&)> log;
    std::unique_ptr<Prior> prior;
    Learned learned;
    mutable std::mutex lock;
    std::condition_variable wake;
    std::optional<std::array<double, 3>> where;
    std::shared_ptr<const nlohmann::json> doc;
    std::string line;
    bool online = false, stopping = false;
    std::string synced;

    State(std::string root, std::function<void(const std::string&)> l)
        : game(std::move(root)), log(std::move(l)), learned(game + "/cache/shipping/learned.json") {
        synced = learned.doc["meta"].value("synced", std::string());
    }

    void say(const std::string& text) {
        {
            std::lock_guard<std::mutex> g(lock);
            line = text;
        }
        if (log) log(text);
    }

    // The ships within the radius, deterministic for a (cell, kind, half-hour).
    nlohmann::json plan(double lon, double lat, double gameUnix, double realUnix) {
        const int64_t day = int64_t(std::floor(gameUnix / 86400.0));
        std::optional<double> hs, wind;
        std::string weatherSource;
        learned.weather(lon, lat, day, hs, wind, weatherSource);
        const int64_t slot = int64_t(std::floor(realUnix / kSlot));
        const double since = realUnix - slot * kSlot;
        const double spanLat = kRadius / kMetresPerDegree + prior->cell;
        const double spanLon = kRadius / (kMetresPerDegree * std::max(0.05, std::cos(radians(lat)))) + prior->cell;
        nlohmann::json source = {{"weather", weatherSource}, {"hs", hs ? nlohmann::json(*hs) : nlohmann::json()},
                                 {"wind", wind ? nlohmann::json(*wind) : nlohmann::json()}, {"live", 0}};
        int r0, c0;
        if (!prior->cellOf(lon, lat, r0, c0)) return {{"ships", nlohmann::json::array()}, {"source", source}};
        const int dr = int(spanLat / prior->cell) + 1, dc = int(spanLon / prior->cell) + 1;
        // Ships seen live, dead-reckoned to the game's instant when it is close
        // enough to when they were seen. They stand in for sampled ones.
        std::vector<Ship> ships, live;
        std::map<std::tuple<int, int, int>, int> liveCount;
        if (std::abs(gameUnix - realUnix) < kLiveWindow) {
            std::lock_guard<std::mutex> g(learned.lock);
            const double span = std::max(spanLat, spanLon);
            for (auto v = learned.doc["vessels"].begin(); v != learned.doc["vessels"].end(); ++v) {
                const auto& r = v.value();  // [kind, lon, lat, knots, cog, t, length]
                const double vlon = r[1], vlat = r[2], knots = r[3], cog = r[4], t = r[5];
                if (vlon < lon - span || vlon > lon + span || vlat < lat - span || vlat > lat + span || t < gameUnix - kLiveWindow)
                    continue;
                const double dt = gameUnix - t, speed = knots > 0.8 ? knots * kKnot : 0.0;
                const P2 p = offset(vlon, vlat, std::sin(radians(cog)) * speed * dt, std::cos(radians(cog)) * speed * dt);
                int row, col;
                if (!prior->cellOf(p.x, p.y, row, col)) continue;
                ++liveCount[{row, col, r[0].get<int>()}];
                live.push_back({"ais-" + v.key(), r[0].get<int>(), p.x, p.y, cog, speed,
                                r[6].is_number() ? std::optional<double>(r[6].get<double>()) : std::nullopt,
                                std::stoull(v.key())});
            }
        }
        for (int row = r0 - dr; row <= r0 + dr; ++row) {
            if (row < 0 || row >= prior->rows) continue;
            for (int c = c0 - dc; c <= c0 + dc; ++c) {
                const int col = int(pymod(c, prior->cols));
                double means[kKindCount], axis;
                prior->at(row, col, means, axis);
                if (!means[0] && !means[1] && !means[2] && !means[3]) continue;
                const P2 centre = prior->centre(row, col);
                const int cellId = row * prior->cols + col;
                for (int k = 0; k < kKindCount; ++k) {
                    if (!means[k]) continue;
                    double lam = means[k] * dayFactor(k, centre.x, centre.y, gameUnix) * weatherFactor(k, hs, wind);
                    const auto seen = learned.cell(cellId, k);
                    double knots = kSpeed[k], movingShare = kMoving[k];
                    if (seen) {
                        const auto& s = *seen;  // obs, pred, vx, vy, moving, still, knots
                        lam *= (s[0] + kShrink) / (s[1] + kShrink);
                        if (s[4] + s[5] >= 3) movingShare = (s[4] + kMoving[k]) / (s[4] + s[5] + 1);
                        if (s[4] >= 3) knots = s[6] / s[4];
                    }
                    PyRandom rng(__int128(mix(mix(uint64_t(cellId)) ^ mix(uint64_t(k) << 40) ^ uint64_t(slot)) >> 1));
                    auto lc = liveCount.find({row, col, k});
                    const int count = poisson(lam, rng) - (lc == liveCount.end() ? 0 : lc->second);
                    for (int n = 0; n < count; ++n) {
                        const uint64_t seed = uint64_t(rng.random() * 9007199254740992.0);
                        PyRandom s((__int128)seed);
                        double plon = centre.x + (s.random() - 0.5) * prior->cell;
                        double plat = centre.y + (s.random() - 0.5) * prior->cell;
                        const bool underWay = s.random() < movingShare;
                        double heading = std::isnan(axis) ? s.random() * 360.0
                                                          : pymod(axis + (s.random() < 0.5 ? 180.0 : 0.0) + s.gauss(0, 6), 360.0);
                        if (seen && (*seen)[4] >= 3) {
                            // The lane's real direction, learned: a ship heading
                            // against it turns round, as often as the flow is one-way.
                            const double flow = pymod(degrees(std::atan2((*seen)[2], (*seen)[3])), 360.0);
                            const double coherence = std::hypot((*seen)[2], (*seen)[3]) / (*seen)[4];
                            if (std::abs(pymod(heading - flow + 180.0, 360.0) - 180.0) > 90.0 && s.random() < coherence)
                                heading = pymod(heading + 180.0, 360.0);
                        }
                        const double speed = underWay ? knots * kKnot * (0.8 + 0.4 * s.random()) : 0.0;
                        if (speed) {
                            const P2 p = offset(plon, plat, std::sin(radians(heading)) * speed * since,
                                                std::cos(radians(heading)) * speed * since);
                            plon = p.x; plat = p.y;
                        }
                        ships.push_back({std::to_string(cellId) + "-" + std::to_string(k) + "-" + std::to_string(slot) + "-" +
                                             std::to_string(n),
                                         k, plon, plat, heading, speed, std::nullopt, seed});
                    }
                }
            }
        }
        ships.insert(ships.end(), live.begin(), live.end());
        source["live"] = live.size();
        struct Out { nlohmann::json j; bool real; double distance; };
        std::vector<Out> out;
        for (const Ship& ship : ships) {
            const double k = std::cos(radians((lat + ship.lat) / 2));
            const double east = (ship.lon - lon) * kMetresPerDegree * k, north = (ship.lat - lat) * kMetresPerDegree;
            const double distance = std::hypot(east, north);
            if (distance > kRadius) continue;
            PyRandom s((__int128)(ship.seed ^ 0x5eaull));
            const auto& options = kHulls[ship.kind];
            double total = 0;
            for (const auto& o : options) total += o.second;
            double r = s.random() * total;
            const char* hull = options.back().first;
            for (const auto& o : options) { r -= o.second; if (r <= 0) { hull = o.first; break; } }
            out.push_back({{{"id", ship.id}, {"kind", kKinds[ship.kind]}, {"model", hull}, {"lon", pyround(ship.lon, 7)},
                            {"lat", pyround(ship.lat, 7)}, {"heading", pyround(ship.heading, 2)},
                            {"speed", pyround(ship.speed, 2)}, {"distance", pyround(distance, 1)}},
                           ship.id.rfind("ais-", 0) == 0, distance});
        }
        // Ships really seen first: a prediction never crowds out a real one.
        std::stable_sort(out.begin(), out.end(), [](const Out& a, const Out& b) {
            if (a.real != b.real) return a.real;
            return a.distance < b.distance;
        });
        nlohmann::json list = nlohmann::json::array();
        for (size_t i = 0; i < out.size() && i < kMaxShips; ++i) list.push_back(std::move(out[i].j));
        return {{"ships", list}, {"source", source}};
    }

    // One scene-node document per hull, which the game clones for every ship.
    void hulls(nlohmann::json& doc) {
        nlohmann::json docs = nlohmann::json::object();
        std::map<std::string, nlohmann::json> specs;
        const Anchor anchor = Anchor::at(0.0, 0.0, 0.0);
        for (const auto& s : doc["ships"]) {
            const std::string name = s["model"];
            if (docs.contains(name)) continue;
            const BoatKind* kind = nullptr;
            for (const BoatKind& b : palette().boats) if (b.name == name) kind = &b;
            if (!kind) continue;
            const double beam = beamOf(*kind);
            auto [nodes, manifest] = boatNodes({Berth{kind, 0.0, 0.0, {0.0, -1.0}, beam}}, anchor);
            nlohmann::json node = nodes[0];
            node["name"] = "Ship " + name;
            node["transform"] = {{"position", {0.0, 0.0, 0.0}}, {"rotation", {0.0, 0.0, 0.0, 1.0}}, {"scale", {1.0, 1.0, 1.0}}};
            docs[name] = node;
            specs[name] = {{"length", manifest[0]["length"]}, {"beam", manifest[0]["beam"]}, {"top", manifest[0]["top"]},
                           {"accel", manifest[0]["accel"]}, {"turn", manifest[0]["turn"]}};
        }
        for (auto& s : doc["ships"])
            if (specs.count(s["model"])) s.update(specs[s["model"].get<std::string>()]);
        doc["hulls"] = docs;
    }

    // The sea state and wind around the game's date, stored per day: the
    // forecast within its horizon, else the same days a year earlier.
    int fetchWeather(double lon, double lat, int64_t gameDay) {
        const int64_t today = int64_t(std::floor(now() / 86400));
        const int64_t target = gameDay <= today + 14 ? gameDay : gameDay - 365;
        char coords[96];
        std::snprintf(coords, sizeof coords, "latitude=%.2f&longitude=%.2f", lat, lon);
        const std::string q = std::string(coords) + "&timezone=GMT&start_date=" + isoDate(target - 3) +
                              "&end_date=" + isoDate(std::min(target + 3, today + 14));
        const auto marine = nlohmann::json::parse(net::requestJson("https://marine-api.open-meteo.com/v1/marine?" + q +
                                                                   "&daily=wave_height_max", {}, {}, 15.0));
        std::map<std::string, nlohmann::json> winds;
        try {
            const auto w = nlohmann::json::parse(net::requestJson("https://api.open-meteo.com/v1/forecast?" + q +
                                                                  "&daily=wind_speed_10m_max&wind_speed_unit=ms", {}, {}, 15.0));
            for (size_t i = 0; i < w["daily"]["time"].size(); ++i) winds[w["daily"]["time"][i]] = w["daily"]["wind_speed_10m_max"][i];
        } catch (const std::exception&) {}
        const std::string sq = std::to_string(square(lon, lat)) + ":";
        int stored = 0;
        std::lock_guard<std::mutex> g(learned.lock);
        const auto& times = marine["daily"]["time"];
        const auto& heights = marine["daily"]["wave_height_max"];
        for (size_t i = 0; i < times.size(); ++i) {
            if (!heights[i].is_number()) continue;
            const std::string date = times[i];
            const int64_t d = daysOf(std::stoi(date.substr(0, 4)), std::stoi(date.substr(5, 2)), std::stoi(date.substr(8, 2)));
            learned.doc["weather"][sq + std::to_string(d)] = {heights[i], winds.count(date) ? winds[date] : nlohmann::json()};
            ++stored;
        }
        return stored;
    }

    static int aisKind(const nlohmann::json& type, bool classB, const std::optional<double>& length) {
        if (type.is_number()) {
            const int t = type;
            if (t == 30) return 1;
            if (t == 36 || t == 37) return 3;
            if (60 <= t && t <= 69) return 2;
            if ((70 <= t && t <= 89) || t == 31 || t == 32 || t == 52) return 0;
        }
        if (length && *length) return *length > 40 ? 0 : 3;
        return classB ? 3 : 0;
    }

    // Live positions around (lon, lat) for two minutes, folded into the base:
    // ships seen against ships predicted, per cell and kind.
    int listenAis(const std::string& key, double lon, double lat) {
        const double span = 0.3;
        struct Seen { double lon, lat, knots, cog, t; bool classB; };
        std::map<long long, Seen> seen;
        std::map<long long, std::pair<nlohmann::json, std::optional<double>>> statics;
        {
            net::WebSocket ws("wss://stream.aisstream.io/v0/stream", 10.0);
            ws.send(nlohmann::json({{"APIKey", key},
                                    {"BoundingBoxes", {{{lat - span, lon - span}, {lat + span, lon + span}}}},
                                    {"FilterMessageTypes", {"PositionReport", "StandardClassBPositionReport", "ShipStaticData"}}})
                        .dump());
            const auto end = Clock::now() + std::chrono::seconds(120);
            while (Clock::now() < end) {
                bool closed = false;
                std::string raw;
                try { raw = ws.receive(closed); } catch (const net::Unreachable&) { break; }  // a quiet sea times out
                if (closed) break;
                const auto msg = nlohmann::json::parse(raw, nullptr, false);
                if (msg.is_discarded()) continue;
                if (msg.contains("error")) throw std::runtime_error("aisstream: " + msg["error"].dump());
                const std::string kind = msg.value("MessageType", "");
                const auto meta = msg.value("MetaData", nlohmann::json::object());
                const long long mmsi = meta.value("MMSI", 0LL);
                if (!mmsi) continue;
                const auto body = msg.value("Message", nlohmann::json::object()).value(kind, nlohmann::json::object());
                if (kind == "ShipStaticData") {
                    const auto d = body.value("Dimension", nlohmann::json::object());
                    const double length = d.value("A", 0.0) + d.value("B", 0.0);
                    statics[mmsi] = {body.value("Type", nlohmann::json()), length ? std::optional<double>(length) : std::nullopt};
                    continue;
                }
                if (!body.value("Valid", true)) continue;
                if (!body.contains("Latitude") || !body.contains("Longitude")) continue;
                const double plat = body["Latitude"], plon = body["Longitude"];
                if (std::abs(plat) > 90 || std::abs(plon) > 180) continue;
                seen[mmsi] = {plon, plat, std::min(body.value("Sog", 0.0), 40.0), pymod(body.value("Cog", 0.0), 360.0), now(),
                              kind == "StandardClassBPositionReport"};
            }
        }
        std::lock_guard<std::mutex> g(learned.lock);
        std::map<std::pair<int, int>, std::array<double, 6>> counts;
        for (const auto& [mmsi, r] : seen) {
            auto st = statics.find(mmsi);
            const std::optional<double> length = st == statics.end() ? std::nullopt : st->second.second;
            const int kind = aisKind(st == statics.end() ? nlohmann::json() : st->second.first, r.classB, length);
            int row, col;
            if (prior->cellOf(r.lon, r.lat, row, col)) {
                auto& c = counts[{row * prior->cols + col, kind}];
                c[0] += 1;
                if (r.knots > 0.8) { c[1] += std::sin(radians(r.cog)); c[2] += std::cos(radians(r.cog)); c[3] += 1; c[5] += r.knots; }
                else c[4] += 1;
            }
            learned.doc["vessels"][std::to_string(mmsi)] = {kind, r.lon, r.lat, r.knots, r.cog, r.t,
                                                            length ? nlohmann::json(*length) : nlohmann::json()};
        }
        // Every cell fully inside the window was predicted, seen or not.
        int r0 = 0, c0 = 0, r1 = 0, c1 = 0;
        if (!prior->cellOf(lon - span, lat + span, r0, c0)) r0 = c0 = 0;
        if (!prior->cellOf(lon + span, lat - span, r1, c1)) r1 = c1 = 0;
        const double t = now();
        for (int row = r0 + 1; row < r1; ++row)
            for (int col = c0 + 1; col < c1; ++col) {
                double means[kKindCount], axis;
                prior->at(row, col, means, axis);
                const P2 centre = prior->centre(row, col);
                for (int k = 0; k < kKindCount; ++k) {
                    const double pred = means[k] * dayFactor(k, centre.x, centre.y, t);
                    const auto it = counts.find({row * prior->cols + col, k});
                    const std::array<double, 6> obs = it == counts.end() ? std::array<double, 6>{} : it->second;
                    if (!pred && !obs[0]) continue;
                    auto& cell = learned.doc["cells"][std::to_string(row * prior->cols + col) + ":" + std::to_string(k)];
                    if (!cell.is_array()) cell = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
                    const double add[7] = {obs[0], pred, obs[1], obs[2], obs[3], obs[4], obs[5]};
                    for (size_t i = 0; i < 7; ++i) cell[i] = cell[i].get<double>() + add[i];
                }
            }
        for (auto v = learned.doc["vessels"].begin(); v != learned.doc["vessels"].end();) {
            if ((*v)[5].get<double>() < t - 7 * 86400) v = learned.doc["vessels"].erase(v);
            else ++v;
        }
        return int(seen.size());
    }

    static void planLoop(std::shared_ptr<State> self) {
        for (;;) {
            std::array<double, 3> at;
            {
                std::unique_lock<std::mutex> g(self->lock);
                self->wake.wait_for(g, std::chrono::seconds(5), [&] { return self->stopping; });
                if (self->stopping) return;
                if (!self->where) continue;
                at = *self->where;
            }
            try {
                nlohmann::json doc = self->plan(at[0], at[1], at[2], now());
                self->hulls(doc);
                doc["at"] = now(); doc["lon"] = at[0]; doc["lat"] = at[1]; doc["gameTime"] = at[2];
                std::lock_guard<std::mutex> g(self->lock);
                doc["source"]["online"] = self->online;
                doc["source"]["synced"] = self->synced.empty() ? nlohmann::json() : nlohmann::json(self->synced);
                self->doc = std::make_shared<const nlohmann::json>(std::move(doc));
            } catch (const std::exception& e) {
                self->say(std::string("planning failed: ") + e.what());
            }
        }
    }

    static void syncLoop(std::shared_ptr<State> self) {
        bool saidOffline = false, saidNoKey = false;
        for (;;) {
            std::array<double, 3> at;
            {
                std::unique_lock<std::mutex> g(self->lock);
                self->wake.wait_for(g, std::chrono::seconds(5), [&] { return self->stopping; });
                if (self->stopping) return;
                if (!self->where) continue;
                at = *self->where;
            }
            auto pause = std::chrono::seconds(900);
            try {
                const int days = self->fetchWeather(at[0], at[1], int64_t(std::floor(at[2] / 86400)));
                std::string key;
                if (const char* env = std::getenv("R1WORLD_AISSTREAM_KEY")) key = env;
                if (key.empty()) {
                    std::ifstream f(self->game + "/cache/shipping/aisstream.key");
                    std::getline(f, key);
                    while (!key.empty() && std::isspace((unsigned char)key.back())) key.pop_back();
                }
                int ships = 0;
                if (!key.empty()) ships = self->listenAis(key, at[0], at[1]);
                else if (!saidNoKey) {
                    self->say("no aisstream.io key (cache/shipping/aisstream.key or R1WORLD_AISSTREAM_KEY): weather only, no live ships");
                    saidNoKey = true;
                }
                char stamp[32];
                const std::time_t t = std::time(nullptr);
                std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S+00:00", std::gmtime(&t));
                {
                    std::lock_guard<std::mutex> g(self->learned.lock);
                    self->learned.doc["meta"]["synced"] = stamp;
                }
                self->learned.save();
                {
                    std::lock_guard<std::mutex> g(self->lock);
                    self->online = true;
                    self->synced = stamp;
                }
                char text[160];
                std::snprintf(text, sizeof text, "synced: %d days of weather, %d live ships near %.3f, %.3f", days, ships, at[0], at[1]);
                self->say(text);
                saidOffline = false;
            } catch (const std::exception& e) {
                {
                    std::lock_guard<std::mutex> g(self->lock);
                    self->online = false;
                }
                if (!saidOffline)
                    self->say(std::string("offline (") + e.what() + ") - predicting from the local base, last synced " +
                              (self->synced.empty() ? "never" : self->synced));
                saidOffline = true;
                pause = std::chrono::seconds(600);
            }
            std::unique_lock<std::mutex> g(self->lock);
            self->wake.wait_for(g, pause, [&] { return self->stopping; });
            if (self->stopping) return;
        }
    }
};

SeaService::SeaService(std::string gameRoot, std::function<void(const std::string&)> log)
    : state_(std::make_shared<State>(std::move(gameRoot), std::move(log))) {
    const std::string path = state_->game + "/assets/world/shipping/prior.bin";
    try {
        state_->prior = std::make_unique<Prior>(path);
    } catch (const std::exception& e) {
        state_->say(std::string("no sea prior (") + e.what() + ") - no ships at sea");
        return;
    }
    std::thread(State::planLoop, state_).detach();
    std::thread(State::syncLoop, state_).detach();
}

SeaService::~SeaService() {
    {
        std::lock_guard<std::mutex> g(state_->lock);
        state_->stopping = true;
    }
    state_->wake.notify_all();
}

void SeaService::ask(double lon, double lat, double gameUnix) {
    std::lock_guard<std::mutex> g(state_->lock);
    state_->where = {{lon, lat, gameUnix}};
}

std::shared_ptr<const nlohmann::json> SeaService::latest() const {
    std::lock_guard<std::mutex> g(state_->lock);
    return state_->doc;
}

std::string SeaService::lastLine() const {
    std::lock_guard<std::mutex> g(state_->lock);
    return state_->line;
}

// ── the sky ─────────────────────────────────────────────────────────────────

struct ConditionsService::State {
    std::string game;
    std::function<void(const std::string&)> log;
    mutable std::mutex lock;
    std::condition_variable wake;
    std::optional<P2> where;
    std::shared_ptr<const nlohmann::json> doc;
    bool stopping = false;

    static constexpr double kRefresh = 15 * 60, kCurrent = 2 * 60 * 60;

    std::string cachePath(P2 cell) const {
        char name[64];
        std::snprintf(name, sizeof name, "%+.1f_%+.1f.json", cell.y, cell.x);
        return game + "/cache/conditions/" + name;
    }

    static nlohmann::json display(const nlohmann::json& saved, double lon, double lat) {
        const double age = saved.is_object() ? now() - saved.value("fetchedAt", 0.0) : 1e300;
        const bool fresh = saved.is_object() && 0 <= age && age < kCurrent;
        const bool zone = saved.is_object() && 0 <= age && age < 24 * 3600;
        return {{"lon", lon}, {"lat", lat}, {"timezone", zone ? saved["timezone"] : nlohmann::json("")},
                {"utcOffsetSeconds", zone ? saved["utcOffsetSeconds"] : nlohmann::json(pyround(lon / 15) * 3600)},
                {"timeSource", zone ? "zone" : "longitude-approximation"},
                {"weather", fresh ? saved["weather"] : nlohmann::json()},
                {"weatherSource", fresh ? "forecast" : "unavailable"}};
    }

    nlohmann::json forecast(P2 cell) {
        char q[160];
        std::snprintf(q, sizeof q, "latitude=%.3f&longitude=%.3f", cell.y, cell.x);
        const auto data = nlohmann::json::parse(net::requestJson(std::string("https://api.open-meteo.com/v1/forecast?") + q +
            "&current=temperature_2m,cloud_cover,precipitation,rain,showers,weather_code,visibility,wind_speed_10m,"
            "wind_direction_10m,snowfall,snow_depth&wind_speed_unit=ms&timezone=auto", {}, {}, 5.0, 1));
        const auto& current = data.at("current");
        const int offset = data.at("utc_offset_seconds");
        const double cloud = current.at("cloud_cover");
        if (!(-43200 <= offset && offset <= 50400 && 0 <= cloud && cloud <= 100)) throw std::runtime_error("Invalid local conditions");
        return {{"lon", cell.x}, {"lat", cell.y}, {"fetchedAt", now()}, {"timezone", data.at("timezone")},
                {"utcOffsetSeconds", offset},
                {"weather", {{"temperature", current.at("temperature_2m")}, {"cloudCover", cloud},
                             {"precipitation", current.at("precipitation")}, {"code", current.at("weather_code")},
                             // Raw current-period millimetres. The game converts the interval to mm/h.
                             {"precipitationIntervalSeconds", current.value("interval", 900)},
                             {"rain", current.value("rain", nlohmann::json())},
                             {"showers", current.value("showers", nlohmann::json())},
                             // Metres, m/s, degrees the wind comes from, cm an hour, metres.
                             {"visibility", current.value("visibility", nlohmann::json())},
                             {"windSpeed", current.value("wind_speed_10m", nlohmann::json())},
                             {"windFrom", current.value("wind_direction_10m", nlohmann::json())},
                             {"snowfall", current.value("snowfall", nlohmann::json())},
                             {"snowDepth", current.value("snow_depth", nlohmann::json())}}}};
    }

    void publish(nlohmann::json value) {
        std::lock_guard<std::mutex> g(lock);
        doc = std::make_shared<const nlohmann::json>(std::move(value));
    }

    static void run(std::shared_ptr<State> self) {
        std::optional<P2> current;
        std::optional<bool> visibleWeather, visibleZone;
        auto nextAttempt = Clock::now();
        for (;;) {
            P2 at;
            {
                std::unique_lock<std::mutex> g(self->lock);
                self->wake.wait_for(g, std::chrono::seconds(2), [&] { return self->stopping; });
                if (self->stopping) return;
                if (!self->where) continue;
                at = *self->where;
            }
            const P2 cell{pyround(at.x, 1), pyround(at.y, 1)};
            if (!current || !(*current == cell)) {
                current = cell;
                visibleWeather.reset(); visibleZone.reset();
                nextAttempt = Clock::now();
            }
            nlohmann::json saved;
            try {
                std::ifstream f(self->cachePath(cell), std::ios::binary);
                if (f) saved = nlohmann::json::parse(f);
            } catch (const std::exception&) { saved = nlohmann::json(); }
            const nlohmann::json shown = display(saved, at.x, at.y);
            const bool hasWeather = !shown["weather"].is_null(), hasZone = shown["timeSource"] == "zone";
            if (visibleWeather != hasWeather || visibleZone != hasZone) {
                self->publish(shown);
                visibleWeather = hasWeather; visibleZone = hasZone;
            }
            const double age = saved.is_object() ? now() - saved.value("fetchedAt", 0.0) : 1e300;
            if (age >= kRefresh && Clock::now() >= nextAttempt) {
                nextAttempt = Clock::now() + std::chrono::seconds(60);
                try {
                    const nlohmann::json fresh = self->forecast(cell);
                    writeAtomically(self->cachePath(cell), fresh.dump());
                    self->publish(display(fresh, at.x, at.y));
                    visibleWeather = visibleZone = true;
                    if (self->log) self->log("WEATHER " + fresh["timezone"].get<std::string>());
                } catch (const std::exception& e) {
                    if (self->log) self->log(std::string("WEATHER-OFFLINE ") + e.what());
                }
            }
        }
    }
};

ConditionsService::ConditionsService(std::string gameRoot, std::function<void(const std::string&)> log)
    : state_(std::make_shared<State>()) {
    state_->game = std::move(gameRoot);
    state_->log = std::move(log);
    std::thread(State::run, state_).detach();
}

ConditionsService::~ConditionsService() {
    {
        std::lock_guard<std::mutex> g(state_->lock);
        state_->stopping = true;
    }
    state_->wake.notify_all();
}

void ConditionsService::ask(double lon, double lat) {
    std::lock_guard<std::mutex> g(state_->lock);
    state_->where = P2{lon, lat};
}

std::shared_ptr<const nlohmann::json> ConditionsService::latest() const {
    std::lock_guard<std::mutex> g(state_->lock);
    return state_->doc;
}

}  // namespace r1
