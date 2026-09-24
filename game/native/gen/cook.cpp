#include "cook.hpp"

#include "buildings.hpp"
#include "harbours.hpp"
#include "landmarks.hpp"
#include "scatter.hpp"
#include "streets.hpp"
#include "terrain.hpp"

#include <chrono>
#include <stdexcept>

namespace r1 {

namespace {
// Buildings are the full chain with its most expensive stage off: a negative
// radius means no facade is near, so walls are textured rather than opened,
// and roofs lose their fascia and soffit: a city tile with modelled openings
// exhausts the vertex arena on its own (CLAUDE.md §5), and openings are rank
// 11 of the fidelity hierarchy, the first to give way.
constexpr double kWorldDetailRadius = -1.0;
constexpr double kWorldRoofThickness = 0.0;
// One water cell per terrain quad, so the bitmap and the picture agree.
constexpr int kWaterGrid = kTerrainMeshSize - 1;

// Complete Overpass ways cut to the tile before elevation sampling, one
// two-point way per surviving segment (Liang–Barsky).
std::vector<OsmWay> clipRoads(const std::vector<OsmWay>& roads, const Bounds& b) {
    std::vector<OsmWay> out;
    for (const OsmWay& road : roads)
        for (size_t i = 0; i + 1 < road.points.size(); ++i) {
            const P2 a = road.points[i], c = road.points[i + 1];
            const double dx = c.x - a.x, dy = c.y - a.y;
            double lo = 0, hi = 1;
            bool valid = true;
            const double pq[4][2] = {{-dx, a.x - b.west}, {dx, b.east - a.x}, {-dy, a.y - b.south}, {dy, b.north - a.y}};
            for (const auto& [p, q] : pq) {
                if (std::abs(p) < 1e-15) {
                    if (q < 0) { valid = false; break; }
                } else if (p < 0) lo = std::max(lo, q / p);
                else hi = std::min(hi, q / p);
            }
            if (valid && lo < hi)
                out.push_back({road.id, {{a.x + lo * dx, a.y + lo * dy}, {a.x + hi * dx, a.y + hi * dy}}, road.tags});
        }
    return out;
}

size_t vertexCount(const std::vector<MeshPart>& parts) {
    size_t n = 0;
    for (const MeshPart& p : parts) n += p.mesh.vertexCount();
    return n;
}
}  // namespace

CookedTile cookTile(const Observations& in) {
    const auto started = std::chrono::steady_clock::now();
    const Tile tile = in.tile;
    const Bounds bounds = tile.bounds();
    const P2 center = tile.center();
    const OsmData& osm = *in.osm;
    const ElevationGrid& elevations = in.elevations;
    for (double h : elevations.values)
        if (!std::isfinite(h)) throw std::runtime_error("Elevation source returned non-finite data");
    const Anchor anchor = Anchor::at(center.x, center.y, 0);
    const GroundAt ground = [&](double lon, double lat) { return groundPoint(lon, lat, elevations, anchor); };
    const auto elevationAt = [&](double lon, double lat) { return elevations.sample(lon, lat); };

    // Each building belongs to exactly one tile, the one its first point is in.
    std::vector<const OsmWay*> buildings;
    for (const OsmWay& w : osm.buildings) if (tileAt(w.points[0].x, w.points[0].y) == tile) buildings.push_back(&w);
    // A lighthouse is a tower on its node, so the building OSM traced around
    // it is not extruded a second time; one traced with no node becomes one.
    std::vector<OsmNode> lightFeatures = osm.features;
    for (const OsmWay* w : buildings) {
        if (!isLighthouse(w->tags)) continue;
        bool hasNode = false;
        for (const OsmNode& n : osm.features)
            hasNode |= tagOr(n.tags, "man_made") == "lighthouse" && pointInPolygon({n.lon, n.lat}, w->points);
        if (!hasNode) lightFeatures.push_back(tracedLighthouse(*w));
    }
    std::vector<P2> lights;
    for (const OsmNode& n : lightFeatures) if (tagOr(n.tags, "man_made") == "lighthouse") lights.push_back({n.lon, n.lat});
    if (!lights.empty()) {
        std::vector<const OsmWay*> kept;
        for (const OsmWay* w : buildings) {
            if (tagOr(w->tags, "building") == "lighthouse" || tagOr(w->tags, "man_made") == "lighthouse") continue;
            bool covers = false;
            for (const P2& p : lights) covers |= pointInPolygon(p, w->points);
            if (!covers) kept.push_back(w);
        }
        buildings = std::move(kept);
    }
    // The twenty places an extrusion cannot draw: their own trace is not
    // extruded, and their model stands in its place.
    LandmarkPlacement landmarks = placeLandmarks(bounds, ground, buildings);
    buildings = landmarks.kept;

    const RegionProfile& profile = profileFor(center.x, center.y);
    const std::string climate = climateAt(profile.climate, center.y);
    BuildingOutput built = buildBuildings(
        buildings, ground, profile, {0.0, 0.0}, kWorldDetailRadius, kWorldRoofThickness,
        [](const Swatch& s, bool doubleSided) { return surfaceMaterial(s.name, s.color, s.roughness, wallFamily(s.name), doubleSided); },
        [](const Swatch& s, bool doubleSided) { return surfaceMaterial(s.name, s.color, s.roughness, roofFamily(s.name), doubleSided); });
    std::vector<Ring> footprints = built.footprints;
    footprints.insert(footprints.end(), landmarks.solids.begin(), landmarks.solids.end());

    // Rank 9: the terrain partitioned by what OSM says the ground is.
    const Landcover landcover(osm.landcover);
    HarbourStats harbour;
    std::optional<Sea> sea;
    if (in.offline) {
        sea = offlineSea(bounds);
        harbour.coastline = "Natural Earth 1:110m approximation";
    } else {
        sea = seaGeometry(osm.coastlines, bounds, elevationAt, harbour.coastline);
    }
    const auto tidal = tidalWater(osm.landcover, osm.maritime);
    const Cells cells(bounds, kWaterGrid, sea ? &*sea : nullptr, landcover, elevationAt, tidal ? &*tidal : nullptr);
    harbour.seaCells = cells.seaCells();

    auto classify = [&](double x, double y) -> std::string {
        if (cells.seaAt(x, y)) return "water";
        const std::string* found = landcover.at(x, y);
        const std::string name = found ? *found : kInferred;
        if (name == "water") return name;
        return name + coldSuffix(y, elevations.sample(x, y), climate);
    };
    auto terrain = buildTerrain(bounds, elevations, anchor, classify,
                                [&](int r, int c, double h) { return cells.adjust(r, c, h); });
    std::vector<MeshPart> terrainParts;
    std::map<std::string, int> groundStats;
    for (auto& [name, mesh] : terrain) {
        const Swatch& swatch = groundSwatch(name, profile);
        groundStats[name] = int(mesh.indices.size() / 3);
        terrainParts.push_back({"Ground \xE2\x80\x94 " + swatch.name, smoothSurface(mesh),
                                surfaceMaterial(swatch.name, swatch.color, swatch.roughness,
                                                groundFamily(name, profile.ground.name, climate))});
    }
    const std::vector<OsmWay> roads = clipRoads(osm.roads, bounds);
    StreetOutput streets = buildStreets(roads, osm.features, elevations, anchor, footprints);
    // Road centre lines in engine metres, so a bench faces its street.
    std::vector<Segment2> roadSegments;
    for (const OsmWay& w : roads) {
        const P3 a = ground(w.points.front().x, w.points.front().y), b = ground(w.points.back().x, w.points.back().y);
        roadSegments.push_back({{a.x, a.z}, {b.x, b.z}});
    }
    nlohmann::json laneGraph = buildLaneGraph(roads, ground, int(buildings.size()), center.x, center.y);
    Works works = buildWorks(osm.maritime, lightFeatures, ground, anchor, cells, harbour);

    auto assemble = [&](std::vector<MeshPart>& harbourParts) {
        std::vector<MeshPart> parts;
        for (auto* list : {&terrainParts, &streets.parts, &harbourParts, &built.parts})
            for (MeshPart& p : *list) parts.push_back(p);
        return parts;
    };
    std::vector<MeshPart> parts = assemble(works.parts);
    if (vertexCount(parts) > kTileVertexBudget && harbour.piers) {
        // The one detail given up before a tile is refused: the piles under
        // the piers, said in the manifest (`pilesDropped`), never silent.
        HarbourStats bare;
        works = buildWorks(osm.maritime, lightFeatures, ground, anchor, cells, bare, false);
        parts = assemble(works.parts);
        harbour.pilesDropped = true;
    }
    const size_t vertices = vertexCount(parts);
    if (vertices > kTileVertexBudget) {
        std::string detail;
        for (const MeshPart& p : parts) detail += " " + p.name + "=" + std::to_string(p.mesh.vertexCount());
        throw std::runtime_error("Tile exceeds geometry budget (" + std::to_string(vertices) + " vertices):" + detail);
    }
    const bool ocean = in.offline
        ? (sea && sea->area() >= (bounds.east - bounds.west) * (bounds.north - bounds.south) * (1 - 1e-9))
        : [&] {
              if (!buildings.empty() || !osm.roads.empty()) return false;
              for (double h : elevations.values) if (std::abs(h) >= 0.01) return false;
              return true;
          }();
    int triangles = 0, inferred = 0;
    for (const auto& [name, n] : groundStats) {
        triangles += n;
        if (name.substr(0, name.find('@')) == kInferred) inferred += n;
    }
    const double measuredGround = 1.0 - double(inferred) / std::max(1, triangles);

    CookedTile out;
    out.tile = tile;
    nlohmann::json boats = nlohmann::json::array();
    Scatter props, nature;
    if (ocean) {
        out.ocean = seaNode(bounds, anchor, "Ocean");
        out.ocean["amplitude"] = 0.05;
        out.ocean["wavelength"] = 12.0;
        props.stats = {{"placed", 0}, {"droppedForBudget", 0}, {"byKind", nlohmann::json::object()}};
        nature.stats = {{"revision", 2}, {"placed", 0}};
    } else {
        out.parts = std::move(parts);
        std::vector<const OsmNode*> inTile;
        for (const OsmNode& f : osm.features)
            if (bounds.west <= f.lon && f.lon <= bounds.east && bounds.south <= f.lat && f.lat <= bounds.north)
                inTile.push_back(&f);
        props = planProps(inTile, ground, profile, roadSegments);
        nature = planNature(osm, tile, anchor, ground);
        // The landmark first after the ground: it is what the player came to
        // see, and the game swaps its far model out once it has streamed.
        for (auto& n : landmarks.nodes) out.props.push_back(n);
        for (auto& n : nature.nodes) out.props.push_back(n);
        for (auto& n : props.nodes) out.props.push_back(n);
        if (cells.hasSea()) out.props.push_back(seaNode(bounds, anchor));
        const auto berths = planBoats(osm, anchor, cells, profile, climate, harbour);
        auto [boatChildren, manifest] = boatNodes(berths, anchor);
        boats = manifest;
        for (auto& n : boatChildren) out.props.push_back(n);
        for (auto& n : planContainers(osm, anchor, cells, footprints, ground, harbour)) out.props.push_back(n);
    }
    nlohmann::json footprintJson = nlohmann::json::array();
    for (const Ring& r : footprints) {
        nlohmann::json ring = nlohmann::json::array();
        for (const P2& p : r) ring.push_back({p.x, p.y});
        footprintJson.push_back(ring);
    }
    nlohmann::json elevationRows = nlohmann::json::array();
    for (int r = 0; r < elevations.size; ++r) {
        nlohmann::json row = nlohmann::json::array();
        for (int c = 0; c < elevations.size; ++c) row.push_back(elevations.at(r, c));
        elevationRows.push_back(row);
    }
    if (ocean) {
        laneGraph = {{"nodes", nlohmann::json::array()}, {"lanes", nlohmann::json::array()}, {"cars", 0},
                     {"leftHand", laneGraph["leftHand"]}};
    }
    out.manifest = {
        {"key", tile.key()}, {"row", tile.row}, {"col", tile.col}, {"lon", center.x}, {"lat", center.y},
        {"bounds", {{"south", bounds.south}, {"west", bounds.west}, {"north", bounds.north}, {"east", bounds.east}}},
        {"elevations", elevationRows}, {"footprints", footprintJson},
        // The landmark models are nodes, not tile geometry, but they share
        // the arena, so the residency count includes them.
        {"vertices", ocean ? 0 : vertices + landmarks.vertices},
        {"buildings", buildings.size()}, {"surface", ocean ? "ocean" : "land"},
        {"source", std::string(in.offline ? "Natural Earth 1:110m; " : "OpenStreetMap; ") + in.elevationSource},
        {"elevationSource", in.elevationSource}, {"offlineApproximation", in.offline},
        {"region", profile.name}, {"regionTier", profile.tier}, {"climate", climate},
        {"osmQueryVersion", osm.queryVersion},
        {"ground", {{"measuredFraction", pyround(measuredGround, 4)}, {"trianglesByClass", groundStats}}},
        {"water", cells.rows()}, {"decks", works.decks}, {"boats", boats}, {"harbour", harbour.json()},
        {"props", props.stats}, {"landmarks", ocean ? nlohmann::json::array() : landmarks.manifest},
        {"landmarkRevision", kLandmarkRevision},
        {"landmarkReplacedWays", ocean ? nlohmann::json::array() : landmarks.replaced},
        {"nature", nature.stats}, {"streets", streets.stats}, {"traffic", laneGraph},
        {"inference", built.stats.json()},
        {"generator", "C++"}};
    out.cookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return out;
}

}  // namespace r1
