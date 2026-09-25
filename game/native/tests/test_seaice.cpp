// The sea ice: what is read, what is filled, and the pack drawn from it.
#include "check.hpp"

#include "gen/cook.hpp"
#include "gen/seaice.hpp"

using namespace r1;

namespace {
// A 3 x 3 window of the ASCAT grid centred on the pole, as ERDDAP answers it:
// the pole cell unread (the satellite's hole), multi-year ice around it.
nlohmann::json poleAnswer(const char* date = "2026-09-22T12:00:00Z") {
    nlohmann::json rows = nlohmann::json::array();
    const double step = 4280.900055;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            nlohmann::json v = (i == 0 && j == 0) ? nlohmann::json() : nlohmann::json(i == 1 && j == 1 ? 3.0 : 5.0);
            rows.push_back({date, 0, -j * step + 0.5, i * step - 0.5, v});
        }
    return {{"table", {{"columnNames", {"time", "altitude", "rows", "cols", "IceClass"}}, {"rows", rows}}}};
}
bool finite(const Mesh& m) {
    for (const P3& p : m.positions) if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    for (const P3& n : m.normals) if (!std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z)) return false;
    return true;
}
Observations frozenOcean(const Tile& tile, std::shared_ptr<const SeaIce> ice) {
    Observations in;
    in.tile = tile;
    in.osm = std::make_shared<const OsmData>();
    in.elevations = ElevationGrid{tile.bounds(), 2, {0.0, 0.0, 0.0, 0.0}};
    in.elevationSource = "test";
    in.seaIce = std::move(ice);
    return in;
}
}  // namespace

TEST(SeaIce, the_pole_hole_takes_the_ice_around_it_and_says_so) {
    const SeaIce ice = seaIceFrom(seaIceDocument(poleAnswer()));
    CHECK(ice.measured && ice.date == "2026-09-22");
    CHECK(ice.filled == 1);
    CHECK(ice.classAt(0, 90) == kIcePerennial);
    const auto at = ice.at(0, 90);
    NEAR(at.concentration, 1.0, 1e-12);
    CHECK(at.age > 0.9);
}
TEST(SeaIce, the_window_of_the_pole_is_one_file_for_every_tile_around_it) {
    const SeaIceWindow a = seaIceWindow(0, 89.999), b = seaIceWindow(120, 89.999), c = seaIceWindow(-60, 89.99);
    CHECK(a.file() == b.file() && b.file() == c.file());
    CHECK(seaIceGridCovers(0, 90) && seaIceGridCovers(-150, 72) && !seaIceGridCovers(2.35, 48.86));
    CHECK(a.url().find("noaacwSARciceclassnpoleDaily") != std::string::npos);
}
TEST(SeaIce, the_season_is_the_observations_date_never_a_clock) {
    CHECK(iceSeason(seaIceFrom(seaIceDocument(poleAnswer("2026-07-20T12:00:00Z"))).dayOfYear, true) == 1);
    CHECK(iceSeason(seaIceFrom(seaIceDocument(poleAnswer())).dayOfYear, true) == 2);
    CHECK(iceSeason(seaIceFrom(seaIceDocument(poleAnswer("2026-02-10T12:00:00Z"))).dayOfYear, true) == 0);
    // The austral summer is January.
    CHECK(iceSeason(20, false) == 1);
}
TEST(SeaIce, without_a_reading_only_the_ice_that_never_leaves_is_drawn) {
    const SeaIce north = inferredSeaIce(89.9);
    CHECK(!north.measured && north.source.find("inferred") == 0);
    CHECK(north.at(0, 90).concentration > 0.99 && north.at(0, 75).concentration == 0.0);
    CHECK(inferredSeaIce(-75).at(0, -75).concentration > 0.99);
}
TEST(SeaIce, the_pack_is_one_surface_across_the_pole_and_across_tiles) {
    const IceParameters p{1.0, 0.8};
    // The same point reached from both sides of the pole.
    const IcePoint a = iceAt(10, 89.99999, p, 0), b = iceAt(10, 89.99999, p, 0);
    CHECK(a.height == b.height && a.kind == b.kind);
    NEAR(iceAt(0, 90, p, 0).height, iceAt(180, 90, p, 0).height, 1e-9);
    // Two tiles of one ring agree on the edge they share.
    const SeaIce ice = inferredSeaIce(88);
    const Tile left = tileAt(10, 88.2), right{left.row, left.col + 1};
    const IceTile l = buildIceTile(left, Anchor::at(left.center().x, left.center().y), ice);
    const IceTile r = buildIceTile(right, Anchor::at(right.center().x, right.center().y), ice);
    const int n = kIceMeshSize;
    for (int row = 0; row < n; ++row) NEAR(l.grid.at(row, n - 1), r.grid.at(row, 0), 1e-12);
}
TEST(SeaIce, a_tile_at_the_pole_is_finite_walkable_and_within_budget) {
    const auto ice = std::make_shared<const SeaIce>(seaIceFrom(seaIceDocument(poleAnswer())));
    for (int col = 0; col < columns(kRows - 1); ++col) {
        const Tile tile{kRows - 1, col};
        const CookedTile t = cookTile(frozenOcean(tile, ice));
        CHECK(t.manifest["surface"] == "sea-ice");
        CHECK(t.ocean.is_null());
        CHECK(t.manifest["elevations"].size() == size_t(kIceMeshSize));
        CHECK(t.manifest["water"].size() == size_t(kIceMeshSize - 1));
        CHECK(t.manifest["vertices"].get<size_t>() <= kTileVertexBudget);
        // Twelve of them stand around the pole, inside 85% of the arena's
        // 3 145 728 indices with the rest of the scene.
        CHECK_MSG(t.manifest["indices"].get<size_t>() * 12 <= 3145728 * 85 / 100 - 150000, t.manifest["indices"]);
        CHECK(t.manifest["seaIce"]["measured"] == true);
        bool snow = false;
        for (const auto& part : t.parts) {
            CHECK_MSG(finite(part.mesh), part.name);
            snow |= part.name.find("Snow on sea ice") != std::string::npos;
        }
        CHECK(snow);
        // Most of the pack is ice to stand on, not water.
        CHECK(t.manifest["seaIce"]["openWater"].get<double>() < 0.3);
    }
}
TEST(SeaIce, the_same_observation_gives_the_same_pack) {
    const auto ice = std::make_shared<const SeaIce>(inferredSeaIce(89));
    const Tile tile = tileAt(33, 89.2);
    const CookedTile a = cookTile(frozenOcean(tile, ice)), b = cookTile(frozenOcean(tile, ice));
    CHECK(a.parts.size() == b.parts.size());
    for (size_t i = 0; i < a.parts.size(); ++i) CHECK(a.parts[i].mesh.positions == b.parts[i].mesh.positions);
    CHECK(a.manifest["seaIce"] == b.manifest["seaIce"]);
}
TEST(SeaIce, open_ocean_stays_ocean) {
    const auto ice = std::make_shared<const SeaIce>(inferredSeaIce(70));
    const Tile tile = tileAt(0, 70.5);
    const CookedTile t = cookTile(frozenOcean(tile, ice));
    CHECK(t.manifest["surface"] == "ocean");
    CHECK(!t.ocean.is_null());
}
TEST(SeaIce, summer_opens_the_leads_and_the_ponds) {
    const IceParameters p{0.9, 0.3};
    int water = 0, ponds = 0, winterWater = 0;
    for (int i = 0; i < 4000; ++i) {
        const double lon = (i % 80) * 0.02, lat = 88.0 + (i / 80) * 0.0004;
        const IcePoint summer = iceAt(lon, lat, p, 1), winter = iceAt(lon, lat, p, 0);
        water += summer.kind == IceKind::Water;
        ponds += summer.kind == IceKind::Pond;
        winterWater += winter.kind == IceKind::Water;
    }
    CHECK(water > winterWater);
    CHECK(ponds > 100);
}
