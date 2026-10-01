#include "predict.hpp"

#include "palette.hpp"
#include "polygons.hpp"
#include "streets.hpp"

#include <algorithm>
#include <set>

namespace r1 {

// ── jurisdiction ────────────────────────────────────────────────────────────

namespace {
// Metropolitan France, coarse: its land borders to within a few kilometres,
// its coasts pushed out to sea, where there are no roads to misjudge.
const std::vector<P2> kFrance = {
    {2.55, 51.09}, {3.00, 50.78}, {3.30, 50.53}, {4.20, 50.27}, {4.80, 50.15}, {4.87, 49.80}, {5.47, 49.50},
    {6.37, 49.46}, {7.00, 49.15}, {7.60, 49.07}, {8.23, 48.97}, {7.80, 48.50}, {7.55, 47.58}, {7.10, 47.49},
    {6.95, 47.35}, {6.45, 46.95}, {6.10, 46.55}, {6.00, 46.25}, {6.30, 46.25}, {6.80, 46.43}, {6.80, 46.10},
    {7.04, 45.92}, {6.80, 45.70}, {6.63, 45.10}, {7.07, 44.85}, {6.95, 44.40}, {7.70, 44.15}, {7.53, 43.78},
    {7.60, 43.20}, {6.60, 42.95}, {5.00, 43.05}, {4.00, 43.25}, {3.30, 42.90}, {3.17, 42.43}, {2.50, 42.35},
    {1.75, 42.60}, {1.40, 42.72}, {0.70, 42.80}, {-0.30, 42.80}, {-1.40, 43.05}, {-1.78, 43.37}, {-2.30, 43.60},
    {-2.60, 46.00}, {-3.20, 47.00}, {-5.50, 47.90}, {-5.40, 48.75}, {-3.50, 48.95}, {-2.30, 48.72},
    {-1.75, 49.00}, {-1.98, 49.75}, {-1.20, 49.80}, {0.00, 49.80}, {1.40, 50.30}, {1.55, 51.10}};
const std::vector<P2> kCorsica = {{8.40, 41.30}, {9.70, 41.30}, {9.70, 43.10}, {8.40, 43.10}};
// The contiguous United States: the 49th parallel, the Great Lakes down their
// middles, the St. Lawrence, New Brunswick, the Rio Grande; coasts out at sea.
const std::vector<P2> kUnitedStates = {
    {-124.9, 48.45}, {-123.3, 48.25}, {-123.2, 49.0}, {-95.15, 49.0}, {-95.15, 49.38}, {-94.6, 48.72},
    {-93.0, 48.62}, {-90.8, 48.2}, {-89.6, 48.0}, {-88.4, 48.3}, {-84.9, 46.9}, {-84.4, 46.5},
    {-83.6, 46.1}, {-83.2, 45.4}, {-82.5, 45.3}, {-82.1, 43.6}, {-82.45, 42.95}, {-82.5, 42.6},
    {-83.1, 42.3}, {-83.1, 42.0}, {-81.3, 42.25}, {-79.9, 42.6}, {-78.95, 42.85}, {-79.05, 43.25},
    {-78.7, 43.63}, {-76.8, 43.63}, {-76.3, 44.2}, {-75.3, 44.85}, {-74.7, 45.0}, {-71.5, 45.01},
    {-71.1, 45.3}, {-70.0, 46.7}, {-69.2, 47.45}, {-68.2, 47.35}, {-67.8, 47.05}, {-67.8, 45.7},
    {-67.4, 45.2}, {-66.9, 44.8}, {-69.5, 43.2}, {-69.9, 41.3}, {-73.8, 40.2}, {-75.3, 35.2},
    {-80.0, 31.0}, {-79.9, 25.5}, {-80.6, 24.4}, {-82.4, 24.4}, {-83.2, 27.8}, {-84.5, 29.6},
    {-90.0, 28.8}, {-94.5, 29.2}, {-97.1, 25.95}, {-97.5, 25.85}, {-99.1, 26.4}, {-99.5, 27.5},
    {-100.3, 28.3}, {-101.4, 29.75}, {-102.7, 29.75}, {-103.2, 29.0}, {-104.5, 29.6}, {-104.9, 30.6},
    {-106.5, 31.78}, {-108.2, 31.78}, {-108.2, 31.33}, {-111.07, 31.33}, {-114.8, 32.49},
    {-114.72, 32.72}, {-117.12, 32.53}, {-117.7, 33.3}, {-120.7, 34.3}, {-122.6, 37.2},
    {-124.5, 40.3}, {-124.8, 43.0}};
const std::vector<P2> kAlaska = {{-179.9, 51.0}, {-141.0, 51.0}, {-141.0, 60.3}, {-137.5, 59.1}, {-135.0, 59.6},
                                 {-133.4, 58.4}, {-130.0, 55.9}, {-130.0, 54.5}, {-141.0, 54.5}, {-141.0, 71.5},
                                 {-179.9, 71.5}};
const std::vector<P2> kHawaii = {{-161.0, 18.5}, {-154.5, 18.5}, {-154.5, 22.5}, {-161.0, 22.5}};

// Used only when the answer does not carry its country (older than question
// 7): a country's outline here is a stopgap, never a source.
const std::vector<std::pair<const char*, const std::vector<P2>*>> kOutlines = {
    {"FR", &kFrance}, {"FR", &kCorsica}, {"US", &kUnitedStates}, {"US", &kAlaska}, {"US", &kHawaii}};

// Where traffic keeps left.
const std::set<std::string> kLeftHand = {
    "AG", "AI", "AU", "BB", "BD", "BM", "BN", "BS", "BT", "BW", "CK", "CY", "DM", "FJ", "FK", "GB", "GD", "GG", "GY",
    "HK", "ID", "IE", "IM", "IN", "JE", "JM", "JP", "KE", "KI", "KN", "KY", "LC", "LK", "LS", "MO", "MS", "MT", "MU",
    "MV", "MW", "MY", "MZ", "NA", "NP", "NR", "NU", "NZ", "PG", "PK", "PN", "SB", "SC", "SG", "SH", "SR", "SZ", "TC",
    "TH", "TK", "TL", "TO", "TT", "TV", "TZ", "UG", "VC", "VG", "VI", "WS", "ZA", "ZM", "ZW"};
}  // namespace

const std::vector<Rulebook>& rulebooks() {
    static const std::vector<Rulebook> books = {
        {"FR", frenchRules, "FR:AB3a", "FR:AB4", "",
         {{"FR:AB4", {"FR:AB3a"}}, {"city_limit", {"FR:EB10", "FR:EB20"}}}},
        {"US", unitedStatesRules, "US:R1-2", "US:R1-1", "US:R1-1,US:R1-3P", {}},
    };
    return books;
}

const Rulebook* rulebookFor(const std::string& country) {
    for (const Rulebook& b : rulebooks()) if (country == b.country) return &b;
    return nullptr;
}

Jurisdiction jurisdictionAt(const OsmData& osm, double lon, double lat) {
    Jurisdiction j;
    if (!osm.country.empty()) {
        j.code = osm.country;
        j.basis = "OSM boundary (ISO3166-1)";
    } else {
        for (const auto& [code, outline] : kOutlines)
            if (pointInPolygon({lon, lat}, *outline)) { j.code = code; j.basis = "coarse outline (answer carries no country)"; break; }
        if (j.code.empty()) j.basis = "unknown (answer carries no country)";
    }
    j.rightHand = !kLeftHand.count(j.code);
    for (const Rulebook& b : rulebooks()) j.rulebook |= j.code == b.country;
    return j;
}

std::vector<const Rule*> rulesFor(const Jurisdiction& j) {
    static const std::vector<Rule> everywhere = structureRules();
    static const std::map<std::string, std::vector<Rule>> books = [] {
        std::map<std::string, std::vector<Rule>> m;
        for (const Rulebook& b : rulebooks()) m[b.country] = b.rules();
        return m;
    }();
    std::vector<const Rule*> out;
    if (auto it = books.find(j.code); it != books.end()) for (const Rule& r : it->second) out.push_back(&r);
    for (const Rule& r : everywhere) out.push_back(&r);
    return out;
}

// ── facts ───────────────────────────────────────────────────────────────────

namespace {
constexpr double kBuildingCell = 50.0;
// Buildings within the 350 m square around a point from which it is in town:
// a French village of two dozen houses is an agglomeration, and the square is
// wide enough that a park or a monument's esplanade inside a city is not the
// open country.
constexpr int kTownReach = 3;
constexpr int kTownBuildings = 25;

std::pair<long, long> cellOf(P2 xz) {
    return {long(std::floor(xz.x / kBuildingCell)), long(std::floor(xz.y / kBuildingCell))};
}
// Towns: buildings on a 40 m grid, each grown by two cells, so houses less
// than 160 m apart are one town, and the town reaches 80 m past its last one
// -- about where France posts its name.
constexpr double kTownCell = 40.0;
constexpr int kTownGrowth = 2, kTownMinimum = 15;
std::pair<long, long> townCellOf(P2 xz) {
    return {long(std::floor(xz.x / kTownCell)), long(std::floor(xz.y / kTownCell))};
}
}  // namespace

Context::Context(const OsmData& o, const RoadNet& n, Jurisdiction j, const std::optional<Bounds>& extent)
    : osm(o), net(n), jurisdiction(std::move(j)) {
    const Anchor& anchor = net.anchor();
    auto xz = [&](P2 p) { const P3 e = anchor.toEngine(p.x, p.y, 0.0); return P2{e.x, e.z}; };
    for (const OsmWay& w : osm.landcover) {
        const std::string use = tagOr(w.tags, "landuse");
        if (use != "residential" && use != "commercial" && use != "retail" && use != "industrial") continue;
        Area a{{}, 1e300, 1e300, -1e300, -1e300};
        for (const P2& p : w.points) {
            const P2 q = xz(p);
            a.ring.push_back(q);
            a.x0 = std::min(a.x0, q.x); a.z0 = std::min(a.z0, q.y); a.x1 = std::max(a.x1, q.x); a.z1 = std::max(a.z1, q.y);
        }
        urbanAreas_.push_back(std::move(a));
    }
    std::map<std::pair<long, long>, int> occupied;
    for (const OsmWay& b : osm.buildings) {
        const P2 at = xz(b.points.front());
        ++buildings_[cellOf(at)];
        ++occupied[townCellOf(at)];
        seen_[0] = std::min(seen_[0], at.x); seen_[1] = std::min(seen_[1], at.y);
        seen_[2] = std::max(seen_[2], at.x); seen_[3] = std::max(seen_[3], at.y);
        if (const std::string* city = tag(b.tags, "addr:city")) addresses_.push_back({at, city});
    }
    // Addressed points count too: a shop, a post box, as the minimap reads them.
    for (const OsmNode& f : osm.features)
        if (const std::string* city = tag(f.tags, "addr:city")) addresses_.push_back({xz({f.lon, f.lat}), city});
    if (extent) {
        const P3 sw = anchor.toEngine(extent->west, extent->south, 0.0), ne = anchor.toEngine(extent->east, extent->north, 0.0);
        seen_[0] = std::min(sw.x, ne.x); seen_[1] = std::min(sw.z, ne.z);
        seen_[2] = std::max(sw.x, ne.x); seen_[3] = std::max(sw.z, ne.z);
    } else {
        seen_[0] -= 250; seen_[1] -= 250; seen_[2] += 250; seen_[3] += 250;
    }
    std::set<std::pair<long, long>> grown;
    for (const auto& [cell, n] : occupied)
        for (long dx = -kTownGrowth; dx <= kTownGrowth; ++dx)
            for (long dz = -kTownGrowth; dz <= kTownGrowth; ++dz) grown.insert({cell.first + dx, cell.second + dz});
    // Connected pieces of the grown grid, in the grid's own order.
    std::map<std::pair<long, long>, int> piece;
    std::vector<int> sizes;
    for (const auto& start : grown) {
        if (piece.count(start)) continue;
        const int id = int(sizes.size());
        sizes.push_back(0);
        std::vector<std::pair<long, long>> stack{start};
        piece[start] = id;
        while (!stack.empty()) {
            const auto cell = stack.back();
            stack.pop_back();
            if (auto it = occupied.find(cell); it != occupied.end()) sizes.back() += it->second;
            for (long dx = -1; dx <= 1; ++dx)
                for (long dz = -1; dz <= 1; ++dz) {
                    const std::pair<long, long> next{cell.first + dx, cell.second + dz};
                    if (grown.count(next) && !piece.count(next)) { piece[next] = id; stack.push_back(next); }
                }
        }
    }
    for (const auto& [cell, id] : piece) if (sizes[size_t(id)] >= kTownMinimum) townCells_[cell] = id;
}

int Context::townAt(P2 at) const {
    auto it = townCells_.find(townCellOf(at));
    return it == townCells_.end() ? -1 : it->second;
}

std::string Context::townName(P2 at, int town) const {
    // The addresses near the gate first -- two communes can share one built-up
    // area -- then the whole town's. Two addresses at least: one could be a
    // typo, and a town is never named on a guess.
    for (const double reach : {600.0, 1e300}) {
        std::map<std::string, int> votes;
        for (const auto& [p, city] : addresses_)
            if (dist(p, at) < reach && townAt(p) == town) ++votes[*city];
        std::string best;
        int most = 1;
        for (const auto& [name, n] : votes)
            if (n > most) { most = n; best = name; }
        if (!best.empty()) return best;
    }
    return "";
}

bool Context::dense(P2 at) const {
    const auto [cx, cz] = cellOf(at);
    int count = 0;
    for (long dx = -kTownReach; dx <= kTownReach; ++dx)
        for (long dz = -kTownReach; dz <= kTownReach; ++dz) {
            auto it = buildings_.find({cx + dx, cz + dz});
            if (it != buildings_.end()) count += it->second;
        }
    return count >= kTownBuildings;
}

Prediction Context::signAt(int way, P2 at, P2 travel) const {
    RoadNet::Walk w;
    w.xz = at;
    w.dir = travel;
    w.way = way;
    return beside(w, false);
}

std::pair<bool, const char*> Context::urbanAt(int way, P2 at) const {
    const RoadNet::Way& w = net.ways()[size_t(way)];
    for (const char* key : {"maxspeed:type", "source:maxspeed", "zone:traffic", "zone:maxspeed"}) {
        const std::string v = tagOr(w.osm->tags, key);
        if (v.find(":urban") != std::string::npos || v.find("zone") != std::string::npos ||
            v == "FR:30" || v == "FR:20")
            return {true, "tagged"};
        if (v.find(":rural") != std::string::npos || v.find(":trunk") != std::string::npos ||
            v.find(":motorway") != std::string::npos)
            return {false, "tagged"};
    }
    if (w.highway == "motorway") return {false, "road class"};
    for (const Area& a : urbanAreas_)
        if (a.x0 <= at.x && at.x <= a.x1 && a.z0 <= at.y && at.y <= a.z1 && pointInPolygon(at, a.ring))
            return {true, "land use"};
    if (townAt(at) >= 0) return {true, "town"};
    return {dense(at), "building density"};
}

// ── placement ───────────────────────────────────────────────────────────────
// A sign stands on the traffic's own side, clear of the carriageway: half the
// road's width and 90 cm more, where a post goes on a verge or a kerb.

Prediction Context::beside(const RoadNet::Walk& w, bool arriving) const {
    const RoadNet::Way& way = net.ways()[size_t(w.way)];
    const P2 t = arriving ? P2{-w.dir.x, -w.dir.y} : w.dir;
    // Engine z points south: the right of a heading (tx, tz) is (-tz, tx).
    const double side = jurisdiction.rightHand ? 1.0 : -1.0;
    const double offset = roadWidth(way.osm->tags) / 2 + 0.9;
    Prediction p;
    p.xz = {w.xz.x - t.y * side * offset, w.xz.y + t.x * side * offset};
    p.travel = t;
    p.urban = urbanAt(w.way, w.xz).first;
    p.evidence = way.osm->id;
    return p;
}

std::optional<Prediction> Context::signAfter(int vertex, const RoadNet::Branch& leave, double metres,
                                             double minimum) const {
    RoadNet::Walk w = net.walk(vertex, leave, metres);
    if (w.reached < metres) {
        // A junction comes first: stand before it, or not at all.
        const double at = std::max(minimum, w.reached - 8);
        if (at > w.reached - 3) return std::nullopt;
        w = net.walk(vertex, leave, at);
    }
    if (w.dir.x == 0 && w.dir.y == 0) return std::nullopt;
    return beside(w, false);
}

std::optional<Prediction> Context::signBefore(int vertex, const RoadNet::Branch& arrival, double metres,
                                              double minimum) const {
    RoadNet::Walk w = net.walk(vertex, arrival, metres, true);
    if (w.reached < metres) {
        const double at = std::max(minimum, w.reached - 8);
        if (at > w.reached - 3) return std::nullopt;
        w = net.walk(vertex, arrival, at, true);
    }
    if (w.dir.x == 0 && w.dir.y == 0) return std::nullopt;
    return beside(w, true);
}

// ── arbitration and emission ────────────────────────────────────────────────

namespace {
constexpr double kSurveyRadius = 30.0;   // a surveyed sign silences a guess this close
constexpr double kMergeRadius = 25.0;    // two guesses of one sign this close are one
constexpr int kSignBudget = 160;         // per tile, after everything else

// "FR:B14[30]" -> "FR:B14"; a post of several plates has several families,
// and a code its rulebook says regulates the same thing counts as that.
std::vector<std::string> families(const std::string& what, const Rulebook* book) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= what.size()) {
        size_t end = what.find_first_of(",;", start);
        if (end == std::string::npos) end = what.size();
        std::string code = what.substr(start, end - start);
        while (!code.empty() && std::isspace((unsigned char)code.front())) code.erase(code.begin());
        const size_t bracket = code.find('[');
        if (bracket != std::string::npos) code.resize(bracket);
        bool aliased = false;
        if (book)
            for (const auto& [alias, codes] : book->aliases)
                if (code == alias) { out.insert(out.end(), codes.begin(), codes.end()); aliased = true; }
        if (!code.empty() && !aliased) out.push_back(code);
        start = end + 1;
    }
    return out;
}

bool isStructure(const Prediction& p) { return p.what == "bridge"; }

// ── what OSM surveyed, drawn ────────────────────────────────────────────────

// The sign a surveyed node is, in a code the catalogue draws; empty if none.
std::string surveyedCode(const Tags& t, const Rulebook* book) {
    const auto& signs = palette().signs;
    std::string code = tagOr(t, "traffic_sign");
    for (char& ch : code) if (ch == ';') ch = ',';
    const std::string highway = tagOr(t, "highway");
    if (code.empty() && book) {
        if (highway == "stop") code = tagOr(t, "stop") == "all" && *book->allWay ? book->allWay : book->stop;
        else if (highway == "give_way") code = book->giveWay;
    }
    if (signs.count(code)) return code;
    const size_t comma = code.find(',');
    if (comma != std::string::npos && signs.count(code.substr(0, comma))) return code.substr(0, comma);
    return "";
}

// Where a surveyed sign stands and whom it faces. Its place is OSM's; which
// way it looks is inferred: on a road, the traffic driving towards the next
// junction (or the way its `direction` says); beside one, the traffic that
// has it on its right.
std::vector<Prediction> placeSurveyed(const Context& c, const OsmNode& f, std::string& why) {
    const RoadNet& net = c.net;
    const P3 e = net.anchor().toEngine(f.lon, f.lat, 0.0);
    const P2 at{e.x, e.z};
    const int v = net.vertexAt({f.lon, f.lat});
    if (v >= 0) {
        if (net.isJunction(v)) {
            // On the crossing itself: every approach's, when OSM says so.
            std::vector<Prediction> all;
            why = "on a crossing, approaches unknown";
            if (tagOr(f.tags, "highway") != "stop" || tagOr(f.tags, "stop") != "all") return all;
            for (const RoadNet::Branch& a : net.arrivals(v)) {
                if (!a.legal || net.ways()[size_t(a.way)].highway == "service") continue;
                if (auto p = c.signBefore(v, a, 4.0, 2.0)) { p->evidence = f.id; all.push_back(*p); }
            }
            return all;
        }
        const std::string direction = tagOr(f.tags, "direction");
        std::optional<RoadNet::Branch> best;
        double nearest = 1e300;
        for (const RoadNet::Branch& b : net.branches(v)) {
            if (!b.legal) continue;
            if ((direction == "forward" && b.step < 0) || (direction == "backward" && b.step > 0)) continue;
            const auto [stop, distance] = net.reach(v, b, 100.0);
            const bool junction = stop >= 0 && net.isJunction(stop);
            const double d = junction ? distance : (direction.empty() ? 1e300 : 1e6);
            if (d < nearest) { nearest = d; best = b; }
        }
        if (!best || nearest >= 1e300) { why = "no junction ahead within 100 m"; return {}; }
        const auto walk = net.walk(v, *best, 0.5);
        Prediction p = c.signAt(best->way, at, walk.dir);
        p.evidence = f.id;
        return {p};
    }
    // Beside the road: the nearest carriageway within 15 m.
    double nearest = 15.0;
    std::optional<Prediction> out;
    const auto& ways = net.ways();
    const auto& verts = net.vertices();
    for (int wi = 0; wi < int(ways.size()); ++wi)
        for (size_t i = 0; i + 1 < ways[size_t(wi)].vertices.size(); ++i) {
            const P2 a = verts[size_t(ways[size_t(wi)].vertices[i])].xz, b = verts[size_t(ways[size_t(wi)].vertices[i + 1])].xz;
            const double dx = b.x - a.x, dz = b.y - a.y, l2 = dx * dx + dz * dz;
            if (l2 < 1e-9) continue;
            const double t = std::clamp(((at.x - a.x) * dx + (at.y - a.y) * dz) / l2, 0.0, 1.0);
            const P2 foot{a.x + dx * t, a.y + dz * t};
            const double d = dist(foot, at);
            if (d >= nearest) continue;
            nearest = d;
            const double l = std::sqrt(l2);
            P2 travel{dx / l, dz / l};
            // Traffic keeps right: it reads the signs on its right.
            if ((at.x - foot.x) * -travel.y + (at.y - foot.y) * travel.x < 0) travel = {-travel.x, -travel.y};
            if (!c.jurisdiction.rightHand) travel = {-travel.x, -travel.y};
            Prediction p;
            p.xz = at;
            p.travel = travel;
            p.urban = c.urbanAt(wi, at).first;
            p.evidence = f.id;
            out = p;
        }
    if (!out) { why = "no road within 15 m"; return {}; }
    return {*out};
}

// A stop or give-way mapped on the crossing node without saying whose it
// is: surveyed that there is one, not where. It silences nothing; the rules
// know which approaches it belongs to.
bool placeless(const OsmNode& f, const RoadNet& net) {
    const std::string h = tagOr(f.tags, "highway");
    if (h != "stop" && h != "give_way") return false;
    if (h == "stop" && tagOr(f.tags, "stop") == "all") return false;
    const int v = net.vertexAt({f.lon, f.lat});
    return v >= 0 && net.isJunction(v);
}
bool isTownSign(const std::string& what) { return what == "FR:EB10" || what == "FR:EB20"; }
bool isStreetSign(const std::string& what) { return what == "US:D3-1"; }
// Signs assembled letter by letter cost a node a letter: a few per tile.
const std::map<std::string, int> kBudgets = {{"US:D3-1", 16}};

// ── a town's name, assembled ────────────────────────────────────────────────

std::vector<uint32_t> decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        uint32_t cp = n == 1 ? c : c & (0x3F >> (n - 1));
        for (int k = 1; k < n && i + size_t(k) < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + size_t(k)] & 0x3F);
        out.push_back(cp);
        i += size_t(n);
    }
    return out;
}
std::string encode(uint32_t c) {
    std::string s;
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    return s;
}
uint32_t upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if (c == 0x153) return 0x152;
    // Latin Extended-A pairs its capitals and small letters as even and odd
    // code points (0x100-0x137, 0x14A-0x177), then odd and even (0x139-0x148,
    // 0x179-0x17E); the Romanian comma letters are 0x218-0x21B.
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return c & ~1u;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1u) ? c : c - 1;
    if (c >= 0x218 && c <= 0x21B) return c & ~1u;
    if (c == 0x131) return 'I';
    return c;
}
// What the atlas does not draw, written as French signs write it.
std::string plainly(uint32_t c) {
    switch (c) {
        case 0xC1: case 0xC3: case 0xC5: return "A";
        case 0xCD: case 0xCC: return "I";
        case 0xD3: case 0xD2: case 0xD5: case 0xD8: return "O";
        case 0xDA: return "U";
        case 0xD1: return "N";
        case 0xDD: return "Y";
        case 0xC6: return "AE";
        case 0x152: return "OE";
        case 0xDF: return "SS";
        case 0xD0: case 0x110: case 0x10E: return "D";
        case 0xDE: return "TH";
        case 0x100: case 0x102: case 0x104: return "A";
        case 0x106: case 0x108: case 0x10A: case 0x10C: return "C";
        case 0x112: case 0x114: case 0x116: case 0x118: case 0x11A: return "E";
        case 0x11C: case 0x11E: case 0x120: case 0x122: return "G";
        case 0x124: case 0x126: return "H";
        case 0x128: case 0x12A: case 0x12C: case 0x12E: case 0x130: return "I";
        case 0x134: return "J";
        case 0x136: return "K";
        case 0x139: case 0x13B: case 0x13D: case 0x13F: case 0x141: return "L";
        case 0x143: case 0x145: case 0x147: return "N";
        case 0x14C: case 0x14E: case 0x150: return "O";
        case 0x154: case 0x156: case 0x158: return "R";
        case 0x15A: case 0x15C: case 0x15E: case 0x160: case 0x218: return "S";
        case 0x162: case 0x164: case 0x166: case 0x21A: return "T";
        case 0x168: case 0x16A: case 0x16C: case 0x16E: case 0x170: case 0x172: return "U";
        case 0x174: return "W";
        case 0x176: return "Y";
        case 0x179: case 0x17B: case 0x17D: return "Z";
        case 0x2019: case 0x2018: return "'";
        case 0x2010: case 0x2011: case 0x2013: return "-";
        default: return "";
    }
}

// A name as the kit's glyphs, capitals; nothing when a letter has none.
std::optional<std::vector<std::string>> spell(const TownSignKit& kit, const std::string& name);

// ── street-name blades ──────────────────────────────────────────────────────

// Two blades crossed on one post, each along its street and read from both
// sides: every part a shared model, every letter one (CLAUDE.md §5).
std::optional<nlohmann::json> streetSign(const TownSignKit& kit, const std::string (&names)[2], const P2 (&along)[2]) {
    const auto plate = kit.plates.find(1);
    if (plate == kit.plates.end()) return std::nullopt;
    nlohmann::json children = nlohmann::json::array();
    auto node = [](const std::string& model, P3 at, P3 size, nlohmann::json rotation = {0.0, 0.0, 0.0, 1.0}) {
        return nlohmann::json{{"type", "Node"}, {"name", "Part"}, {"importedFrom", model},
                              {"transform", {{"position", {at.x, at.y, at.z}}, {"rotation", rotation},
                                             {"scale", {size.x, size.y, size.z}}}}};
    };
    children.push_back(node(kit.post, {0, 0, 0}, {1, kit.lowerEdge, 1}));
    for (int k = 0; k < 2; ++k) {
        const auto chars = spell(kit, names[k]);
        if (!chars) return std::nullopt;
        double text = 0;
        for (const std::string& ch : *chars) text += ch == " " ? 0.45 * kit.cap : kit.glyphs.at(ch).second;
        const double scale = text > 2.2 ? 2.2 / text : 1.0;
        const double w = text * scale + 2 * (kit.end + 0.05), y0 = 0;
        nlohmann::json parts = nlohmann::json::array();
        parts.push_back(node(plate->second.left, {-w / 2, y0, 0}, {1, 1, 1}));
        parts.push_back(node(plate->second.middle, {-w / 2 + kit.end, y0, 0}, {w - 2 * kit.end, 1, 1}));
        parts.push_back(node(plate->second.right, {w / 2, y0, 0}, {1, 1, 1}));
        double x = -text * scale / 2;
        const nlohmann::json turned = {0.0, 1.0, 0.0, 0.0};  // half a turn: the far side
        for (const std::string& ch : *chars) {
            const double advance = (ch == " " ? 0.45 * kit.cap : kit.glyphs.at(ch).second) * scale;
            if (ch != " ") {
                const std::string& model = kit.glyphs.at(ch).first;
                parts.push_back(node(model, {x, y0 + kit.pad, 0}, {scale, scale, 1}));
                parts.push_back(node(model, {-x, y0 + kit.pad, 0}, {scale, scale, 1}, turned));
            }
            x += advance;
        }
        // The blade's length along its street: local +X turned onto `along`.
        const double yaw = std::atan2(-along[k].y, along[k].x);
        children.push_back({{"type", "Node"}, {"name", "Blade"}, {"children", parts},
                            {"transform", {{"position", {0.0, kit.lowerEdge + k * kit.line, 0.0}},
                                           {"rotation", {0.0, std::sin(yaw / 2), 0.0, std::cos(yaw / 2)}},
                                           {"scale", {1.0, 1.0, 1.0}}}}});
    }
    return children;
}

// The sign as a node's children: its posts, its plate cut to the name, its
// letters, and for the way out the bar struck through it. Every child is a
// shared model; nothing is drawn per town (CLAUDE.md §5). Nothing when a
// letter of the name has no glyph: a misspelt town is worse than none.
std::optional<std::vector<std::string>> spell(const TownSignKit& kit, const std::string& name) {
    std::vector<std::string> chars;
    for (uint32_t c : decode(name)) {
        c = upper(c);
        if (c == ' ') { chars.push_back(" "); continue; }
        const std::string key = encode(c);
        if (kit.glyphs.count(key)) { chars.push_back(key); continue; }
        const std::string alt = plainly(c);
        if (alt.empty()) return std::nullopt;
        for (char a : alt) {
            if (!kit.glyphs.count(std::string(1, a))) return std::nullopt;
            chars.push_back(std::string(1, a));
        }
    }
    if (chars.empty()) return std::nullopt;
    return chars;
}

std::optional<nlohmann::json> townSign(const TownSignKit& kit, const std::string& name, bool exit) {
    const auto spelt = spell(kit, name);
    if (!spelt) return std::nullopt;
    const std::vector<std::string>& chars = *spelt;
    auto advance = [&](const std::string& ch) { return ch == " " ? 0.45 * kit.cap : kit.glyphs.at(ch).second; };
    auto width = [&](size_t from, size_t to) {
        while (from < to && chars[from] == " ") ++from;
        while (to > from && chars[to - 1] == " ") --to;
        double w = 0;
        for (size_t i = from; i < to; ++i) w += advance(chars[i]);
        return w;
    };
    // Two lines when the name is long, broken after a hyphen or at a space
    // nearest its middle.
    std::vector<std::pair<size_t, size_t>> lines{{0, chars.size()}};
    if (width(0, chars.size()) > 2.4) {
        size_t best = 0;
        double widest = 1e300;
        for (size_t i = 1; i < chars.size(); ++i) {
            const bool breakable = chars[i - 1] == "-" || chars[i] == " ";
            if (!breakable) continue;
            const double w = std::max(width(0, i), width(i, chars.size()));
            if (w < widest) { widest = w; best = i; }
        }
        if (best) lines = {{0, best}, {best, chars.size()}};
    }
    double text = 0;
    for (const auto& [a, b] : lines) text = std::max(text, width(a, b));
    const double scale = text > 3.2 ? 3.2 / text : 1.0;
    const auto plate = kit.plates.find(int(lines.size()));
    if (plate == kit.plates.end()) return std::nullopt;
    const double h = plate->second.height, w = text * scale + 2 * (kit.end + 0.06), y0 = kit.lowerEdge;
    nlohmann::json children = nlohmann::json::array();
    auto part = [&](const std::string& model, P3 at, P3 size, nlohmann::json rotation = {0.0, 0.0, 0.0, 1.0}) {
        children.push_back({{"type", "Node"}, {"name", "Part"}, {"importedFrom", model},
                            {"transform", {{"position", {at.x, at.y, at.z}}, {"rotation", rotation},
                                           {"scale", {size.x, size.y, size.z}}}}});
    };
    for (double x : w < 1.3 ? std::vector<double>{0.0} : std::vector<double>{-w / 4, w / 4})
        part(kit.post, {x, 0, 0}, {1, y0 + h - 0.05, 1});
    part(plate->second.left, {-w / 2, y0, 0}, {1, 1, 1});
    part(plate->second.middle, {-w / 2 + kit.end, y0, 0}, {w - 2 * kit.end, 1, 1});
    part(plate->second.right, {w / 2, y0, 0}, {1, 1, 1});
    for (size_t l = 0; l < lines.size(); ++l) {
        auto [from, to] = lines[l];
        while (from < to && chars[from] == " ") ++from;
        while (to > from && chars[to - 1] == " ") --to;
        const double baseline = y0 + kit.pad + (lines.size() - 1 - l) * kit.line;
        double x = -width(from, to) * scale / 2;
        for (size_t i = from; i < to; ++i) {
            if (chars[i] != " ") part(kit.glyphs.at(chars[i]).first, {x, baseline, 0}, {scale, scale, 1});
            x += advance(chars[i]) * scale;
        }
    }
    if (exit) {
        const double a = std::atan2(h, w);
        part(kit.bar, {0, y0 + h / 2, 0}, {std::hypot(w, h) * 0.92, 0.07, 1},
             {0.0, 0.0, std::sin(a / 2), std::cos(a / 2)});
    }
    return children;
}

nlohmann::json yaw(double y) { return {0.0, std::sin(y * 0.5), 0.0, std::cos(y * 0.5)}; }
}  // namespace

std::optional<nlohmann::json> facadeLettering(const std::string& name,double width,double height) {
    const auto found=palette().streetSigns.find("US");
    if(found==palette().streetSigns.end())return std::nullopt;
    const auto& kit=found->second;const auto chars=spell(kit,name);if(!chars)return std::nullopt;
    double total=0;for(auto& ch:*chars)total+=ch==" "?.45*kit.cap:kit.glyphs.at(ch).second;
    if(total<=0)return std::nullopt;
    double scale=std::min(width/total,height/kit.cap),x=-total*scale/2;
    nlohmann::json children=nlohmann::json::array();
    for(auto& ch:*chars) {
        if(ch!=" ")children.push_back({{"type","Node"},{"name","Fascia letter"},{"importedFrom",kit.glyphs.at(ch).first},
            {"transform",{{"position",{x,0.,0.}},{"scale",{scale,scale,1.}}}}});
        x+=(ch==" "?.45*kit.cap:kit.glyphs.at(ch).second)*scale;
    }
    return nlohmann::json{{"type","Node"},{"name","Store name: "+name},{"children",children}};
}

PredictOutput predictDetails(const OsmData& osm, const Tile& tile, const Anchor& anchor, const GroundAt& ground,
                             const std::optional<Bounds>& extent) {
    const Bounds bounds = tile.bounds();
    const P2 centre = tile.center();
    const Jurisdiction jurisdiction = jurisdictionAt(osm, centre.x, centre.y);
    // 450 m around the tile: a rule reasons from a junction up to a few
    // hundred metres from the sign it places.
    const RoadNet net(osm.roads, anchor, bounds, 0.004);
    const Context context(osm, net, jurisdiction, extent);

    // 2. Rules propose.
    const std::vector<const Rule*> rules = rulesFor(jurisdiction);
    std::vector<Prediction> proposals;
    std::map<std::string, int> order;
    nlohmann::json perRule = nlohmann::json::object();
    for (const Rule* rule : rules) {
        const size_t before = proposals.size();
        rule->propose(context, *rule, proposals);
        for (size_t i = before; i < proposals.size(); ++i) {
            proposals[i].rule = rule->id;
            if (proposals[i].confidence <= 0) proposals[i].confidence = rule->confidence;
        }
        order[rule->id] = int(order.size());
        perRule[rule->id] = {{"says", rule->says}, {"confidence", rule->confidence},
                             {"proposed", int(proposals.size() - before)}, {"silencedBySurvey", 0},
                             {"merged", 0}, {"outsideTile", 0}, {"overBudget", 0}, {"noModel", 0}, {"placed", 0}};
    }

    // 3. Arbitration. What OSM surveyed first: a guess never stands next to
    // the sign it would have guessed.
    const Rulebook* book = rulebookFor(jurisdiction.code);
    struct Surveyed { std::string family; P2 xz; };
    std::vector<Surveyed> surveyed;
    for (const OsmNode& f : osm.features) {
        std::string codes = tagOr(f.tags, "traffic_sign");
        const std::string highway = tagOr(f.tags, "highway");
        // A give-way or stop line surveyed without its sign code is still that
        // sign, in the country's own code.
        if (!placeless(f, net)) {
            if (book && highway == "give_way") codes += std::string(",") + book->giveWay;
            if (book && highway == "stop") codes += std::string(",") + book->stop;
        }
        if (codes.empty()) continue;
        const P3 e = anchor.toEngine(f.lon, f.lat, 0.0);
        for (const std::string& family : families(codes, book)) surveyed.push_back({family, {e.x, e.z}});
    }
    // What OSM surveyed is drawn, not only obeyed.
    {
        const size_t before = proposals.size();
        std::map<std::string, int> unplaced;
        for (const OsmNode& f : osm.features) {
            // The tile's own and those just past its edge: a sign's place
            // is where it was mapped, and the tile owns what stands on it.
            if (f.lon < bounds.west - 0.001 || f.lon > bounds.east + 0.001 || f.lat < bounds.south - 0.001 ||
                f.lat > bounds.north + 0.001)
                continue;
            const std::string what = surveyedCode(f.tags, book);
            if (what.empty() || placeless(f, net)) continue;
            std::string why;
            auto placed = placeSurveyed(context, f, why);
            if (placed.empty()) { ++unplaced[why]; continue; }
            for (Prediction& p : placed) {
                p.what = what;
                p.rule = "survey";
                p.confidence = 1.0;
                proposals.push_back(std::move(p));
            }
        }
        order["survey"] = -1;
        perRule["survey"] = {{"says", "a sign OSM surveyed stands where it was mapped, facing the traffic it regulates"},
                             {"confidence", 1.0}, {"proposed", int(proposals.size() - before)}, {"unplaced", unplaced},
                             {"silencedBySurvey", 0}, {"merged", 0}, {"outsideTile", 0}, {"overBudget", 0},
                             {"noModel", 0}, {"placed", 0}};
    }
    std::stable_sort(proposals.begin(), proposals.end(), [&](const Prediction& a, const Prediction& b) {
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        if (order[a.rule] != order[b.rule]) return order[a.rule] < order[b.rule];
        return a.evidence < b.evidence;
    });
    PredictOutput out;
    std::vector<const Prediction*> kept;
    nlohmann::json structures = nlohmann::json::array();
    std::map<std::string, int> byWhat, towns;
    int signs = 0;
    auto bump = [](nlohmann::json& stats, const char* key) { stats[key] = stats[key].get<int>() + 1; };
    for (const Prediction& p : proposals) {
        nlohmann::json& stats = perRule[p.rule];
        bool silenced = false;
        if (!isStructure(p) && p.rule != "survey")
            for (const std::string& family : families(p.what, book))
                for (const Surveyed& s : surveyed)
                    silenced |= s.family == family && dist(s.xz, p.xz) < kSurveyRadius;
        if (silenced) { bump(stats, "silencedBySurvey"); continue; }
        bool merged = false;
        for (const Prediction* k : kept)
            merged |= k->what == p.what && dist(k->xz, p.xz) < kMergeRadius &&
                      k->travel.x * p.travel.x + k->travel.y * p.travel.y > 0.87;
        if (merged) { bump(stats, "merged"); continue; }
        kept.push_back(&p);
        if (isStructure(p)) out.structures.push_back(p);
        // Each detail belongs to the one tile it stands on.
        const P3 geo = anchor.toGeodetic(p.xz.x, 0.0, p.xz.y);
        if (!(bounds.west <= geo.x && geo.x < bounds.east && bounds.south <= geo.y && geo.y < bounds.north)) {
            bump(stats, "outsideTile");
            continue;
        }
        if (isStructure(p)) {
            nlohmann::json s = p.detail;
            s["kind"] = p.what;
            s["rule"] = p.rule;
            s["confidence"] = p.confidence;
            s["at"] = {pyround(geo.x, 7), pyround(geo.y, 7)};
            structures.push_back(s);
            bump(stats, "placed");
            continue;
        }
        if (signs >= kSignBudget) { bump(stats, "overBudget"); continue; }
        if (auto limit = kBudgets.find(p.what); limit != kBudgets.end() && byWhat[p.what] >= limit->second) {
            bump(stats, "overBudget");
            continue;
        }
        if (isStreetSign(p.what)) {
            const auto kit = palette().streetSigns.find(jurisdiction.code);
            const std::string names[2] = {p.detail["names"][0].get<std::string>(), p.detail["names"][1].get<std::string>()};
            const P2 along[2] = {{p.detail["along"][0][0].get<double>(), p.detail["along"][0][1].get<double>()},
                                 {p.detail["along"][1][0].get<double>(), p.detail["along"][1][1].get<double>()}};
            const auto children = kit == palette().streetSigns.end() ? std::nullopt : streetSign(kit->second, names, along);
            if (!children) { bump(stats, "noModel"); continue; }
            const P3 at = ground(geo.x, geo.y);
            out.nodes.push_back({{"type", "Node"}, {"name", "Sign " + p.what + " inferred " + p.rule + " " + std::to_string(p.evidence) + " " + names[0] + " / " + names[1]},
                                 {"enabled", true}, {"groups", {"roadside", "inferred"}}, {"children", *children},
                                 {"transform", {{"position", {at.x, at.y, at.z}}, {"rotation", {0.0, 0.0, 0.0, 1.0}},
                                                {"scale", {1.0, 1.0, 1.0}}}}});
            ++signs;
            ++byWhat[p.what];
            bump(stats, "placed");
            continue;
        }
        if (isTownSign(p.what)) {
            const auto kit = palette().townSigns.find(jurisdiction.code);
            const std::string name = p.detail.value("name", std::string());
            const auto children = kit == palette().townSigns.end() ? std::nullopt : townSign(kit->second, name, p.what == "FR:EB20");
            if (!children) { bump(stats, "noModel"); continue; }
            const P3 at = ground(geo.x, geo.y);
            out.nodes.push_back({{"type", "Node"}, {"name", "Sign " + p.what + " inferred " + p.rule + " " + std::to_string(p.evidence) + " " + name},
                                 {"enabled", true}, {"groups", {"roadside", "inferred"}}, {"children", *children},
                                 {"transform", {{"position", {at.x, at.y, at.z}},
                                                {"rotation", yaw(std::atan2(-p.travel.x, -p.travel.y))},
                                                {"scale", {1.0, 1.0, 1.0}}}}});
            ++signs;
            ++byWhat[p.what];
            ++towns[name];
            bump(stats, "placed");
            continue;
        }
        // 4. Emission: a node on the sign's shared model, its face (+Z in
        // the model) turned to meet the traffic it speaks to.
        const auto model = palette().signs.find(p.what);
        const std::string mount = p.urban ? "urban" : "rural";
        if (model == palette().signs.end() || !model->second.mounts.count(mount)) { bump(stats, "noModel"); continue; }
        const P3 at = ground(geo.x, geo.y);
        const bool measured = p.rule == "survey";
        out.nodes.push_back({{"type", "Node"}, {"name", "Sign " + p.what + (measured ? " surveyed " : " inferred ") + p.rule + " " + std::to_string(p.evidence)},
                             {"enabled", true}, {"groups", {"roadside", measured ? "surveyed" : "inferred"}},
                             {"transform", {{"position", {at.x, at.y, at.z}},
                                            {"rotation", yaw(std::atan2(-p.travel.x, -p.travel.y))},
                                            {"scale", {1.0, 1.0, 1.0}}}},
                             {"importedFrom", model->second.mounts.at(mount)}});
        ++signs;
        ++byWhat[p.what];
        bump(stats, "placed");
    }
    out.stats = {{"revision", kPredictRevision}, {"jurisdiction", jurisdiction.code},
                 {"jurisdictionBasis", jurisdiction.basis}, {"rulebook", jurisdiction.rulebook}, {"provenance", "inferred"},
                 {"signs", signs}, {"signsByCode", byWhat}, {"signBudget", kSignBudget}, {"townsNamed", towns},
                 {"surveyedSigns", surveyed.size()}, {"structures", structures}, {"rules", perRule},
                 // Hump and sign nodes are asked of Overpass from question 7 on;
                 // an older answer only carries those that are also crossings.
                 {"calmingNodesQueried", osm.queryVersion >= 7}};
    return out;
}

}  // namespace r1
