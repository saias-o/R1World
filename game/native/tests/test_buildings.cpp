// The building chain (§10): a measured value always beats an inferred one and
// says so; openings are aligned; the geometry is closed and faces out; the
// same input is the same building on every run. Footprints are literal metres
// in the engine's plane: the ground function hands back what it is given.
#include "check.hpp"

#include "gen/buildings.hpp"

#include <set>

using namespace r1;

namespace {
Ring square(double side, P2 origin = {0, 0}) {
    return {{origin.x, origin.y}, {origin.x + side, origin.y}, {origin.x + side, origin.y + side}, {origin.x, origin.y + side}};
}
Ring rotate(const Ring& ring, double degrees) {
    const double a = radians(degrees), c = std::cos(a), s = std::sin(a);
    Ring out;
    for (const P2& p : ring) out.push_back({p.x * c - p.y * s, p.x * s + p.y * c});
    return out;
}
std::vector<double> interiorAngles(const Ring& ring) {
    std::vector<double> out;
    for (size_t i = 0; i < ring.size(); ++i) {
        const P2 prev = ring[(i + ring.size() - 1) % ring.size()], cur = ring[i], next = ring[(i + 1) % ring.size()];
        const P2 a{prev.x - cur.x, prev.y - cur.y}, b{next.x - cur.x, next.y - cur.y};
        const double na = std::hypot(a.x, a.y), nb = std::hypot(b.x, b.y);
        out.push_back(degrees(std::acos(std::max(-1.0, std::min(1.0, (a.x * b.x + a.y * b.y) / (na * nb))))));
    }
    return out;
}
const RegionProfile& chamonix() { return profileByKey("CHAMONIX"); }

struct Built { BuildingOutput out; std::vector<OsmWay> ways; };
BuildingOutput build(const std::vector<std::pair<Ring, Tags>>& footprints, double detailRadius = 1e6,
                     const RegionProfile* profile = nullptr, int64_t firstId = 1) {
    static std::vector<OsmWay> keep;  // the chain holds pointers into these while it runs
    keep.clear();
    int64_t id = firstId;
    for (const auto& [ring, tags] : footprints) {
        OsmWay w{id++, ring, tags};
        w.points.push_back(ring.front());
        keep.push_back(std::move(w));
    }
    std::vector<const OsmWay*> ways;
    for (const auto& w : keep) ways.push_back(&w);
    return buildBuildings(ways, [](double x, double z) { return P3{x, 0.0, z}; }, profile ? *profile : chamonix(),
                          {0, 0}, detailRadius, 0.22, nullptr, nullptr);
}
BuildingOutput build(const Ring& ring, Tags tags = {}, double detailRadius = 1e6) { return build({{ring, tags}}, detailRadius); }

struct Tri { P3 a, b, c; P3 n() const { return faceNormal(a, b, c); } };
std::vector<Tri> triangles(const BuildingOutput& out, const std::string& contains) {
    std::vector<Tri> tris;
    for (const auto& part : out.parts) {
        if (part.name.find(contains) == std::string::npos) continue;
        const Mesh& m = part.mesh;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
            tris.push_back({m.positions[m.indices[i]], m.positions[m.indices[i + 1]], m.positions[m.indices[i + 2]]});
    }
    return tris;
}
Gabarit gabarit(const Tags& tags, int64_t id = 7, Ring ring = {{0, 0}, {12, 0}, {12, 8}, {0, 8}}, bool commercial = false) {
    const Ring clean = *cleanFootprint(ring);
    const OrientedBox box = orientedBox(clean);
    return planGabarit(tags, id, chamonix(), box, std::abs(polygonArea(clean)) / box.area(), commercial);
}
}  // namespace

// ── footprint hygiene ───────────────────────────────────────────────────────

TEST(Footprint, welds_duplicate_vertices) { CHECK(cleanFootprint({{0, 0}, {0, 0.001}, {10, 0}, {10, 10}, {0, 10}})->size() == 4); }
TEST(Footprint, drops_a_vertex_that_does_not_turn) { CHECK(cleanFootprint({{0, 0}, {5, 0}, {10, 0}, {10, 8}, {0, 8}})->size() == 4); }
TEST(Footprint, keeps_a_shallow_but_real_corner) { CHECK(cleanFootprint({{0, 0}, {5, -0.3}, {10, 0}, {10, 8}, {0, 8}})->size() == 5); }
TEST(Footprint, rejects_a_mapping_artefact) { CHECK(!cleanFootprint(square(0.8))); }
TEST(Footprint, keeps_a_garden_shed) { CHECK(cleanFootprint(square(1.6)).has_value()); }
TEST(Footprint, orients_every_ring_the_same_way) {
    Ring r = square(10);
    CHECK(polygonArea(*cleanFootprint(r)) > 0);
    std::reverse(r.begin(), r.end());
    CHECK(polygonArea(*cleanFootprint(r)) > 0);
}

TEST(Rectify, squares_up_a_hand_traced_rectangle) {
    for (double a : interiorAngles(*cleanFootprint({{0, 0}, {12.1, 0.35}, {11.7, 8.2}, {-0.3, 7.9}}))) CHECK_MSG(std::abs(a - 90) < 1, a);
}
TEST(Rectify, a_rotated_building_stays_rotated) {
    const Ring clean = *cleanFootprint(rotate({{0, 0}, {12, 0.3}, {11.8, 8}, {-0.2, 7.8}}, 31));
    for (double a : interiorAngles(clean)) CHECK(std::abs(a - 90) < 1);
    const double bearing = pymod(degrees(std::atan2(clean[1].y - clean[0].y, clean[1].x - clean[0].x)), 90.0);
    CHECK_MSG(std::abs(bearing - 31) < 2, bearing);
}
TEST(Rectify, a_real_diagonal_survives) {
    const Ring clean = *cleanFootprint({{0, 0}, {10, 0}, {10, 6}, {6, 10}, {0, 10}});
    CHECK(clean.size() == 5);
    auto angles = interiorAngles(clean);
    std::sort(angles.begin(), angles.end());
    CHECK(std::abs(angles.front() - 90) < 1.5 && angles.back() > 120);
}

TEST(OrientedBox, is_exact_for_a_rectangle) {
    const OrientedBox box = orientedBox(*cleanFootprint({{0, 0}, {12, 0}, {12, 6}, {0, 6}}));
    NEAR(box.halfU, 6, 1e-5);
    NEAR(box.halfV, 3, 1e-5);
}
TEST(OrientedBox, the_long_axis_follows_the_building) {
    const OrientedBox box = orientedBox(*cleanFootprint(rotate({{0, 0}, {20, 0}, {20, 7}, {0, 7}}, 40)));
    const double bearing = pymod(degrees(std::atan2(box.uz, box.ux)), 180.0);
    CHECK(std::min(std::abs(bearing - 40), std::abs(bearing - 220)) < 1.5);
    CHECK(box.halfU > box.halfV);
}
TEST(OrientedBox, always_contains_the_footprint) {
    const Ring ring = *cleanFootprint({{0, 0}, {14, 2}, {16, 9}, {5, 12}, {-2, 6}});
    const OrientedBox box = orientedBox(ring);
    for (const P2& p : ring) {
        CHECK(std::abs((p.x - box.cx) * box.ux + (p.y - box.cz) * box.uz) <= box.halfU + 1e-6);
        CHECK(std::abs((p.x - box.cx) * box.vx() + (p.y - box.cz) * box.vz()) <= box.halfV + 1e-6);
    }
}

// ── closed and outward ──────────────────────────────────────────────────────

TEST(Orientation, every_wall_faces_out) {
    const auto out = build(square(12, {-6, -6}), {{"building:levels", "2"}});
    int checked = 0;
    for (const Tri& t : triangles(out, "Walls")) {
        const P3 n = t.n();
        if (std::abs(n.y) > 0.5) continue;
        const double cx = (t.a.x + t.b.x + t.c.x) / 3, cz = (t.a.z + t.b.z + t.c.z) / 3;
        if (std::hypot(cx, cz) < 1e-6) continue;
        CHECK_MSG(n.x * cx + n.z * cz > 0, "a wall faces into the building");
        ++checked;
    }
    CHECK(checked > 8);
}
TEST(Orientation, a_roof_has_an_upper_and_a_lower_surface) {
    int up = 0, down = 0;
    for (const Tri& t : triangles(build({{0, 0}, {14, 0}, {14, 8}, {0, 8}}, {{"building:levels", "2"}, {"roof:shape", "gabled"}}), "Roofs")) {
        up += t.n().y > 0.2;
        down += t.n().y < -0.2;
    }
    CHECK(up > 0 && down > 0);
}
TEST(Orientation, the_ridge_is_a_single_straight_line) {
    const auto tris = triangles(build({{0, 0}, {18, 0}, {18, 8}, {0, 8}}, {{"building:levels", "2"}, {"roof:shape", "gabled"}}), "Roofs");
    double peak = -1e300;
    for (const Tri& t : tris) for (const P3& p : {t.a, t.b, t.c}) peak = std::max(peak, p.y);
    std::set<long> across;
    for (const Tri& t : tris) for (const P3& p : {t.a, t.b, t.c}) if (std::abs(p.y - peak) < 1e-6) across.insert(std::lround(p.z * 1e4));
    CHECK_MSG(across.size() == 1, across.size());
}

// ── height and roof ─────────────────────────────────────────────────────────

TEST(Gabarit, a_shop_makes_a_building_taller_not_its_flats_shorter) {
    const Ring r{{0, 0}, {20, 0}, {20, 12}, {0, 12}};
    const Gabarit plain = gabarit({{"building:levels", "3"}}, 5, r, false), shop = gabarit({{"building:levels", "3"}}, 5, r, true);
    CHECK(shop.wallHeight > plain.wallHeight && shop.storeys == plain.storeys);
}
TEST(Gabarit, a_tagged_height_is_used_and_marked_measured) {
    const Gabarit g = gabarit({{"height", "14"}});
    CHECK(g.heightSource == "tag:height" && g.wallHeight + g.roofHeight <= 14.01);
}
TEST(Gabarit, levels_become_metres_through_the_region) {
    const Gabarit g = gabarit({{"building:levels", "4"}});
    CHECK(g.heightSource == "tag:levels" && g.storeys == 4);
    NEAR(g.wallHeight, 4 * chamonix().storeyHeight, 1e-9);
}
TEST(Gabarit, an_untagged_building_is_marked_inferred) {
    const Gabarit g = gabarit({});
    CHECK(g.heightSource == "atlas" && g.roofSource == "atlas");
}
TEST(Gabarit, feet_are_metres) { const Gabarit g = gabarit({{"height", "30 ft"}}); CHECK(std::abs(g.wallHeight + g.roofHeight - 9.144) < 0.6); }
TEST(Gabarit, a_tagged_roof_shape_wins) { const Gabarit g = gabarit({{"roof:shape", "hipped"}}); CHECK(g.roofShape == "hipped" && g.roofSource == "tag:shape"); }
TEST(Gabarit, an_unbuildable_shape_maps_to_its_nearest_relative) { CHECK(gabarit({{"roof:shape", "mansard"}}).roofShape == "hipped"); }
TEST(Gabarit, a_non_rectangular_footprint_refuses_a_ridge) {
    const Gabarit g = gabarit({{"roof:shape", "gabled"}}, 7, {{0, 0}, {16, 0}, {16, 5}, {6, 5}, {6, 14}, {0, 14}});
    CHECK(g.roofShape == "flat" && g.roofSource == "atlas:not-rectangular");
}
TEST(Gabarit, a_building_too_wide_for_one_ridge_refuses_a_ridge) {
    const Gabarit g = gabarit({{"roof:shape", "gabled"}, {"building:levels", "3"}}, 7, {{0, 0}, {60, 0}, {60, 40}, {0, 40}});
    CHECK(g.roofShape == "flat" && g.roofSource == "atlas:span");
}
TEST(Gabarit, an_inferred_roof_never_out_rises_its_walls) {
    for (int id = 0; id < 60; ++id) { const Gabarit g = gabarit({}, id); CHECK(g.roofHeight <= g.wallHeight + 1e-6); }
}
TEST(Gabarit, a_measured_roof_height_is_left_alone) {
    const Gabarit g = gabarit({{"roof:shape", "pyramidal"}, {"roof:height", "18"}, {"height", "30"}});
    CHECK(g.roofHeight > g.wallHeight);
}
TEST(Gabarit, the_region_supplies_a_plausible_storey_count) {
    std::set<int> counts;
    for (int id = 0; id < 200; ++id) counts.insert(gabarit({}, id).storeys);
    for (int c : counts) CHECK(c >= 2 && c <= 4);
    CHECK(counts.size() > 1);
}

// ── alignment ───────────────────────────────────────────────────────────────

TEST(Facade, windows_on_different_walls_share_floor_levels) {
    std::set<long> sills;
    const auto panes = triangles(build(square(16, {-8, -8}), {{"building:levels", "3"}}), "glazing");
    CHECK(!panes.empty());
    for (const Tri& t : panes) sills.insert(std::lround(std::min({t.a.y, t.b.y, t.c.y}) * 1000));
    CHECK_MSG(sills.size() == 3, sills.size());
}
TEST(Facade, a_distant_building_keeps_its_massing_and_loses_its_windows) {
    const auto out = build(square(16, {-8, -8}), {{"building:levels", "3"}}, -1.0);
    CHECK(triangles(out, "glazing").empty());
    CHECK(!triangles(out, "Walls").empty() && !triangles(out, "Roofs").empty());
}
TEST(Facade, a_shared_wall_carries_no_openings) {
    const auto out = build({{square(10, {0, 0}), {{"building:levels", "2"}}}, {square(10, {10, 0}), {{"building:levels", "2"}}}});
    CHECK(out.stats.partyWalls >= 2);
    for (const Tri& t : triangles(out, "glazing")) {
        const double x = (t.a.x + t.b.x + t.c.x) / 3;
        CHECK_MSG(std::abs(x - 10) > 0.5, "a window opens onto the neighbour at x=" << x);
    }
}
TEST(Facade, a_street_gap_is_not_a_party_wall) {
    const auto out = build({{square(10, {0, 0}), {{"building:levels", "2"}}}, {square(10, {14, 0}), {{"building:levels", "2"}}}});
    CHECK(out.stats.partyWalls == 0);
}

// ── determinism and the region ──────────────────────────────────────────────

TEST(Determinism, two_runs_produce_identical_geometry) {
    const auto a = build(square(12), {}), b = build(square(12), {});
    CHECK(a.parts.size() == b.parts.size());
    for (size_t i = 0; i < a.parts.size(); ++i) {
        CHECK(a.parts[i].name == b.parts[i].name);
        CHECK(a.parts[i].mesh.positions == b.parts[i].mesh.positions);
        CHECK(a.parts[i].mesh.indices == b.parts[i].mesh.indices);
    }
}
TEST(Determinism, neighbouring_ids_do_not_draw_the_same_house) {
    std::set<long> heights;
    for (int id = 1; id <= 12; ++id) heights.insert(std::lround(gabarit({}, id).wallHeight * 100));
    CHECK(heights.size() > 3);
}

TEST(Region, a_named_region_beats_the_band_it_sits_in) {
    CHECK(profileFor(6.87, 45.92).key == "CHAMONIX");
    CHECK(profileFor(2.35, 48.85).key == "PARIS");
    CHECK(profileFor(7.5, 46.5).tier == "band");
}
TEST(Region, outside_every_rectangle_admits_it) { CHECK(profileFor(-150.0, -60.0).tier == "none"); }
TEST(Region, every_roof_shape_a_profile_can_draw_is_one_the_generator_builds) {
    for (const auto& p : palette().profiles)
        for (const auto& [shape, weight] : p.roofShapeWeights)
            CHECK_MSG(shape == "flat" || shape == "gabled" || shape == "hipped" || shape == "pyramidal" || shape == "skillion",
                      p.key << " draws " << shape);
}
TEST(Region, a_place_of_worship_gets_its_steeple) {
    const auto out = build({{0, 0}, {30, 0}, {30, 12}, {0, 12}}, {{"building", "church"}, {"religion", "christian"}});
    CHECK(out.stats.steeples == 1);
}
