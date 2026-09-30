// The building chain (plan §10): footprint from OSM, height and
// roof measured where tagged and drawn from the region where not, facade
// synthesised, and every inference counted for the manifest (PLAN §3 I5).
#pragma once

#include "palette.hpp"
#include "polygons.hpp"
#include "interiors.hpp"

#include <functional>

namespace r1 {

struct BuildingStats {
    int total = 0, rejected = 0, heightMeasured = 0, heightInferred = 0, roofTagged = 0, roofInferred = 0;
    int roofFlattened = 0, detailed = 0, partyWalls = 0, steeples = 0;
    std::map<std::string, int> shapes;
    nlohmann::json json() const;
};

using MaterialFor = std::function<Material(const Swatch&, bool doubleSided)>;

// The tile cook can step down building geometry before refusing a dense tile.
enum class BuildingLod { Full, UnifiedBase, SimpleRoofline };

struct BuildingOutput {
    std::vector<MeshPart> parts;
    std::vector<Ring> footprints;  // engine (x, z)
    // The highest point over each footprint, engine y: what an aircraft
    // clears or stops against, and what a helicopter lands on.
    std::vector<double> tops;
    std::vector<InteriorPlan> interiors;
    BuildingStats stats;
    // Open roofs (gen/fuel): their fuel canopies, and the canopy lettering.
    nlohmann::json fuelStations = nlohmann::json::array();
    nlohmann::json lettering = nlohmann::json::array();
    int openRoofs = 0;
};

// `groundOf(lon, lat)` is the engine point on the terrain. A negative
// `detailRadius` means no building is near: plain walls, no modelled openings.
BuildingOutput buildBuildings(const std::vector<const OsmWay*>& ways,
                              const std::function<P3(double, double)>& groundOf,
                              const RegionProfile& profile, P2 detailCenter, double detailRadius,
                              double roofThickness, const MaterialFor& wallMaterials,
                              const MaterialFor& roofMaterials, BuildingLod lod = BuildingLod::Full);

// Metres from an OSM length tag, honouring feet; nullopt when it has none.
std::optional<double> taggedLength(const std::string& raw);

// ── the chain's steps, exposed for the tests ────────────────────────────────

// The footprint's own rectangle: `u` is the long axis a ridge runs along.
struct OrientedBox {
    double cx = 0, cz = 0, ux = 1, uz = 0, halfU = 0.5, halfV = 0.5;
    double vx() const { return -uz; }
    double vz() const { return ux; }
    P2 point(double a, double b) const { return {cx + ux * a + vx() * b, cz + uz * a + vz() * b}; }
    double area() const { return 4.0 * halfU * halfV; }
};

// Weld, simplify, rectify and orient a raw ring (positive area), or reject it.
std::optional<Ring> cleanFootprint(const Ring& ring);
// Minimum-area enclosing rectangle, by rotating calipers over the hull.
OrientedBox orientedBox(const Ring& ring);

// A building's vertical dimensions, and where each of them came from.
struct Gabarit {
    double wallHeight = 0, roofHeight = 0;
    int storeys = 1;
    std::string heightSource, roofShape, roofSource;
};

// How tall, and what is on top: measured where tagged, the region elsewhere.
Gabarit planGabarit(const Tags& tags, int64_t osmId, const RegionProfile& profile, const OrientedBox& box,
                    double rectangularity, bool commercial = false);

}  // namespace r1
