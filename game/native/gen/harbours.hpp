// Harbours: the sea, what is built into it, and the boats moored there.
// The sea is rebuilt from the coastline (land on its left); the works are
// what OSM mapped; the boats are inferred, and say so.
#pragma once

#include "clip.hpp"
#include "palette.hpp"

#include <functional>

namespace r1 {

// The sea inside a tile, in (lon, lat), and how it was decided.
struct Sea {
    clip::Paths64 region;  // on the degrees grid
    bool contains(double lon, double lat) const { return clip::contains(region, P2{lon, lat}, clip::kDegrees); }
    double area() const { return clip::area(region, clip::kDegrees); }
};

std::optional<Sea> seaGeometry(const std::vector<OsmWay>& coastlines, const Bounds& bounds,
                               const std::function<double(double, double)>& elevationAt, std::string& how);
// Natural Earth's coarse sea, for a tile cooked without observations.
std::optional<Sea> offlineSea(const Bounds& bounds);
// Mapped water that is the sea's though no coastline crosses the tile.
std::optional<clip::Paths64> tidalWater(const std::vector<OsmWay>& landcover, const std::vector<OsmWay>& maritime);

// The tile's cells: 0 land, 1 inland water, 2 sea-level water. The same grid
// the terrain is partitioned on and the game walks and sails on.
class Cells {
public:
    Cells(const Bounds& bounds, int size, const Sea* sea, const class Landcover& landcover,
          const std::function<double(double, double)>& elevationAt, const clip::Paths64* tidal,
          const std::function<bool(double, double)>& inlandAt = {});
    int at(double lon, double lat) const;
    double adjust(int row, int col, double height) const;
    std::vector<std::string> rows() const;
    bool hasSea() const { return hasSea_; }
    int seaCells() const;
    const Bounds& bounds() const { return bounds_; }
    int size() const { return size_; }

private:
    Bounds bounds_;
    int size_;
    const Sea* sea_;
    std::vector<std::vector<int>> codes_;
    bool hasSea_ = false;
};

struct HarbourStats {
    std::string coastline = "no coastline";
    int seaCells = 0, piers = 0, breakwaters = 0, quays = 0, lighthouses = 0, containers = 0, berthsRefused = 0;
    bool pilesDropped = false;
    std::map<std::string, int> boats;
    nlohmann::json json() const;
};

using GroundFn = std::function<P3(double, double)>;

bool isLighthouse(const Tags& tags);
// A lighthouse traced as a footprint, as the node its tower stands on.
OsmNode tracedLighthouse(const OsmWay& way);

struct Works {
    std::vector<MeshPart> parts;
    nlohmann::json decks = nlohmann::json::array();
};
Works buildWorks(const std::vector<OsmWay>& maritime, const std::vector<OsmNode>& features, const GroundFn& ground,
                 const Anchor& anchor, const Cells& cells, HarbourStats& stats, bool piles = true);

struct Berth {
    const BoatKind* kind;
    double x, z;
    P2 axis;
    double beam;
};
std::vector<Berth> planBoats(const OsmData& osm, const Anchor& anchor, const Cells& cells,
                             const RegionProfile& profile, const std::string& climate, HarbourStats& stats);
// Scene nodes for the berths, and the manifest entries the game boards them by.
std::pair<nlohmann::json, nlohmann::json> boatNodes(const std::vector<Berth>& berths, const Anchor& anchor);
nlohmann::json planContainers(const OsmData& osm, const Anchor& anchor, const Cells& cells,
                              const std::vector<Ring>& footprints, const GroundFn& ground, HarbourStats& stats);
// The animated water over one tile, and no further.
nlohmann::json seaNode(const Bounds& bounds, const Anchor& anchor, const std::string& name = "Sea");

// A glTF model's position bounds (from its accessors, no vertex data read).
struct ModelBounds { P3 low, high; };
const ModelBounds& modelBounds(const std::string& projectPath);
// A hull's beam: its kind's length ratio, or the model's own proportions.
double beamOf(const BoatKind& kind);

}  // namespace r1
