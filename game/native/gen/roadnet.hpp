// The road network as a graph: which ways meet where, in which direction a
// car may travel them, and how far it is from one point to the next.
//
// OSM draws roads as ways; a junction is a node two ways share. Overpass
// gives every node of a way the exact same coordinates in every way that
// uses it, so the graph is welded on those coordinates, never on a
// tolerance: two roads drawn across each other without a shared node do not
// meet, which is what a bridge looks like (gen/predict.cpp).
#pragma once

#include "osm.hpp"

namespace r1 {

// A speed limit in km/h from an OSM `maxspeed` value, or 0 when it is not a
// number ("none", "signals", "FR:urban").
double speedKmh(const std::string* value);
// How important a road is, for "which passes over": motorway 6 ... others 1.
int roadRank(const std::string& highway);

class RoadNet {
public:
    struct Way {
        const OsmWay* osm = nullptr;
        std::string highway;
        int direction = 0;  // 1 only forwards, -1 only backwards, 0 both
        bool roundabout = false, bridge = false, tunnel = false, link = false;
        int layer = 0;
        std::vector<int> vertices;
        std::vector<double> along;  // metres from the first vertex
    };
    struct Vertex {
        P2 lonlat, xz;  // xz: engine metres (x east, z south)
        std::vector<std::pair<int, int>> uses;  // (way, index in the way)
    };
    // Leaving `vertex` along `way` by `step` (+1 towards its end, -1 its start).
    struct Branch {
        int way = -1, index = 0, step = 1;
        bool legal = true;  // a car may drive it in this direction
    };
    // Where a walk along the road ended.
    struct Walk {
        P2 xz, dir;          // the point and the direction of travel there
        double reached = 0;  // metres walked, <= asked
        int stopVertex = -1; // the junction or dead end that stopped it, or -1
        int way = -1;        // the way the point lies on
    };

    // Every road a car drives on (and its links) whose box comes within
    // `margin` degrees of `bounds`.
    RoadNet(const std::vector<OsmWay>& roads, const Anchor& anchor, const Bounds& bounds, double margin);

    const std::vector<Way>& ways() const { return ways_; }
    const std::vector<Vertex>& vertices() const { return vertices_; }
    const Anchor& anchor() const { return anchor_; }
    // The vertex at exactly these coordinates, or -1.
    int vertexAt(P2 lonlat) const;

    std::vector<Branch> branches(int vertex) const;
    // The branches that bring a car *into* `vertex`: the reverse of each
    // branch leaving it, legal when driving towards the vertex is.
    std::vector<Branch> arrivals(int vertex) const;
    // An intersection in the highway code's sense: a driver arriving here
    // can choose between two ways on. Service roads are accesses, not
    // choices; the merge of a split roundabout exit is no choice either.
    bool isJunction(int vertex) const;

    // Walk `metres` from the vertex a branch leaves. Carries on where the
    // road simply continues and stops at a junction or a dead end. `against`
    // walks back up the traffic (to stand a sign before a point).
    Walk walk(int vertex, const Branch& branch, double metres, bool against = false) const;
    // The vertex at the far end of a branch's way section: the next junction
    // or dead end reached by walking it, and the distance to it.
    std::pair<int, double> reach(int vertex, const Branch& branch, double limit = 1e9) const;

    // `maxspeed` for travel along a branch, from `maxspeed:forward` or
    // `:backward` when tagged, else `maxspeed`; nullptr when not tagged.
    const std::string* maxspeed(const Branch& b) const;

private:
    P2 heading(const Branch& b) const;
    std::vector<Branch> onwards(int at, const Branch& from, P2 dir, bool against) const;
    Anchor anchor_;
    std::vector<Way> ways_;
    std::vector<Vertex> vertices_;
    std::map<P2, int> index_;
};

}  // namespace r1
