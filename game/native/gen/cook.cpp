#include "cook.hpp"

#include "airports.hpp"
#include "bridges.hpp"
#include "buildings.hpp"
#include "crowd.hpp"
#include "harbours.hpp"
#include "landmarks.hpp"
#include "peaks.hpp"
#include "predict.hpp"
#include "scatter.hpp"
#include "seaice.hpp"
#include "streets.hpp"
#include "terrain.hpp"
#include "waterways.hpp"
#include "retail.hpp"
#include "fuel.hpp"
#include "spatial.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace r1 {
namespace {
// Trees a tile may carry when the canopy is measured (a periurban tile has
// about 500 tree cells, a forest tile 2 304: past this the densest keep a spread).
constexpr int kMeasuredNatureBudget = 640;
}  // namespace


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

clip::Paths64 projectWater(const clip::Paths64& region, const Anchor& anchor) {
    clip::Paths64 out;
    for (const auto& path : region) {
        std::vector<P2> points;
        for (const auto& p : path) {
            const P2 geo = clip::kDegrees.back(p);
            const P3 at = anchor.toEngine(geo.x, geo.y, 0.0);
            points.push_back({at.x, at.z});
        }
        out.push_back(clip::kMetres.path(points));
    }
    return out;
}

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

// How far past the tile a street is still cut for it: a carriageway and its
// pavements that cross the edge, or run along it outside, reach in this far.
constexpr double kStreetReach = 30.0;
Bounds grown(const Bounds& b, double metres) {
    const double dLat = metres / 111320.0;
    const double dLon = metres / (111320.0 * std::max(0.01, std::cos((b.south + b.north) / 2 * kPi / 180.0)));
    return {b.south - dLat, b.west - dLon, b.north + dLat, b.east + dLon};
}

// Where a street crosses a water cell, the street is dry: a cell is 25 m of
// water or not, and a car on the quay must not sink with it. The pieces are
// in the tile's frame, rings in even-odd, read before the water (world.cpp).
nlohmann::json dryStreets(const clip::Paths64& streets, const Cells& cells, const Anchor& anchor) {
    const Bounds& b = cells.bounds();
    const int n = cells.size();
    clip::Paths64 wet;
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col) {
            const double lon0 = b.west + (b.east - b.west) * col / n, lon1 = b.west + (b.east - b.west) * (col + 1) / n;
            const double lat0 = b.south + (b.north - b.south) * row / n, lat1 = b.south + (b.north - b.south) * (row + 1) / n;
            if (cells.at((lon0 + lon1) / 2, (lat0 + lat1) / 2) <= 0) continue;
            std::vector<P2> square;
            for (const P2& c : {P2{lon0, lat0}, P2{lon1, lat0}, P2{lon1, lat1}, P2{lon0, lat1}}) {
                const P3 e = anchor.toEngine(c.x, c.y, 0.0);
                square.push_back({e.x, e.z});
            }
            wet.push_back(clip::kMetres.path(square));
        }
    nlohmann::json out = nlohmann::json::array();
    if (wet.empty() || streets.empty()) return out;
    for (const auto& path : clip::intersect(streets, clip::unite(wet))) {
        nlohmann::json ring = nlohmann::json::array();
        for (const auto& p : path) ring.push_back({double(p.x) / 1000.0, double(p.y) / 1000.0});
        out.push_back(std::move(ring));
    }
    return out;
}

size_t indexCount(const std::vector<MeshPart>& parts) {
    size_t n = 0;
    for (const MeshPart& p : parts) n += p.mesh.indices.size();
    return n;
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
    for (double h : in.elevations.values)
        if (!std::isfinite(h)) throw std::runtime_error("Elevation source returned non-finite data");
    // Measured summits first: every model of the relief rounds them off, and
    // everything below stands on the ground they give (gen/peaks.hpp). The
    // neighbours' relief is raised the same way, so a bridge or a summit near
    // the edge is solved on the same ground from both sides.
    std::vector<const ElevationGrid*> known{&in.elevations};
    for (const ElevationGrid& g : in.around) known.push_back(&g);
    nlohmann::json peakReport;
    const ElevationGrid surveyed = raiseToPeaks(in.elevations, in.peaks, known, &peakReport);
    peakReport["source"] = in.peaksSource;
    std::vector<ElevationGrid> around;
    for (const ElevationGrid& g : in.around) around.push_back(raiseToPeaks(g, in.peaks, known));
    const Anchor anchor = Anchor::at(center.x, center.y, 0);
    // What the maps do not say comes first: the crossings the model predicts
    // are bridges, and bridges decide where the roads leave the ground and
    // where the ground is dug under them. Everything after stands on that.
    const GroundAt surveyedGround = [&](double lon, double lat) { return groundPoint(lon, lat, surveyed, anchor); };
    PredictOutput predicted = predictDetails(osm, tile, anchor, surveyedGround, in.osmExtent);
    const GroundField field(surveyed, around);
    const GradePlan grades = planGrades(osm, predicted.structures, field, anchor, bounds);
    const ElevationGrid dug = grades.carves.empty() ? ElevationGrid{} : carvedGround(surveyed, grades);
    const ElevationGrid& elevations = grades.carves.empty() ? surveyed : dug;
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
    auto retailWays=interiorBuildings(retailBuildings(buildings,osm,ground),osm,ground);
    buildings.clear();for(auto& w:retailWays)buildings.push_back(&w);
    // Fuel stations: each canopy marked, or inferred where none is mapped.
    nlohmann::json fuelManifest=nlohmann::json::array();
    // The country the answer names, else the bundled borders'.
    const std::string fuelCountry=!osm.country.empty()?osm.country:countryAt(center.x,center.y);
    auto fuelWays=fuelCanopies(buildings,osm,tile,fuelCountry,fuelManifest);
    buildings.clear();for(auto& w:fuelWays)buildings.push_back(&w);

    const RegionProfile& profile = profileFor(center.x, center.y);
    const std::string climate = climateAt(profile.climate, center.y);
    const MaterialFor wallMaterial = [](const Swatch& s, bool doubleSided) {
        return surfaceMaterial(s.name, s.color, s.roughness, wallFamily(s.name), doubleSided);
    };
    const MaterialFor roofMaterial = [](const Swatch& s, bool doubleSided) {
        return surfaceMaterial(s.name, s.color, s.roughness, roofFamily(s.name), doubleSided);
    };
    auto buildAtLod = [&](BuildingLod lod) {
        return buildBuildings(buildings, ground, profile, {0.0, 0.0}, kWorldDetailRadius,
                              kWorldRoofThickness, wallMaterial, roofMaterial, lod);
    };
    BuildingLod buildingLod = BuildingLod::Full;
    BuildingOutput built = buildAtLod(buildingLod);
    const Landcover landcover(osm.landcover);
    // What a garage's forecourt bays stay clear of: every building and road
    // of the neighbourhood, buffered, and its water and green. A bay only
    // meets what reaches near it, so each garage unites only that; each piece
    // is buffered once, for every garage and every LOD below.
    const bool garages=std::any_of(built.interiors.begin(),built.interiors.end(),[](const auto& p){return p.recipe=="garage";});
    struct Obstacle { int kind; double buffer; Ring ring; Box box; std::optional<clip::Paths64> paths; };
    std::vector<Obstacle> obstacles;
    std::optional<BoxIndex> obstacleIndex;
    auto forecourtExclusions=[&](const Box& bays) {
        if(!garages)return clip::Paths64{};
        if(!obstacleIndex) {
            obstacleIndex.emplace(64.);
            auto add=[&](const OsmWay& w,int kind,double buffer) {
                Ring ring;for(auto q:w.points){auto x=anchor.toEngine(q.x,q.y,0);ring.push_back({x.x,x.z});}
                const Box box=boxOf(ring).grown(buffer+.01);
                obstacleIndex->add(obstacles.size(),box);obstacles.push_back({kind,buffer,std::move(ring),box,std::nullopt});
            };
            for(const auto& w:osm.buildings)add(w,0,.4);
            for(const auto& road:osm.roads)add(road,1,roadWidth(road.tags)/2+.4);
            for(const auto& w:osm.landcover)if(tagOr(w.tags,"natural")=="water"||has(w.tags,"water")||
                tagOr(w.tags,"leisure")=="park"||tagOr(w.tags,"landuse")=="forest"||tagOr(w.tags,"natural")=="wood")add(w,2,0);
        }
        // Past the bays' reach, a margin. Around the bays this union has the
        // edges a union of the whole neighbourhood has; only where a far piece
        // crosses a near one is a vertex rounded elsewhere, under a millimetre:
        // only a bay grazing an obstacle by a sliver of 0.001 m² could tell.
        const Box reach=bays.grown(5.);
        clip::Paths64 near;
        for(size_t i:obstacleIndex->near(reach)) {
            auto& o=obstacles[i];
            if(!o.box.overlaps(reach))continue;
            if(!o.paths)o.paths=o.kind==0?clip::bufferRing(o.ring,o.buffer):o.kind==1?clip::bufferLine(o.ring,o.buffer):
                clip::Paths64{clip::kMetres.path(o.ring)};
            near.insert(near.end(),o.paths->begin(),o.paths->end());
        }
        return clip::unite(near);
    };
    auto approaches=[&] {
        for(auto& p:built.interiors) {
            const auto a=p.point(0,-4);const auto geo=anchor.toGeodetic(a.x,0,a.y);
            p.approach=ground(geo.x,geo.y).y+.09;
            p.exteriorVehicles.clear();
            if(p.recipe!="garage")continue;
            Box bays;
            for(double u:{-3.5,3.5})for(double v:{-8.,-3.})for(double side:{-1.15,1.15})bays.add(p.point(u+side,v));
            const clip::Paths64 exclusions=forecourtExclusions(bays);
            for(double u:{-3.5,3.5}) {
                Ring bay{p.point(u-1.15,-8),p.point(u+1.15,-8),p.point(u+1.15,-3),p.point(u-1.15,-3)};
                clip::Paths64 shape{clip::kMetres.path(bay)};
                bool clear=clip::area(clip::intersect(shape,exclusions))<.001;
                double lowest=1e9,highest=-1e9;
                for(auto at:bay) {
                    const auto g=anchor.toGeodetic(at.x,0,at.y);const auto cover=landcover.at(g.x,g.y);
                    if(cover&&(*cover=="water"||*cover=="forest"||*cover=="park"))clear=false;
                    auto y=ground(g.x,g.y).y;lowest=std::min(lowest,y);highest=std::max(highest,y);
                }
                if(clear&&highest-lowest<.4) {
                    const auto at=p.point(u,-5.5);const auto g=anchor.toGeodetic(at.x,0,at.y);
                    p.exteriorVehicles.push_back(ground(g.x,g.y));
                }
            }
        }
    };
    approaches();
    std::vector<Ring> footprints = built.footprints;
    footprints.insert(footprints.end(), landmarks.solids.begin(), landmarks.solids.end());
    std::vector<double> tops = built.tops;
    tops.insert(tops.end(), landmarks.solidTops.begin(), landmarks.solidTops.end());
    // A totem by the road for each station, clear of what already stands.
    CanopyBook totems;totems.stations=built.fuelStations;
    placeFuelTotems(osm.roads,anchor,ground,footprints,totems);
    footprints.insert(footprints.end(),totems.obstacles.begin(),totems.obstacles.end());
    tops.insert(tops.end(),totems.obstacleTops.begin(),totems.obstacleTops.end());
    const nlohmann::json totemLettering=totems.lettering;
    nlohmann::json fuelCanopiesJson=totems.stations;
    for(auto& c:fuelCanopiesJson){c.erase("ring");c.erase("centre");c.erase("ground");}
    std::vector<MeshPart> totemParts=totems.parts();

    // Rank 9: the terrain partitioned by what OSM says the ground is.
    HarbourStats harbour;
    std::optional<Sea> sea;
    if (in.offline) {
        sea = offlineSea(bounds);
        harbour.coastline = "Natural Earth 1:110m approximation";
    } else {
        sea = seaGeometry(osm.coastlines, bounds, elevationAt, harbour.coastline);
    }
    const auto tidal = tidalWater(osm.landcover, osm.maritime);
    const clip::Paths64 inland = inlandWaterRegion(osm.waterways, anchor);
    const auto inlandAt = [&](double lon, double lat) {
        const P3 p = anchor.toEngine(lon, lat, 0.0);
        return clip::contains(inland, P2{p.x, p.z});
    };
    const Cells cells(bounds, kWaterGrid, sea ? &*sea : nullptr, landcover, elevationAt,
                      tidal ? &*tidal : nullptr, inlandAt);
    harbour.seaCells = cells.seaCells();

    auto classify = [&](double x, double y) -> std::string {
        if (cells.at(x, y) > 0) return "water";
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
    const std::vector<OsmWay> roads = clipRoads(grades.roads, bounds);
    // The streets lie on the ground drawn, above the sea it meets.
    StreetOutput streets = buildStreets(clipRoads(grades.roads, grown(bounds, kStreetReach)), osm.features, elevations,
                                        anchor, footprints,
                                        [&](int r, int c, double h) { return cells.roadLevel(r, c, h); });
    std::vector<InteriorPlan> stores;
    for(const auto& p:built.interiors)if(retailInterior(p.recipe))stores.push_back(p);
    const ParkingOutput parking=buildRetailParking(osm,stores,footprints,elevations,anchor);
    Mesh inlandMesh(UvMode::Planar);
    clip::Paths64 visibleInland = inland;
    if (sea) visibleInland = clip::subtract(visibleInland, projectWater(sea->region, anchor));
    if (tidal) visibleInland = clip::subtract(visibleInland, projectWater(*tidal, anchor));
    // No water is drawn over a street: what the water region says of a road
    // (a river's assumed width, a canal's edge) is wrong where the road is.
    visibleInland = clip::subtract(visibleInland, streets.ground);
    if (!visibleInland.empty()) Drape(elevations, anchor).lay(visibleInland, 0.12, inlandMesh);
    BridgeOutput bridges = buildBridges(grades, bounds, elevations, anchor);
    // Road centre lines in engine metres, so a bench faces its street.
    std::vector<Segment2> roadSegments;
    for (const OsmWay& w : roads) {
        const P3 a = ground(w.points.front().x, w.points.front().y), b = ground(w.points.back().x, w.points.back().y);
        roadSegments.push_back({{a.x, a.z}, {b.x, b.z}});
    }
    // Traffic drives over the bridges, at the level they were solved at, cut
    // pieces at the tile's edge included.
    std::map<P2, double> laneLevels = grades.levels;
    for (const OsmWay& w : roads)
        if (taggedYes(w.tags, "bridge") || has(w.tags, "r1:raised"))
            for (const P2& p : w.points)
                if (!laneLevels.count(p))
                    if (auto level = grades.levelAt(p)) laneLevels[p] = *level;
    const GroundAt roadGround = [&](double lon, double lat) {
        auto it = laneLevels.find({lon, lat});
        return it == laneLevels.end() ? ground(lon, lat) : anchor.toEngine(lon, lat, it->second);
    };
    nlohmann::json laneGraph = buildLaneGraph(roads, roadGround, int(buildings.size()), center.x, center.y);
    Works works = buildWorks(osm.maritime, lightFeatures, ground, anchor, cells, harbour);
    // Runways, taxiways, aprons and what is parked on them; one helicopter
    // per military base. Nothing parks in water, inside the tile or out.
    const AirportOutput airports = buildAirports(osm, tile, elevations, anchor, [&](double lon, double lat) {
        if (bounds.west <= lon && lon <= bounds.east && bounds.south <= lat && lat <= bounds.north) return cells.at(lon, lat) != 0;
        const std::string* c = landcover.at(lon, lat);
        return c && *c == "water";
    });

    auto assemble = [&](std::vector<MeshPart>& harbourParts) {
        std::vector<MeshPart> parts;
        for (auto* list : {&terrainParts, &streets.parts, &bridges.parts, &harbourParts, &built.parts})
            for (MeshPart& p : *list) parts.push_back(p);
        for (const MeshPart& p : airports.parts) parts.push_back(p);
        for (const MeshPart& p : parking.parts) parts.push_back(p);
        for (const MeshPart& p : totemParts) parts.push_back(p);
        return parts;
    };
    std::vector<MeshPart> parts = assemble(works.parts);
    const size_t target = std::min(in.targetVertices, kTileVertexBudget);
    // The target is a share of the resident neighbourhood, while 120k stays
    // the absolute per-tile contract. Rebuild only the building meshes: their
    // footprints, street cut-outs and continuous terrain stay the same.
    for (BuildingLod lod : {BuildingLod::UnifiedBase, BuildingLod::SimpleRoofline}) {
        if (vertexCount(parts) + landmarks.vertices <= target) break;
        buildingLod = lod;
        built = buildAtLod(lod);
        approaches();
        parts = assemble(works.parts);
    }
    if (vertexCount(parts) + landmarks.vertices > kTileVertexBudget && harbour.piers) {
        // The one detail given up before a tile is refused: the piles under
        // the piers, said in the manifest (`pilesDropped`), never silent.
        HarbourStats bare;
        works = buildWorks(osm.maritime, lightFeatures, ground, anchor, cells, bare, false);
        parts = assemble(works.parts);
        harbour.pilesDropped = true;
    }
    const size_t vertices = vertexCount(parts) + landmarks.vertices;
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
    // The sea frozen over: the pack replaces the open ocean, and is walked on.
    std::optional<IceTile> pack;
    if (ocean && in.seaIce && in.seaIce->any(bounds)) {
        pack = buildIceTile(tile, anchor, *in.seaIce);
        if (vertexCount(pack->parts) > kTileVertexBudget)
            throw std::runtime_error("Sea-ice tile exceeds geometry budget (" + std::to_string(vertexCount(pack->parts)) + " vertices)");
    }
    int triangles = 0, inferred = 0;
    for (const auto& [name, n] : groundStats) {
        triangles += n;
        if (name.substr(0, name.find('@')) == kInferred) inferred += n;
    }
    const double measuredGround = 1.0 - double(inferred) / std::max(1, triangles);

    CookedTile out;
    out.minimap = makeMiniMapTile(roads, osm, bounds);
    out.tile = tile;
    nlohmann::json boats = nlohmann::json::array();
    Scatter props, nature;
    // Nobody walks on the open sea or on the pack.
    nlohmann::json crowd = {{"revision", kCrowdRevision}, {"nodes", nlohmann::json::array()},
                            {"links", nlohmann::json::array()}, {"seats", nlohmann::json::array()}, {"people", 0}};
    out.seaIce = in.seaIce;
    if (pack) {
        out.parts = pack->parts;
        if (pack->openWater) {
            // Sheltered water: a lead does not carry the ocean's swell.
            auto leads = seaNode(bounds, anchor, "Leads");
            leads["amplitude"] = 0.03;
            leads["wavelength"] = 6.0;
            out.props.push_back(leads);
        }
        props.stats = {{"placed", 0}, {"droppedForBudget", 0}, {"byKind", nlohmann::json::object()}};
        nature.stats = {{"revision", 3}, {"placed", 0}};
    } else if (ocean) {
        out.ocean = seaNode(bounds, anchor, "Ocean");
        out.ocean["amplitude"] = 0.05;
        out.ocean["wavelength"] = 12.0;
        props.stats = {{"placed", 0}, {"droppedForBudget", 0}, {"byKind", nlohmann::json::object()}};
        nature.stats = {{"revision", 3}, {"placed", 0}};
    } else {
        out.parts = std::move(parts);
        if (!inlandMesh.empty()) {
            auto water = seaNode(bounds, anchor, "Inland water");
            water["amplitude"] = 0.045;
            water["wavelength"] = 5.0;
            water["choppiness"] = 0.08;
            water["foamIntensity"] = 0.01;
            water["shoreFoam"] = 0.0;
            std::ostringstream surface;
            surface << std::fixed << std::setprecision(3);
            for (uint32_t index : inlandMesh.indices) {
                const P3 p = inlandMesh.positions[index];
                surface << p.x << ' ' << p.y << ' ' << p.z << ' ';
            }
            water["surface"] = surface.str();
            out.props.push_back(std::move(water));
        }
        std::vector<const OsmNode*> inTile;
        for (const OsmNode& f : osm.features)
            if (bounds.west <= f.lon && f.lon <= bounds.east && bounds.south <= f.lat && f.lat <= bounds.north)
                inTile.push_back(&f);
        props = planProps(inTile, ground, profile, roadSegments);
        std::vector<const OsmWay*> nearbyBuildings;
        for (const OsmWay& b : osm.buildings) nearbyBuildings.push_back(&b);
        crowd = buildWalkGraph(roads, osm.features, nearbyBuildings, footprints, elevations, anchor, props.nodes);
        // Measured canopy places real trees; the budget must hold a tile's
        // trees, not a sample of them (see kMeasuredNatureBudget).
        nature = planNature(osm, tile, anchor, ground, in.canopy ? kMeasuredNatureBudget : 320,
                            in.canopy ? &*in.canopy : nullptr);
        // The signs were placed on the surveyed ground: they stand on the dug
        // one, or on the embankment beside them.
        for (auto& n : predicted.nodes) {
            auto& position = n["transform"]["position"];
            const P3 geo = anchor.toGeodetic(position[0].get<double>(), 0.0, position[2].get<double>());
            const auto raised = grades.levelNear({geo.x, geo.y}, 5.0);
            position[1] = raised ? anchor.toEngine(geo.x, geo.y, *raised).y : ground(geo.x, geo.y).y;
        }
        // The landmark first after the ground: it is what the player came to
        // see, and the game swaps its far model out once it has streamed.
        for (auto& n : landmarks.nodes) out.props.push_back(n);
        // The signs next: they are read from the road, so they stream while
        // driving, when the rest of the furniture waits (native/world.cpp).
        for (auto& n : predicted.nodes) out.props.push_back(n);
        for (auto& n : nature.nodes) out.props.push_back(n);
        for (auto& n : props.nodes) out.props.push_back(n);
        if (cells.hasSea()) out.props.push_back(seaNode(bounds, anchor));
        const auto berths = planBoats(osm, anchor, cells, profile, climate, harbour);
        auto [boatChildren, manifest] = boatNodes(berths, anchor);
        boats = manifest;
        for (auto& n : boatChildren) out.props.push_back(n);
        for (auto& n : planContainers(osm, anchor, cells, footprints, ground, harbour)) out.props.push_back(n);
    }
    nlohmann::json footprintJson = nlohmann::json::array(), topJson = nlohmann::json::array();
    for (double t : tops) topJson.push_back(pyround(t, 2));
    for (const Ring& r : footprints) {
        nlohmann::json ring = nlohmann::json::array();
        for (const P2& p : r) ring.push_back({p.x, p.y});
        footprintJson.push_back(ring);
    }
    // What the player walks on: the pack's own surface where the sea is frozen.
    const ElevationGrid& walked = pack ? pack->grid : elevations;
    nlohmann::json elevationRows = nlohmann::json::array();
    for (int r = 0; r < walked.size; ++r) {
        nlohmann::json row = nlohmann::json::array();
        for (int c = 0; c < walked.size; ++c) row.push_back(pack ? pyround(walked.at(r, c), 3) : walked.at(r, c));
        elevationRows.push_back(row);
    }
    if (ocean) {
        laneGraph = {{"nodes", nlohmann::json::array()}, {"lanes", nlohmann::json::array()}, {"cars", 0},
                     {"leftHand", laneGraph["leftHand"]}};
    }
    out.manifest = {
        {"key", tile.key()}, {"row", tile.row}, {"col", tile.col}, {"lon", center.x}, {"lat", center.y},
        {"bounds", {{"south", bounds.south}, {"west", bounds.west}, {"north", bounds.north}, {"east", bounds.east}}},
        {"elevations", elevationRows}, {"footprints", footprintJson}, {"footprintTops", topJson},
        // The landmark models are nodes, not tile geometry, but they share
        // the arena, so the residency count includes them.
        {"vertices", pack ? vertexCount(pack->parts) : ocean ? 0 : vertices},
        // The arena holds indices too (three for every vertex it holds): a
        // finely gridded tile reaches that limit before the vertex one.
        {"indices", pack ? indexCount(pack->parts) : ocean ? 0 : indexCount(parts)},
        {"buildings", buildings.size()}, {"surface", pack ? "sea-ice" : ocean ? "ocean" : "land"},
        {"source", std::string(in.offline ? "Natural Earth 1:110m; " : "OpenStreetMap; ") + in.elevationSource},
        {"elevationSource", in.elevationSource}, {"offlineApproximation", in.offline || in.groundPending},
        {"groundPending", in.groundPending},
        {"region", profile.name}, {"regionTier", profile.tier}, {"climate", climate},
        {"osmQueryVersion", osm.queryVersion},
        {"ground", {{"measuredFraction", pyround(measuredGround, 4)}, {"trianglesByClass", groundStats}}},
        {"peaks", peakReport},
        {"water", pack ? pack->water : cells.rows()}, {"decks", works.decks},
        {"dryStreets", pack || ocean ? nlohmann::json::array() : dryStreets(streets.ground, cells, anchor)}, {"boats", boats}, {"harbour", harbour.json()},
        {"props", props.stats},
        {"aircraft", ocean ? nlohmann::json::array() : airports.aircraft}, {"airports", airports.stats},
        {"airportsPending", in.airportsPending}, {"provisional", in.provisional}, {"landmarks", ocean ? nlohmann::json::array() : landmarks.manifest},
        {"landmarkRevision", kLandmarkRevision},
        {"landmarkReplacedWays", ocean ? nlohmann::json::array() : landmarks.replaced},
        {"nature", nature.stats}, {"streets", streets.stats},
        {"predicted", ocean || pack ? nlohmann::json({{"revision", kPredictRevision}, {"signs", 0}}) : predicted.stats},
        {"bridges", bridges.stats}, {"raised", ocean || pack ? nlohmann::json::array() : bridges.raised}, {"traffic", laneGraph}, {"crowd", crowd},
        {"inference", built.stats.json()},
        {"buildingGeometryLod", buildingLod == BuildingLod::Full ? "full" :
                                buildingLod == BuildingLod::UnifiedBase ? "unified-base" : "simple-roofline"},
        {"seaIce", pack ? pack->stats : in.seaIce ? nlohmann::json({{"source", in.seaIce->source}, {"frozen", false}})
                                               : nlohmann::json()},
        {"generator", "C++"}};
    out.manifest["interiors"]=nlohmann::json::array();
    for(const auto& p:built.interiors)out.manifest["interiors"].push_back(p.json());
    out.manifest["interiorStreaming"]={{"revision",1},{"observationsQueried",osm.interiorUsesQueried},
        {"loadRadius",65},{"releaseRadius",85},{"maxActive",2},{"maxVertices",24000},{"unavailable",built.interiorUnavailable}};
    out.manifest["retail"]={{"revision",1},{"stores",stores.size()},
        {"parking",parking.manifest},{"observationsQueried",osm.retailQueried}};
    const std::string fuelWord=fuelTitle(fuelCountry);
    out.manifest["fuel"]={{"revision",1},{"stations",fuelManifest},{"canopies",fuelCanopiesJson},
        {"country",fuelCountry},{"countrySource",osm.country.empty()?"bundled borders":"OSM boundary (ISO3166-1)"},
        {"title",fuelWord.empty()?"brand (no word listed for this country)":
            facadeLettering(fuelWord,14.0,0.4)?fuelWord:"brand (the sign font cannot spell "+fuelWord+")"},
        {"openRoofs",built.openRoofs},{"observationsQueried",osm.fuelQueried}};
    nlohmann::json lettering=built.lettering;
    for(const auto& l:totemLettering)lettering.push_back(l);
    out.manifest["lettering"]=lettering;
    out.cookMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return out;
}

}  // namespace r1
