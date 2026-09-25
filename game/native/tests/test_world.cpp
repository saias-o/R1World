// The ground, the streets, the sea and what stands on them, then a whole tile.
#include "check.hpp"

#include "gen/buildings.hpp"
#include "gen/cook.hpp"
#include "gen/harbours.hpp"
#include "gen/landmarks.hpp"
#include "gen/scatter.hpp"
#include "gen/sea.hpp"
#include "gen/service.hpp"
#include "gen/sources.hpp"
#include "gen/streets.hpp"
#include "gen/terrain.hpp"

#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <thread>

using namespace r1;
namespace fs = std::filesystem;

namespace {
OsmWay way(int64_t id, std::vector<P2> points, Tags tags) { return OsmWay{id, std::move(points), std::move(tags)}; }
bool exists(const std::string& projectPath) { return fs::exists(r1test::gameRoot() + "/" + projectPath); }

// A flat 220 m box on the equator whose north edge is 20 m higher.
struct Slope {
    Anchor anchor = Anchor::at(0, 0);
    ElevationGrid grid{{-0.001, -0.001, 0.001, 0.001}, 2, {0.0, 0.0, 20.0, 20.0}};
    OsmWay road = way(1, {{-0.0005, 0.0}, {0.0005, 0.0}}, {{"highway", "residential"}, {"width", "6"}, {"sidewalk", "both"}});
    StreetOutput streets(const std::vector<OsmWay>& roads, const std::vector<OsmNode>& features = {}) {
        return buildStreets(roads, features, grid, anchor, {});
    }
};
const MeshPart* part(const std::vector<MeshPart>& parts, const std::string& name) {
    for (const auto& p : parts) if (p.name == name) return &p;
    return nullptr;
}
double area(const Mesh& m) {
    double total = 0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const P3 a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]], c = m.positions[m.indices[i + 2]];
        total += std::abs((b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x)) / 2;
    }
    return total;
}
}  // namespace

// ── the ground ──────────────────────────────────────────────────────────────

TEST(Ground, water_beats_everything_it_sits_inside) {
    const Landcover cover({way(1, {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0, 0}}, {{"landuse", "forest"}}),
                           way(2, {{0.4, 0.4}, {0.6, 0.4}, {0.6, 0.6}, {0.4, 0.6}, {0.4, 0.4}}, {{"natural", "water"}})});
    CHECK(*cover.at(0.5, 0.5) == "water");
    CHECK(*cover.at(0.1, 0.1) == "forest");
    CHECK(cover.at(2, 2) == nullptr);
}
TEST(Ground, a_way_that_says_nothing_about_the_ground_is_not_ground) {
    CHECK(classifyWay({{"building", "yes"}}).empty());
    CHECK(classifyWay({{"leisure", "park"}}) == "grass");
}
TEST(Ground, every_class_is_a_plausible_albedo) {
    // CLAUDE.md rule 2: above ~0.35 a sunlit ground saturates. Only ice and snow may.
    for (const auto& c : palette().groundClasses)
        for (double v : c.swatch.color)
            CHECK_MSG(v <= (c.name == "glacier" ? 0.9 : 0.45), c.name << " " << v);
    for (const auto& p : palette().profiles)
        for (double v : p.ground.color) CHECK_MSG(v <= 0.45, p.key << " ground " << v);
}
TEST(Ground, partitioning_the_terrain_creates_no_vertices) {
    const ElevationGrid grid{{0, 0, 0.005, 0.0076}, 2, {10, 12, 30, 25}};
    const Anchor anchor = Anchor::at(0.0038, 0.0025);
    const auto one = buildTerrain(grid.bounds, grid, anchor, nullptr, nullptr);
    const auto split = buildTerrain(grid.bounds, grid, anchor, [](double x, double) { return x < 0.003 ? "a" : "b"; }, nullptr);
    size_t whole = 0, parts = 0;
    for (const auto& [n, m] : one) whole += m.vertexCount();
    for (const auto& [n, m] : split) parts += m.vertexCount();
    CHECK(split.size() == 2);
    CHECK_MSG(parts <= whole + 2 * kTerrainMeshSize * 2, parts << " vs " << whole);
}

// ── surfaces ────────────────────────────────────────────────────────────────

TEST(Surfaces, every_family_is_on_disk) {
    for (auto it = palette().surfaces["families"].begin(); it != palette().surfaces["families"].end(); ++it)
        for (const char* k : {"albedo", "normal", "mr"}) CHECK_MSG(exists(it.value()[k].get<std::string>()), it.key());
}
TEST(Surfaces, every_ground_class_in_every_climate_has_a_material) {
    for (const auto& c : palette().groundClasses)
        for (const char* climate : {"tropical", "arid", "mediterranean", "temperate", "boreal", "polar"})
            if (c.name != "water") CHECK_MSG(groundFamily(c.name, "Ground, temperate", climate).has_value(), c.name << " " << climate);
}
TEST(Surfaces, the_snowline_falls_with_latitude) {
    CHECK(snowline(10) == 5500 && snowline(46) > 2500 && snowline(46) < 3000 && snowline(75) == 0);
    CHECK(coldSuffix(46, 3200, "temperate") == "@snow" && coldSuffix(46, 2600, "temperate") == "@frost");
    CHECK(climateAt("temperate", 60) == "boreal" && climateAt("arid", 70) == "polar");
}
TEST(Surfaces, the_rendered_mean_is_the_palette_albedo) {
    const Material m = surfaceMaterial("Asphalt", {0.10, 0.11, 0.12}, 0.9, std::string("asphalt"));
    const double level = palette().surfaces["families"]["asphalt"]["level"];
    NEAR(m.color[0] * level, 0.10, 1e-12);
    CHECK(!m.baseColorTexture.empty() && !m.normalTexture.empty());
}
TEST(Surfaces, materials_are_what_their_names_say) {
    CHECK(wallFamily("Red brick") == "brick_red" && wallFamily("Pierre de taille, cream") == "stone");
    CHECK(roofFamily("Ardoise, blue-black") == "slate" && roofFamily("Zinc, grey") == "metal_seam");
}

// ── streets ─────────────────────────────────────────────────────────────────

TEST(Streets, explicit_absence_prevents_a_sidewalk) {
    Slope s;
    for (const char* v : {"no", "none", "separate"}) {
        const auto out = s.streets({way(2, s.road.points, {{"highway", "residential"}, {"sidewalk", v}})});
        CHECK(!part(out.parts, "Sidewalks"));
    }
    CHECK(roadWidth({{"highway", "primary"}, {"width", "4.2"}}) == 4.2);
}
TEST(Streets, no_roads_is_valid_empty_street_geometry) {
    Slope s;
    const auto out = s.streets({});
    CHECK(out.parts.empty() && out.stats["carriagewayAreaM2"] == 0);
}
TEST(Streets, one_surface_alone_has_no_kerbs) {
    Slope s;
    CHECK(s.streets({way(2, s.road.points, {{"highway", "footway"}})}).parts.size() == 1);
    CHECK(s.streets({way(2, s.road.points, {{"highway", "residential"}, {"sidewalk", "no"}})}).parts.size() == 1);
}
TEST(Streets, grade_separated_roads_are_counted_not_drawn) {
    Slope s;
    const auto out = s.streets({way(2, s.road.points, {{"highway", "primary"}, {"bridge", "yes"}})});
    CHECK(out.parts.empty() && out.stats["unsupportedGradeSeparatedWays"] == 1);
}
TEST(Streets, the_road_and_its_kerbs_follow_the_terrain_it_crosses) {
    Slope s;
    const auto out = s.streets({s.road});
    const MeshPart* road = part(out.parts, "Carriageway");
    const MeshPart* kerb = part(out.parts, "Kerbs");
    CHECK(road && kerb && !kerb->mesh.empty());
    double low = 1e300, high = -1e300;
    for (const P3& p : road->mesh.positions) {
        low = std::min(low, p.y); high = std::max(high, p.y);
        const P3 g = s.anchor.toGeodetic(p.x, 0, p.z);
        NEAR(p.y, groundPoint(g.x, g.y, s.grid, s.anchor).y + 0.06, 1e-4);
    }
    CHECK(high - low > 0.4);
}
TEST(Streets, the_carriageway_covers_its_width_and_nothing_more) {
    Slope s;
    const auto out = s.streets({s.road});
    // 111 m long, 6 m wide: the drawn surface is what the strip is.
    NEAR(area(part(out.parts, "Carriageway")->mesh), out.stats["carriagewayAreaM2"].get<double>(), 1.0);
    NEAR(out.stats["carriagewayAreaM2"].get<double>(), 111.3 * 6, 5.0);
}
TEST(Streets, a_zebra_needs_a_surveyed_marking) {
    Slope s;
    for (const auto& [marking, expected] : std::vector<std::pair<std::string, int>>{{"zebra", 1}, {"no", 0}, {"yes", 0}}) {
        const auto out = s.streets({s.road}, {OsmNode{2, 0, 0, {{"highway", "crossing"}, {"crossing:markings", marking}}}});
        CHECK(out.stats["zebraCrossingsTagged"] == expected);
        CHECK(bool(part(out.parts, "Surveyed zebra crossings")) == bool(expected));
    }
}
TEST(Streets, pavement_is_cut_out_of_building_footprints) {
    Slope s;
    const P3 a = s.anchor.toEngine(-0.0001, -0.00003), b = s.anchor.toEngine(0.0001, 0.00008);
    const Ring footprint{{a.x, a.z}, {b.x, a.z}, {b.x, b.z}, {a.x, b.z}};
    const auto with = buildStreets({s.road}, {}, s.grid, s.anchor, {footprint});
    const auto without = s.streets({s.road});
    CHECK(with.stats["sidewalkAreaM2"].get<double>() < without.stats["sidewalkAreaM2"].get<double>() - 10);
}

// ── the sea ─────────────────────────────────────────────────────────────────

namespace {
const Bounds kBay{43.270, 6.630, 43.275, 6.640};
const double kMid = (kBay.south + kBay.north) / 2;
const OsmWay kCoast = way(1, {{6.620, kMid}, {6.650, kMid}}, {{"natural", "coastline"}});
}  // namespace

TEST(Sea, the_sea_is_right_of_the_coastline) {
    std::string how;
    const auto sea = seaGeometry({kCoast}, kBay, [](double, double) { return 5.0; }, how);
    CHECK(sea && how == "coastline");
    CHECK(sea->contains(6.635, kBay.south + 0.0005) && !sea->contains(6.635, kBay.north - 0.0005));
    OsmWay reversed = kCoast;
    std::reverse(reversed.points.begin(), reversed.points.end());
    CHECK(seaGeometry({reversed}, kBay, [](double, double) { return 5.0; }, how)->contains(6.635, kBay.north - 0.0005));
}
TEST(Sea, an_island_keeps_its_land) {
    std::string how;
    const auto sea = seaGeometry({way(3, {{6.633, 43.2715}, {6.637, 43.2715}, {6.637, 43.2735}, {6.633, 43.2735}, {6.633, 43.2715}},
                                      {{"natural", "coastline"}})},
                                 kBay, [](double, double) { return 0.0; }, how);
    CHECK(sea && !sea->contains(6.635, 43.2725) && sea->contains(6.631, 43.2705));
}
TEST(Sea, no_coastline_no_sea) {
    std::string how;
    CHECK(!seaGeometry({}, kBay, [](double, double) { return 0.0; }, how) && how == "no coastline");
}
TEST(Sea, cells_mark_the_sea_and_sink_its_floor) {
    std::string how;
    const auto sea = seaGeometry({kCoast}, kBay, [](double, double) { return 5.0; }, how);
    const Landcover none({});
    const Cells cells(kBay, 40, &*sea, none, [](double, double) { return -0.2; }, nullptr);
    CHECK(cells.hasSea() && cells.rows()[2][20] == '2' && cells.rows()[37][20] == '0');
    CHECK(cells.adjust(2, 20, 0.0) == -4.0 && cells.adjust(37, 20, -0.6) == 0.25);
}
TEST(Sea, mapped_water_at_sea_level_joins_the_sea) {
    std::string how;
    const auto sea = seaGeometry({kCoast}, kBay, [](double, double) { return 5.0; }, how);
    const Landcover dock({way(4, {{6.634, 43.2735}, {6.636, 43.2735}, {6.636, 43.2745}, {6.634, 43.2745}, {6.634, 43.2735}},
                              {{"natural", "water"}})});
    CHECK(Cells(kBay, 40, &*sea, dock, [](double, double) { return 1.0; }, nullptr).at(6.635, 43.274) == 2);
    CHECK(Cells(kBay, 40, &*sea, dock, [](double, double) { return 40.0; }, nullptr).at(6.635, 43.274) == 1);
}
TEST(Sea, every_hull_is_on_disk) {
    for (const BoatKind& b : palette().boats) CHECK_MSG(exists("assets/models/external/kenney_boats/" + b.model + ".glb"), b.model);
}
TEST(Sea, a_boat_node_faces_its_heading) {
    const BoatKind* speed = nullptr;
    for (const BoatKind& b : palette().boats) if (b.name == "speed_a") speed = &b;
    auto [nodes, manifest] = boatNodes({Berth{speed, 10.0, -5.0, {1.0, 0.0}, 3.0}}, Anchor::at(6.635, kMid));
    NEAR(manifest[0]["heading"].get<double>(), 90.0, 1e-6);
    const double angle = 2.0 * std::atan2(nodes[0]["transform"]["rotation"][1].get<double>(), nodes[0]["transform"]["rotation"][3].get<double>());
    NEAR(-std::sin(angle), 1.0, 1e-6);
    CHECK(nodes[0]["children"].size() == 2);  // hull and helm: never flattened
}
TEST(Sea, a_traced_lighthouse_stands_on_its_footprint) {
    const double d = 0.00008;
    const OsmNode n = tracedLighthouse(way(715849418, {{6.635 - d, kMid - d}, {6.635 + d, kMid - d}, {6.635 + d, kMid + d},
                                                       {6.635 - d, kMid + d}, {6.635 - d, kMid - d}},
                                           {{"building", "yes"}, {"man_made", "lighthouse"}, {"height", "52"}}));
    NEAR(n.lon, 6.635, 1e-9);
    CHECK(n.tags.at("man_made") == "lighthouse" && std::stod(n.tags.at("r1:radius")) > 8);
}

// ── props, plants, traffic ──────────────────────────────────────────────────

TEST(Props, no_region_plants_a_kit_tree) {
    // CLAUDE.md rule 1: the trees are the photoscanned ones, never a kit's.
    for (const auto& p : palette().profiles) for (const auto& stem : p.treeModels) CHECK(stem.find("kenney") == std::string::npos);
    for (const auto& k : palette().props)
        for (const auto& m : k.models) {
            CHECK_MSG(exists(m), m);
            if (k.name == "tree") CHECK(m.find("kenney") == std::string::npos);
        }
}
TEST(Props, a_share_of_zero_places_nothing_at_all) {
    std::vector<OsmNode> trees;
    for (int i = 0; i < 50; ++i) trees.push_back({i + 1, 0.0001 * i, 0, {{"natural", "tree"}}});
    std::vector<const OsmNode*> in;
    for (const auto& t : trees) in.push_back(&t);
    const auto out = planProps(in, [](double lon, double lat) { return P3{lon * 1e5, 0, lat * 1e5}; }, profileByKey("PARIS"), {});
    CHECK(out.nodes.empty());
}
TEST(Props, a_thousand_lamps_do_not_squeeze_out_five_benches) {
    std::vector<OsmNode> nodes;
    for (int i = 0; i < 1000; ++i) nodes.push_back({i + 1, 0.00001 * i, 0, {{"highway", "street_lamp"}}});
    for (int i = 0; i < 5; ++i) nodes.push_back({5000 + i, 0.00001 * i, 0.0001, {{"amenity", "bench"}}});
    std::vector<const OsmNode*> in;
    for (const auto& n : nodes) in.push_back(&n);
    const auto out = planProps(in, [](double lon, double lat) { return P3{lon * 1e5, 0, lat * 1e5}; }, profileByKey("PARIS"), {});
    CHECK(out.stats["byKind"]["bench"] == 5 && out.stats["placed"] == 260);
    CHECK(out.stats["droppedForBudget"] == 1005 - 260);
}
TEST(Props, a_bench_faces_the_street) {
    const OsmNode bench{1, 0, 0, {{"amenity", "bench"}}};
    const auto out = planProps({&bench}, [](double, double) { return P3{0, 0, 2}; }, profileByKey("PARIS"), {{{-10, 0}, {10, 0}}});
    const double yaw = 2 * std::atan2(out.nodes[0]["transform"]["rotation"][1].get<double>(), out.nodes[0]["transform"]["rotation"][3].get<double>());
    NEAR(std::abs(std::sin(yaw)), 1.0, 1e-9);  // along the street, which runs east-west
}

TEST(Traffic, a_street_is_two_lanes_and_a_one_way_is_one) {
    auto ground = [](double lon, double lat) { return P3{lon * 111320, 0, -lat * 111320}; };
    const auto two = buildLaneGraph({way(1, {{0, 0}, {0.001, 0}}, {{"highway", "residential"}})}, ground, 0, 2, 48);
    const auto one = buildLaneGraph({way(1, {{0, 0}, {0.001, 0}}, {{"highway", "residential"}, {"oneway", "yes"}})}, ground, 0, 2, 48);
    const auto round = buildLaneGraph({way(1, {{0, 0}, {0.001, 0}}, {{"highway", "residential"}, {"junction", "roundabout"}})}, ground, 0, 2, 48);
    CHECK(two["lanes"].size() == 2 && one["lanes"].size() == 1 && round["lanes"].size() == 1);
}
TEST(Traffic, a_tagged_limit_is_used_and_counted_as_measured) {
    auto ground = [](double lon, double lat) { return P3{lon * 111320, 0, -lat * 111320}; };
    const auto g = buildLaneGraph({way(1, {{0, 0}, {0.001, 0}}, {{"highway", "primary"}, {"maxspeed", "30 mph"}})}, ground, 0, 2, 48);
    NEAR(g["lanes"][0][2].get<double>(), 13.4, 0.05);
    CHECK(g["speedsTagged"] == 1);
}
TEST(Traffic, the_left_hand_world_drives_on_the_left) {
    auto ground = [](double lon, double lat) { return P3{lon * 111320, 0, -lat * 111320}; };
    CHECK(buildLaneGraph({}, ground, 0, -0.12, 51.5)["leftHand"] == true);
    CHECK(buildLaneGraph({}, ground, 0, 2.35, 48.85)["leftHand"] == false);
    CHECK(buildLaneGraph({}, ground, 0, 139.7, 35.7)["leftHand"] == true);
}
TEST(Traffic, no_car_is_bright_enough_to_saturate_and_the_fleet_is_not_one_colour) {
    CHECK(palette().carPaints.size() >= 5);
    for (const auto& c : palette().carPaints) for (double v : c) CHECK(v <= 0.35);
}
TEST(Traffic, the_cap_holds_against_any_density) {
    auto ground = [](double lon, double lat) { return P3{lon * 111320, 0, -lat * 111320}; };
    CHECK(buildLaneGraph({way(1, {{0, 0}, {0.01, 0}}, {{"highway", "primary"}})}, ground, 100000, 2, 48)["cars"] == 22);
    CHECK(buildLaneGraph({}, ground, 1000, 2, 48)["cars"] == 0);
}

// ── landmarks ───────────────────────────────────────────────────────────────

TEST(Landmarks, twenty_places_each_under_its_budgets) {
    CHECK(landmarks().size() == 22);  // Giza is three pyramids
    const size_t budget[] = {32768, 12288, 4096};
    for (const Landmark& l : landmarks()) {
        CHECK(l.levels.size() == 3);
        for (size_t i = 0; i < 3; ++i) {
            CHECK_MSG(l.levels[i].vertices <= budget[i], l.slug << " level " << i);
            CHECK_MSG(exists(l.levels[i].path), l.levels[i].path);
        }
        CHECK(l.levels[0].vertices > l.levels[1].vertices && l.levels[1].vertices >= l.levels[2].vertices);
    }
}
TEST(Landmarks, the_trace_of_a_landmark_is_replaced_by_its_model) {
    const Landmark* eiffel = nullptr;
    for (const Landmark& l : landmarks()) if (l.slug == "eiffel_tower") eiffel = &l;
    const Tile t = tileAt(eiffel->lon, eiffel->lat);
    const OsmWay trace = way(5013364, {{2.2940, 48.8578}, {2.2950, 48.8578}, {2.2950, 48.8587}, {2.2940, 48.8587}, {2.2940, 48.8578}},
                             {{"building", "tower"}, {"wikidata", eiffel->wikidata}});
    const OsmWay neighbour = way(7, {{2.2990, 48.8555}, {2.2992, 48.8555}, {2.2992, 48.8557}, {2.2990, 48.8557}, {2.2990, 48.8555}},
                                 {{"building", "yes"}});
    const Anchor anchor = Anchor::at(t.center().x, t.center().y);
    const auto placed = placeLandmarks(t.bounds(), [&](double lon, double lat) { return anchor.toEngine(lon, lat, 0); }, {&trace, &neighbour});
    CHECK(placed.kept.size() == 1 && placed.kept[0] == &neighbour);
    CHECK(placed.replaced.size() == 1 && placed.nodes.size() == 1);
    CHECK(placed.solids.size() == 4);  // the pillars
}

// ── a whole tile ────────────────────────────────────────────────────────────

namespace {
Observations paris(Tile tile) {
    ObservationStore store(r1test::gameRoot());
    auto doc = store.osm(tile, std::nullopt);
    auto ground = store.ground(tile);
    CHECK_MSG(doc && ground, "needs the cached observations of " << tile.key() << " (play Paris once)");
    Observations in;
    in.tile = tile;
    in.osm = std::make_shared<const OsmData>(normalizeOsm(*doc));
    in.elevations = ground->first;
    in.elevationSource = ground->second;
    return in;
}
}  // namespace

TEST(Cook, a_dense_paris_tile_fits_its_budget_and_says_what_it_inferred) {
    const CookedTile t = cookTile(paris(tileAt(2.3522, 48.8566)));
    CHECK(t.manifest["vertices"].get<size_t>() <= kTileVertexBudget);
    CHECK(t.manifest["buildings"].get<int>() > 100);
    CHECK(t.manifest["water"].size() == 40 && t.manifest["water"][0].get<std::string>().size() == 40);
    CHECK(t.manifest["inference"]["heightMeasured"].get<int>() + t.manifest["inference"]["heightInferred"].get<int>() ==
          t.manifest["inference"]["count"].get<int>());
    CHECK(t.manifest["region"] == "Paris intra-muros");
}
TEST(Cook, the_heart_of_paris_is_busy_and_its_people_have_somewhere_to_walk) {
    const CookedTile t = cookTile(paris(tileAt(2.3522, 48.8566)));
    const auto& crowd = t.manifest["crowd"];
    CHECK(crowd["nodes"].size() > 200 && crowd["links"].size() > 200);
    CHECK(crowd["inputs"]["crossingsJoined"].get<int>() > 10);
    CHECK_MSG(crowd["people"].get<int>() >= 30, crowd["inputs"]);
}
TEST(Cook, a_resident_target_uses_building_lod_without_losing_the_ground) {
    Observations in = paris(tileAt(2.3522, 48.8566));
    in.targetVertices = 80000;
    const CookedTile t = cookTile(in);
    CHECK(t.manifest["vertices"].get<size_t>() <= in.targetVertices);
    CHECK(t.manifest["buildingGeometryLod"] != "full");
    CHECK(t.manifest["footprints"].size() > 100);
    bool ground = false, roof = false;
    for (const auto& part : t.parts) {
        ground |= part.name.find("Ground") == 0;
        roof |= part.name.find("Roofs") == 0;
    }
    CHECK(ground && roof);
}
TEST(Cook, every_footprint_says_how_high_it_stands) {
    // What an aircraft clears or stops against (gen/airports, world.cpp).
    const CookedTile t = cookTile(paris(tileAt(2.3522, 48.8566)));
    const auto& tops = t.manifest["footprintTops"];
    CHECK(tops.size() == t.manifest["footprints"].size());
    for (const auto& top : tops) CHECK(top.get<double>() > 25.0 && top.get<double>() < 400.0);
    CHECK(t.manifest.contains("aircraft") && t.manifest.contains("airports"));
}
TEST(Cook, the_same_observations_give_the_same_tile) {
    const Observations in = paris(tileAt(2.2945, 48.8584));
    const CookedTile a = cookTile(in), b = cookTile(in);
    CHECK(a.parts.size() == b.parts.size() && a.props == b.props);
    for (size_t i = 0; i < a.parts.size(); ++i) CHECK(a.parts[i].mesh.positions == b.parts[i].mesh.positions);
    CHECK(a.manifest["landmarks"].size() == 1 && a.manifest["landmarks"][0]["slug"] == "eiffel_tower");
}
TEST(Cook, a_cooked_tile_uploads_nothing_outside_its_own_parts) {
    const CookedTile t = cookTile(paris(tileAt(2.3522, 48.8566)));
    for (const auto& p : t.parts) {
        CHECK(!p.mesh.empty());
        for (uint32_t i : p.mesh.indices) CHECK(i < p.mesh.vertexCount());
    }
}

// ── the service ─────────────────────────────────────────────────────────────

namespace {
// A game root holding one tile's observations, answered to query `version`.
std::string placeVisitedAt(int version, const Tile& t, const std::string& suffix = {}) {
    const std::string root = (fs::temp_directory_path() / ("r1-visited-v" + std::to_string(version) + suffix)).string();
    fs::remove_all(root);
    const std::string folder = root + "/cache/world/" + t.key();
    fs::create_directories(folder);
    const Bounds b = t.bounds();
    const double lon = (b.west + b.east) / 2, lat = (b.south + b.north) / 2, d = 0.0001;
    nlohmann::json osm = {{"r1QueryVersion", version}, {"elements", nlohmann::json::array()}};
    int id = 1;
    for (auto [x, y] : {std::pair{lon - d, lat - d}, {lon + d, lat - d}, {lon + d, lat + d}, {lon - d, lat + d}})
        osm["elements"].push_back({{"type", "node"}, {"id", id++}, {"lon", x}, {"lat", y}});
    osm["elements"].push_back({{"type", "way"}, {"id", 10}, {"nodes", {1, 2, 3, 4, 1}}, {"tags", {{"building", "yes"}}}});
    osm["elements"].push_back({{"type", "node"}, {"id", 5}, {"lon", lon - d}, {"lat", lat + 3 * d}});
    osm["elements"].push_back({{"type", "node"}, {"id", 6}, {"lon", lon + d}, {"lat", lat + 3 * d}});
    osm["elements"].push_back({{"type", "way"}, {"id", 11}, {"nodes", {5, 6}},
                                {"tags", {{"highway", "residential"}, {"name", "Allée de Noyalo"}}}});
    std::ofstream(folder + "/osm.json") << osm.dump();
    nlohmann::json ground = {{"bounds", {{"south", b.south}, {"west", b.west}, {"north", b.north}, {"east", b.east}}},
                             {"size", 2}, {"values", {{50.0, 50.0}, {50.0, 50.0}}}, {"source", "test"}};
    std::ofstream(folder + "/ground-elevation.json") << ground.dump();
    return root;
}

std::shared_ptr<const ServedTile> waitForTile(WorldService& service, const Tile& tile, uint64_t after = 0) {
    std::shared_ptr<const ServedTile> served;
    for (int i = 0; i < 200; ++i) {
        served = service.find(tile);
        if (served && served->serial > after) return served;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return nullptr;
}
}  // namespace

TEST(Sources, an_answer_before_the_aero_layer_is_cooked_without_asking_again) {
    const Tile t = tileAt(5.0, 45.0);
    const ObservationStore store(placeVisitedAt(kOsmBaseVersion, t));
    bool stale = true, needed = false;
    const auto path = store.osmPath(t, std::nullopt, &stale);
    CHECK(path && !stale);
    CHECK(!store.aeroPath(t, std::nullopt, *path, needed) && needed);
    CHECK(store.aeroTarget(t, std::nullopt).path == store.tileFolder(t) + "/osm.aero.json");
    CHECK(ObservationStore::aeroSibling("a/sources/x.json") == "a/sources/x.aero.json");
    const ObservationStore current(placeVisitedAt(kOsmQueryVersion, t));
    const auto full = current.osmPath(t, std::nullopt);
    CHECK(full && !current.aeroPath(t, std::nullopt, *full, needed) && !needed);
}
TEST(Sources, a_layer_read_with_its_answer_adds_what_it_has_and_nothing_twice) {
    const nlohmann::json main = {{"elements", {{{"type", "node"}, {"id", 1}, {"lon", 0.0}, {"lat", 0.0}},
                                               {{"type", "node"}, {"id", 2}, {"lon", 0.001}, {"lat", 0.0}},
                                               {{"type", "way"}, {"id", 5}, {"nodes", {1, 2}}, {"tags", {{"highway", "service"}}}}}}};
    nlohmann::json layer = main;
    layer["elements"].push_back({{"type", "node"}, {"id", 3}, {"lon", 0.0005}, {"lat", 0.0}, {"tags", {{"aeroway", "parking_position"}}}});
    layer["elements"].push_back({{"type", "way"}, {"id", 6}, {"nodes", {1, 2}}, {"tags", {{"aeroway", "runway"}}}});
    const OsmData data = normalizeOsm(main, &layer);
    CHECK(data.roads.size() == 1 && data.aeroways.size() == 1 && data.features.size() == 1);
}
TEST(Service, a_place_visited_before_the_aero_layer_does_not_wait_for_it) {
    // What made every Go wait on Overpass after the question widened: now
    // the older answer is cooked at once and says its airports are to come.
#ifdef _WIN32
    _putenv_s("HTTPS_PROXY", "http://127.0.0.1:9");
    _putenv_s("HTTP_PROXY", "http://127.0.0.1:9");
#endif
    const Tile t = tileAt(5.0, 45.0);
    WorldService::Options options;
    options.gameRoot = placeVisitedAt(kOsmBaseVersion, t);
    options.threads = 1;
    WorldService service(std::move(options));
    service.want({t}, {});
    std::shared_ptr<const ServedTile> served;
    for (int i = 0; i < 200 && !(served = service.find(t)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(served);
    CHECK(!served->cooked.manifest["offlineApproximation"].get<bool>());
    CHECK(served->cooked.manifest["airportsPending"].get<bool>());
    CHECK(served->cooked.manifest["buildings"].get<int>() == 1);
}
TEST(Service, a_visited_place_never_touches_the_network) {
    // CLAUDE.md §7: every source pointed at a closed port. A tile that
    // needed one would come back as the offline approximation.
#ifdef _WIN32
    _putenv_s("HTTPS_PROXY", "http://127.0.0.1:9");
    _putenv_s("HTTP_PROXY", "http://127.0.0.1:9");
#endif
    std::vector<std::string> said;
    std::mutex lock;
    WorldService::Options options;
    options.gameRoot = r1test::gameRoot();
    options.threads = 2;
    options.log = [&](const std::string& s) { std::lock_guard<std::mutex> g(lock); said.push_back(s); };
    WorldService service(std::move(options));
    const Tile t = tileAt(2.3522, 48.8566);
    service.want({t}, {});
    std::shared_ptr<const ServedTile> served;
    for (int i = 0; i < 300 && !(served = service.find(t)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(served);
    CHECK(!served->cooked.manifest["offlineApproximation"].get<bool>());
    std::lock_guard<std::mutex> g(lock);
    for (const auto& s : said) CHECK_MSG(s.find("OFFLINE") == std::string::npos && s.find("FALLBACK") == std::string::npos, s);
}

TEST(Service, cached_streets_appear_before_relief_and_remain_after_it_arrives) {
    const Tile t = tileAt(5.0, 45.0);
    const std::string root = placeVisitedAt(kOsmQueryVersion, t, "-relief-pending");
    fs::remove(root + "/cache/world/" + t.key() + "/ground-elevation.json");
    std::promise<void> release;
    const auto ready = release.get_future().share();
    WorldService::Options options;
    options.gameRoot = root;
    options.threads = 1;
    options.quickGround = [](const Tile&) -> std::pair<ElevationGrid, std::string> {
        throw SourceUnavailable("quick relief unavailable in this test");
    };
    options.fetchGround = [ready, root](const Tile& tile) {
        ready.wait();
        const Bounds b = tile.bounds();
        nlohmann::json doc = {{"bounds", {{"south", b.south}, {"west", b.west}, {"north", b.north}, {"east", b.east}}},
                              {"size", 2}, {"values", {{50.0, 50.0}, {50.0, 50.0}}}, {"source", "test"}};
        std::ofstream(root + "/cache/world/" + tile.key() + "/ground-elevation.json") << doc.dump();
        return std::pair{ElevationGrid{b, 2, {50.0, 50.0, 50.0, 50.0}}, std::string("test")};
    };
    WorldService service(std::move(options));
    service.want({t}, {});
    const auto first = waitForTile(service, t);
    CHECK(first);
    CHECK(first->cooked.manifest["groundPending"].get<bool>());
    CHECK(first->cooked.manifest["buildings"].get<int>() == 1);
    CHECK(!first->cooked.minimap.roads.empty());
    release.set_value();
    const auto upgraded = waitForTile(service, t, first->serial);
    CHECK(upgraded);
    CHECK(!upgraded->cooked.manifest["groundPending"].get<bool>());
    CHECK(upgraded->cooked.manifest["buildings"].get<int>() == 1);
    CHECK(!upgraded->cooked.minimap.roads.empty());
}

TEST(Service, arriving_osm_replaces_only_the_missing_map_data) {
    const Tile t = tileAt(5.0, 45.0);
    const std::string root = placeVisitedAt(kOsmQueryVersion, t, "-osm-pending");
    const std::string original = root + "/cache/world/" + t.key() + "/osm.json";
    std::ifstream input(original);
    const std::string answer((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    fs::remove(original);
    std::promise<void> release;
    const auto ready = release.get_future().share();
    WorldService::Options options;
    options.gameRoot = root;
    options.threads = 1;
    options.fetchOsm = [ready, answer](const Bounds&, const std::string& path) {
        ready.wait();
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream(path) << answer;
    };
    WorldService service(std::move(options));
    service.want({t}, {});
    const auto first = waitForTile(service, t);
    CHECK(first);
    CHECK(first->cooked.manifest["provisional"].get<bool>());
    CHECK(!first->cooked.manifest["groundPending"].get<bool>());
    CHECK(first->cooked.minimap.roads.empty());
    release.set_value();
    const auto upgraded = waitForTile(service, t, first->serial);
    CHECK(upgraded);
    CHECK(!upgraded->cooked.manifest["provisional"].get<bool>());
    CHECK(!upgraded->cooked.minimap.roads.empty());
    CHECK(upgraded->cooked.manifest["elevationSource"] == "test");
}

TEST(Service, failed_relief_does_not_hold_back_arriving_streets) {
    const Tile t = tileAt(5.0, 45.0);
    const std::string root = placeVisitedAt(kOsmQueryVersion, t, "-independent-failure");
    const std::string folder = root + "/cache/world/" + t.key();
    std::ifstream input(folder + "/osm.json");
    const std::string answer((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    fs::remove(folder + "/osm.json");
    fs::remove(folder + "/ground-elevation.json");
    std::promise<void> release;
    const auto ready = release.get_future().share();
    WorldService::Options options;
    options.gameRoot = root;
    options.threads = 1;
    options.fetchOsm = [ready, answer](const Bounds&, const std::string& path) {
        ready.wait();
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream(path) << answer;
    };
    options.quickGround = [](const Tile&) -> std::pair<ElevationGrid, std::string> {
        throw SourceUnavailable("quick relief failed");
    };
    options.fetchGround = [](const Tile&) -> std::pair<ElevationGrid, std::string> {
        throw SourceUnavailable("surveyed relief failed");
    };
    WorldService service(std::move(options));
    service.want({t}, {});
    const auto first = waitForTile(service, t);
    CHECK(first);
    CHECK(first->cooked.minimap.roads.empty());
    release.set_value();
    const auto upgraded = waitForTile(service, t, first->serial);
    CHECK(upgraded);
    CHECK(!upgraded->cooked.minimap.roads.empty());
    CHECK(upgraded->cooked.manifest["groundPending"].get<bool>());
    CHECK(!upgraded->cooked.manifest["provisional"].get<bool>());
}

// ── ships ───────────────────────────────────────────────────────────────────

TEST(Ships, the_channel_has_ships_and_the_desert_has_none) {
    SeaService sea(r1test::gameRoot(), nullptr);
    const double noon = 1790000000;  // a Saturday in September 2026
    sea.ask(1.5, 50.9, noon);        // the Strait of Dover
    std::shared_ptr<const nlohmann::json> doc;
    for (int i = 0; i < 200 && !(doc = sea.latest()); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(doc && !(*doc)["ships"].empty());
    for (const auto& s : (*doc)["ships"]) {
        CHECK(s["distance"].get<double>() <= 6000);
        CHECK((*doc)["hulls"].contains(s["model"].get<std::string>()));
    }
}
