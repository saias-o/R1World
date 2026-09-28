// The rulebook of the United States: the Manual on Uniform Traffic Control
// Devices (MUTCD, FHWA, 2009 edition with its revisions), as regularities of
// the road graph. Each rule proposes; none places (gen/predict.hpp).
#include "predict.hpp"

#include "palette.hpp"
#include "streets.hpp"

#include <algorithm>
#include <cctype>

namespace r1 {

namespace {
using Branch = RoadNet::Branch;

const RoadNet::Way& wayOf(const Context& c, const Branch& b) { return c.net.ways()[size_t(b.way)]; }
Branch reversed(const Branch& b) { return {b.way, b.index, -b.step, b.legal}; }
bool onRing(const Context& c, int vertex) {
    for (const auto& [w, i] : c.net.vertices()[size_t(vertex)].uses)
        if (c.net.ways()[size_t(w)].roundabout) return true;
    return false;
}
// Which road yields to which: the functional class OSM gives.
int rank(const RoadNet::Way& w) {
    if (w.highway == "service") return 0;
    const int r = roadRank(w.highway);
    return r == 1 && w.highway != "residential" && w.highway != "unclassified" && w.highway != "living_street" ? 0 : r;
}
bool minorAccess(const RoadNet::Way& w) { return w.highway == "service" || w.link || w.highway == "motorway"; }

// A speed limit in mph, as the signs say it: tagged "35 mph", or a metric
// value turned back to the 5 mph step it was converted from.
int mph(const std::string* value) {
    if (!value) return 0;
    std::string t;
    for (char ch : *value) t += char(std::tolower((unsigned char)ch));
    // speedKmh reads either unit; back to miles, to the sign's 5 mph step.
    const double v = speedKmh(value);
    if (v <= 0) return 0;
    return int(std::lround(v / 1.609344 / 5.0) * 5);
}

// Junctions that have traffic lights: nobody stops at a sign there.
std::vector<P2> signals(const Context& c) {
    std::vector<P2> out;
    const Anchor& a = c.net.anchor();
    for (const OsmNode& f : c.osm.features)
        if (tagOr(f.tags, "highway") == "traffic_signals") {
            const P3 e = a.toEngine(f.lon, f.lat, 0.0);
            out.push_back({e.x, e.z});
        }
    return out;
}
bool signalled(const std::vector<P2>& lights, P2 at) {
    for (const P2& l : lights) if (dist(l, at) < 30.0) return true;
    return false;
}

void add(std::vector<Prediction>& out, std::optional<Prediction> p, const std::string& what) {
    if (!p) return;
    p->what = what;
    out.push_back(std::move(*p));
}

// ── the rules ───────────────────────────────────────────────────────────────

// A minor road meets a more important one without lights: STOP on the minor
// approaches (MUTCD 2B.05). Where two equal roads meet in a T, the one that
// ends there stops.
void twoWayStops(const Context& c, const Rule&, std::vector<Prediction>& out) {
    const auto lights = signals(c);
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        if (!c.net.isJunction(v) || onRing(c, v) || signalled(lights, c.net.vertices()[size_t(v)].xz)) continue;
        int top = 0, legs = 0;
        bool through = false;
        for (const Branch& b : c.net.branches(v)) {
            const RoadNet::Way& w = wayOf(c, b);
            if (minorAccess(w)) continue;
            top = std::max(top, rank(w));
            ++legs;
            through |= b.index > 0 && b.index + 1 < int(w.vertices.size());
        }
        for (const Branch& a : c.net.arrivals(v)) {
            const RoadNet::Way& w = wayOf(c, a);
            if (!a.legal || minorAccess(w) || w.roundabout || rank(w) == 0) continue;
            const bool ends = a.index == 0 || a.index + 1 == int(w.vertices.size());
            const bool minor = rank(w) < top;
            const bool stem = rank(w) == top && legs == 3 && ends && through;
            if (!minor && !stem) continue;
            add(out, c.signBefore(v, a, 4.0, 2.0), "US:R1-1");
        }
    }
}

// Two residential streets crossing, neither more important, no lights: an
// all-way stop, its plaque beneath (MUTCD 2B.07). The commonest control of a
// neighbourhood crossing in the United States, and still a guess.
void allWayStops(const Context& c, const Rule&, std::vector<Prediction>& out) {
    const auto lights = signals(c);
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        if (!c.net.isJunction(v) || onRing(c, v) || signalled(lights, c.net.vertices()[size_t(v)].xz)) continue;
        int legs = 0, lo = 99, hi = 0;
        for (const Branch& b : c.net.branches(v)) {
            const RoadNet::Way& w = wayOf(c, b);
            if (minorAccess(w)) continue;
            ++legs;
            lo = std::min(lo, rank(w));
            hi = std::max(hi, rank(w));
        }
        if (legs < 4 || lo != hi || hi < 1 || hi > 2) continue;
        for (const Branch& a : c.net.arrivals(v)) {
            const RoadNet::Way& w = wayOf(c, a);
            if (!a.legal || minorAccess(w)) continue;
            add(out, c.signBefore(v, a, 4.0, 2.0), "US:R1-1,US:R1-3P");
        }
    }
}

// Every roundabout entry yields to the ring (MUTCD 2B.09).
void roundaboutYield(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        if (!onRing(c, v)) continue;
        for (const Branch& a : c.net.arrivals(v)) {
            const RoadNet::Way& w = wayOf(c, a);
            if (!a.legal || w.roundabout || w.highway == "service") continue;
            add(out, c.signBefore(v, a, 4.0, 2.0), "US:R1-2");
        }
    }
}

// A speed limit is posted where it changes (MUTCD 2B.13), in mph.
void speedLimits(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        const std::vector<Branch> arrivals = c.net.arrivals(v);
        for (const Branch& b : c.net.branches(v)) {
            const RoadNet::Way& w = wayOf(c, b);
            if (!b.legal || w.roundabout || w.link || w.highway == "service") continue;
            const int limit = mph(c.net.maxspeed(b));
            if (limit < 15 || limit > 75) continue;
            bool changes = false;
            for (const Branch& a : arrivals) {
                if (!a.legal || (a.way == b.way && a.index == b.index && a.step == b.step)) continue;
                const int before = mph(c.net.maxspeed(reversed(a)));
                changes |= before > 0 && before != limit;
            }
            if (!changes) continue;
            const std::string code = "US:R2-1[" + std::to_string(limit) + "]";
            if (!palette().signs.count(code)) continue;
            add(out, c.signAfter(v, b, c.net.isJunction(v) ? 30.0 : 15.0, 10.0), code);
        }
    }
}

// Where a one-way street ends at a crossing, DO NOT ENTER faces whoever would
// turn into it the wrong way (MUTCD 2B.37).
void doNotEnter(const Context& c, const Rule&, std::vector<Prediction>& out) {
    const auto& ways = c.net.ways();
    for (int wi = 0; wi < int(ways.size()); ++wi) {
        const RoadNet::Way& w = ways[size_t(wi)];
        if (w.direction == 0 || w.roundabout || w.link || w.highway == "motorway" || w.highway == "trunk") continue;
        const int last = int(w.vertices.size()) - 1;
        const int endIndex = w.direction > 0 ? last : 0;
        const int end = w.vertices[size_t(endIndex)];
        if (!c.net.isJunction(end)) continue;
        // Into the street from its exit end: against its traffic.
        const Branch wrongWay{wi, endIndex, w.direction > 0 ? -1 : 1, false};
        add(out, c.signAfter(end, wrongWay, 4.0, 2.0), "US:R5-1");
    }
}

// The names on a street-name blade, as American blades write them: capitals,
// the suffix shortened as the Postal Service does (MUTCD 2D.43).
std::string label(const std::string& name) {
    static const std::map<std::string, std::string> suffix = {
        {"STREET", "ST"}, {"AVENUE", "AVE"}, {"BOULEVARD", "BLVD"}, {"ROAD", "RD"}, {"DRIVE", "DR"},
        {"LANE", "LN"}, {"PLACE", "PL"}, {"COURT", "CT"}, {"PARKWAY", "PKWY"}, {"HIGHWAY", "HWY"},
        {"TERRACE", "TER"}, {"CIRCLE", "CIR"}, {"SQUARE", "SQ"}, {"EXPRESSWAY", "EXPY"}, {"TRAIL", "TRL"},
        {"WAY", "WAY"}, {"PLAZA", "PLZ"}, {"ALLEY", "ALY"}};
    static const std::map<std::string, std::string> compass = {{"NORTH", "N"}, {"SOUTH", "S"}, {"EAST", "E"}, {"WEST", "W"}};
    std::vector<std::string> words;
    std::string word;
    for (char ch : name + " ") {
        if (ch == ' ') { if (!word.empty()) words.push_back(word); word.clear(); continue; }
        word += (unsigned char)ch < 0x80 ? char(std::toupper((unsigned char)ch)) : ch;
    }
    if (words.empty()) return "";
    if (auto it = suffix.find(words.back()); it != suffix.end() && words.size() > 1) words.back() = it->second;
    if (auto it = compass.find(words.front()); it != compass.end() && words.size() > 2) words.front() = it->second;
    std::string out;
    for (const std::string& w : words) out += (out.empty() ? "" : " ") + w;
    return out;
}

// At a crossing of two named streets, one post at a corner carries both
// names, each blade along its street (MUTCD 2D.43). The names are OSM's.
void streetNames(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        if (!c.net.isJunction(v) || onRing(c, v)) continue;
        struct Leg { std::string name; int rank; P2 along; double half; int64_t id; };
        std::vector<Leg> legs;
        const P2 at = c.net.vertices()[size_t(v)].xz;
        for (const Branch& b : c.net.branches(v)) {
            const RoadNet::Way& w = wayOf(c, b);
            const std::string name = tagOr(w.osm->tags, "name");
            if (name.empty() || minorAccess(w)) continue;
            const auto walk = c.net.walk(v, b, 1.0);
            legs.push_back({name, rank(w), walk.dir, roadWidth(w.osm->tags) / 2, w.osm->id});
        }
        std::stable_sort(legs.begin(), legs.end(), [](const Leg& a, const Leg& b) {
            return a.rank != b.rank ? a.rank > b.rank : a.name < b.name;
        });
        const Leg* first = legs.empty() ? nullptr : &legs.front();
        const Leg* second = nullptr;
        for (const Leg& l : legs)
            if (first && l.name != first->name && std::abs(l.along.x * first->along.y - l.along.y * first->along.x) > 0.5) {
                second = &l;
                break;
            }
        if (!second) continue;
        // The corner between the two streets, clear of both carriageways.
        Prediction p;
        p.xz = {at.x + first->along.x * (second->half + 2.2) + second->along.x * (first->half + 2.2),
                at.y + first->along.y * (second->half + 2.2) + second->along.y * (first->half + 2.2)};
        p.travel = first->along;
        p.urban = true;
        p.evidence = first->id;
        p.what = "US:D3-1";
        // Blades are rationed per tile (gen/predict.cpp): the busier the
        // crossing, the surer its blades are wanted, and they go first.
        p.confidence = 0.6 + 0.02 * (first->rank + second->rank);
        p.detail = {{"names", {label(first->name), label(second->name)}},
                    {"along", {{first->along.x, first->along.y}, {second->along.x, second->along.y}}}};
        out.push_back(std::move(p));
    }
}
}  // namespace

std::vector<Rule> unitedStatesRules() {
    return {
        {"us.roundabout.yield", "every roundabout entry yields to the ring: R1-2 at the entry (MUTCD 2B.09)", 0.95,
         roundaboutYield},
        {"us.one_way.do_not_enter", "a one-way street's exit end faces wrong-way traffic with R5-1 (MUTCD 2B.37)",
         0.90, doNotEnter},
        {"us.stop.minor", "a minor road meeting a major one without lights stops: R1-1 (MUTCD 2B.05)", 0.80,
         twoWayStops},
        {"us.speed.limit", "a speed limit is posted where it changes, in mph: R2-1 (MUTCD 2B.13)", 0.75, speedLimits},
        {"us.street.names", "a crossing of named streets has its name blades on a corner post (MUTCD 2D.43)", 0.70,
         streetNames},
        {"us.stop.all_way", "two equal residential streets crossing without lights: all-way stop, R1-1 and R1-3P",
         0.60, allWayStops},
    };
}

}  // namespace r1
