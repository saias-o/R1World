#include "roadnet.hpp"

#include "streets.hpp"

#include <algorithm>
#include <cstdlib>

namespace r1 {

namespace {
bool drivable(const std::string& highway) {
    if (isMotorway(highway)) return true;
    static const char* links[] = {"motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link"};
    for (const char* l : links) if (highway == l) return true;
    return false;
}
// An access, not an intersection: a car park, a yard, a drive.
bool minor(const std::string& highway) { return highway == "service"; }
}  // namespace

double speedKmh(const std::string* value) {
    if (!value) return 0;
    std::string text;
    for (char c : *value) text += char(std::tolower((unsigned char)c));
    auto trim = [&] {
        while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
        while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
    };
    trim();
    double unit = 1.0;
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, "mph") == 0) { text.resize(text.size() - 3); unit = 1.609344; }
    else if (text.size() >= 4 && text.compare(text.size() - 4, 4, "km/h") == 0) text.resize(text.size() - 4);
    trim();
    if (text.empty()) return 0;
    char* end = nullptr;
    const double n = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size() || !std::isfinite(n) || n <= 0 || n > 200) return 0;
    return n * unit;
}

int roadRank(const std::string& highway) {
    std::string h = highway;
    if (h.size() > 5 && h.compare(h.size() - 5, 5, "_link") == 0) h.resize(h.size() - 5);
    if (h == "motorway") return 6;
    if (h == "trunk") return 5;
    if (h == "primary") return 4;
    if (h == "secondary") return 3;
    if (h == "tertiary") return 2;
    return 1;
}

RoadNet::RoadNet(const std::vector<OsmWay>& roads, const Anchor& anchor, const Bounds& b, double margin)
    : anchor_(anchor) {
    const double mLon = margin / std::max(0.05, std::cos(radians((b.south + b.north) / 2)));
    for (const OsmWay& road : roads) {
        const std::string highway = tagOr(road.tags, "highway");
        if (!drivable(highway) || tagOr(road.tags, "area") == "yes") continue;
        double w = 1e300, e = -1e300, s = 1e300, n = -1e300;
        for (const P2& p : road.points) { w = std::min(w, p.x); e = std::max(e, p.x); s = std::min(s, p.y); n = std::max(n, p.y); }
        if (e < b.west - mLon || w > b.east + mLon || n < b.south - margin || s > b.north + margin) continue;
        Way way;
        way.osm = &road;
        way.highway = highway;
        std::string one;
        for (char c : tagOr(road.tags, "oneway")) one += char(std::tolower((unsigned char)c));
        const std::string junction = tagOr(road.tags, "junction");
        way.roundabout = junction == "roundabout" || junction == "circular";
        if (one == "yes" || one == "true" || one == "1") way.direction = 1;
        else if (one == "-1" || one == "reverse") way.direction = -1;
        else if (one.empty() && (way.roundabout || highway == "motorway" || highway == "motorway_link")) way.direction = 1;
        way.bridge = taggedYes(road.tags, "bridge");
        way.tunnel = taggedYes(road.tags, "tunnel");
        way.link = highway.size() > 5 && highway.compare(highway.size() - 5, 5, "_link") == 0;
        way.layer = std::atoi(tagOr(road.tags, "layer", "0").c_str());
        const int wayIndex = int(ways_.size());
        double along = 0;
        for (size_t i = 0; i < road.points.size(); ++i) {
            const P2 p = road.points[i];
            auto it = index_.find(p);
            int v;
            if (it == index_.end()) {
                const P3 e3 = anchor.toEngine(p.x, p.y, 0.0);
                v = int(vertices_.size());
                vertices_.push_back({p, {e3.x, e3.z}, {}});
                index_[p] = v;
            } else v = it->second;
            if (i) along += dist(vertices_[size_t(way.vertices.back())].xz, vertices_[size_t(v)].xz);
            way.vertices.push_back(v);
            way.along.push_back(along);
            vertices_[size_t(v)].uses.push_back({wayIndex, int(i)});
        }
        ways_.push_back(std::move(way));
    }
}

int RoadNet::vertexAt(P2 lonlat) const {
    auto it = index_.find(lonlat);
    return it == index_.end() ? -1 : it->second;
}

std::vector<RoadNet::Branch> RoadNet::branches(int vertex) const {
    std::vector<Branch> out;
    for (const auto& [w, i] : vertices_[size_t(vertex)].uses) {
        const Way& way = ways_[size_t(w)];
        if (i + 1 < int(way.vertices.size())) out.push_back({w, i, 1, way.direction >= 0});
        if (i > 0) out.push_back({w, i, -1, way.direction <= 0});
    }
    return out;
}

std::vector<RoadNet::Branch> RoadNet::arrivals(int vertex) const {
    std::vector<Branch> out = branches(vertex);
    for (Branch& b : out) b.legal = b.step > 0 ? ways_[size_t(b.way)].direction <= 0 : ways_[size_t(b.way)].direction >= 0;
    return out;
}

P2 RoadNet::heading(const Branch& b) const {
    const Way& way = ways_[size_t(b.way)];
    const P2 a = vertices_[size_t(way.vertices[size_t(b.index)])].xz;
    const P2 c = vertices_[size_t(way.vertices[size_t(b.index + b.step)])].xz;
    const double length = dist(a, c);
    return length > 1e-9 ? P2{(c.x - a.x) / length, (c.y - a.y) / length} : P2{0, 0};
}

// Where a car travelling `dir` into `at` along `from` may go on: never back
// the way it came nor into a hairpin (the other flare of a split exit, the
// other carriageway), never into an access from a road, and only the way
// traffic runs -- or, walking `against` it, only the way it comes from.
std::vector<RoadNet::Branch> RoadNet::onwards(int at, const Branch& from, P2 dir, bool against) const {
    const Way& way = ways_[size_t(from.way)];
    std::vector<Branch> out;
    for (const Branch& o : branches(at)) {
        if (o.way == from.way && o.index == from.index + from.step && o.step == -from.step) continue;
        const Way& next = ways_[size_t(o.way)];
        if (minor(next.highway) && !minor(way.highway)) continue;
        const bool legal = against ? (o.step > 0 ? next.direction <= 0 : next.direction >= 0) : o.legal;
        if (!legal) continue;
        const P2 h = heading(o);
        if (h.x * dir.x + h.y * dir.y < -0.5) continue;
        out.push_back(o);
    }
    return out;
}

// An intersection is where a driver chooses: some way into the vertex offers
// two ways on. Where two flares of a roundabout exit merge back into one road,
// nobody chooses anything, and the road simply goes on.
bool RoadNet::isJunction(int vertex) const {
    for (const Branch& a : arrivals(vertex)) {
        if (!a.legal || minor(ways_[size_t(a.way)].highway)) continue;
        const P2 h = heading(a);
        // The arrival as the branch that brought the car here.
        const Branch from{a.way, a.index + a.step, -a.step, true};
        if (onwards(vertex, from, {-h.x, -h.y}, false).size() >= 2) return true;
    }
    return false;
}

RoadNet::Walk RoadNet::walk(int vertex, const Branch& start, double metres, bool against) const {
    Branch b = start;
    int at = vertex;
    double walked = 0;
    Walk out;
    for (int guard = 0; guard < 10000; ++guard) {
        const Way& way = ways_[size_t(b.way)];
        const int j = b.index + b.step;
        const P2 a = vertices_[size_t(way.vertices[size_t(b.index)])].xz;
        const int next = way.vertices[size_t(j)];
        const P2 c = vertices_[size_t(next)].xz;
        const double length = dist(a, c);
        const P2 dir = length > 1e-9 ? P2{(c.x - a.x) / length, (c.y - a.y) / length} : P2{0, 0};
        out.way = b.way;
        if (length > 1e-9) out.dir = dir;
        if (walked + length >= metres) {
            const double t = length > 1e-9 ? (metres - walked) / length : 0;
            out.xz = {a.x + (c.x - a.x) * t, a.y + (c.y - a.y) * t};
            out.reached = metres;
            return out;
        }
        walked += length;
        at = next;
        out.xz = c;
        out.reached = walked;
        // The road goes on where there is exactly one way on; a junction or
        // a dead end stops the walk.
        const std::vector<Branch> on = onwards(at, b, out.dir, against);
        if (on.size() != 1) { out.stopVertex = at; return out; }
        b = on.front();
    }
    out.stopVertex = at;
    return out;
}

std::pair<int, double> RoadNet::reach(int vertex, const Branch& branch, double limit) const {
    const Walk w = walk(vertex, branch, limit, false);
    return {w.stopVertex, w.reached};
}

const std::string* RoadNet::maxspeed(const Branch& b) const {
    const Tags& t = ways_[size_t(b.way)].osm->tags;
    if (const std::string* directed = tag(t, b.step > 0 ? "maxspeed:forward" : "maxspeed:backward")) return directed;
    return tag(t, "maxspeed");
}

}  // namespace r1
