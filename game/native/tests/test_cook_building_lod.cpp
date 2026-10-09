// Building variants are worker products, while density and collision continue
// to describe the same surveyed baseline independently of the camera's LOD.
#include "check.hpp"
#include "gen/cook.hpp"

#include <algorithm>

using namespace r1;

namespace {
struct BuildingTile {
    Tile tile = tileAt(2.3522, 48.8566);
    Anchor anchor = Anchor::at(tile.center().x, tile.center().y, 0);
    OsmData osm;

    BuildingTile() {
        osm.queryVersion = 10;
        osm.interiorUsesQueried = true;
        add(401, {{-12, -8}, {12, -8}, {12, 10}, {-12, 10}},
            {{"building", "house"}, {"height", "11"}, {"roof:shape", "gabled"}, {"roof:height", "3"}});
    }
    void add(int64_t id, const Ring& ring, const Tags& tags) {
        OsmWay w; w.id = id; w.tags = tags;
        for (auto p : ring) {
            const auto q = anchor.toGeodetic(p.x, 0, p.y);
            w.points.push_back({q.x, q.y});
        }
        w.points.push_back(w.points.front());
        osm.buildings.push_back(std::move(w));
    }
    CookedTile cook(size_t target = kTileVertexBudget) const {
        Observations in; in.tile = tile; in.osm = std::make_shared<const OsmData>(osm);
        in.elevations = {tile.bounds(), 2, {10, 10, 10, 10}};
        in.elevationSource = "test:flat"; in.targetVertices = target;
        return cookTile(in);
    }
};
size_t vertices(const std::vector<MeshPart>& parts) {
    size_t n = 0; for (const auto& p : parts) n += p.mesh.vertexCount(); return n;
}
size_t indices(const std::vector<MeshPart>& parts) {
    size_t n = 0; for (const auto& p : parts) n += p.mesh.indices.size(); return n;
}
void valid(const std::vector<MeshPart>& parts) {
    for (const auto& p : parts) {
        CHECK(!p.mesh.empty());
        for (auto i : p.mesh.indices) CHECK(i < p.mesh.vertexCount());
    }
}
}  // namespace

TEST(CookBuildingLod, baseline_density_is_separate_from_resident_far_geometry) {
    const auto tile = BuildingTile{}.cook();
    CHECK(tile.buildings.size() == 1);
    CHECK(!tile.buildingCollisions.empty());
    CHECK(tile.manifest.at("landmarks").empty());
    CHECK(tile.manifest.at("vertices").get<size_t>() == vertices(tile.parts) + vertices(tile.buildingCollisions));
    CHECK(tile.manifest.at("indices").get<size_t>() == indices(tile.parts) + indices(tile.buildingCollisions));
    size_t residentVertices = vertices(tile.parts), residentIndices = indices(tile.parts);
    for (const auto& building : tile.buildings) {
        CHECK(building.id == 401);
        CHECK(building.footprint < tile.manifest.at("footprints").size());
        for (const auto& level : building.levels) { CHECK(!level.empty()); valid(level); }
        CHECK(!building.reducedNear.empty()); valid(building.reducedNear);
        residentVertices += vertices(building.levels[2]);
        residentIndices += indices(building.levels[2]);
    }
    CHECK(tile.manifest.at("residentVertices").get<size_t>() == residentVertices);
    CHECK(tile.manifest.at("residentIndices").get<size_t>() == residentIndices);
    CHECK(tile.manifest.at("buildingLods").at("count") == 1);
    CHECK(tile.manifest.at("buildingLods").at("buildings")[0].at("id") == 401);
    valid(tile.buildingCollisions);
    for (const auto& p : tile.parts) {
        CHECK(p.name.rfind("Walls", 0) != 0);
        CHECK(p.name.rfind("Roofs", 0) != 0);
    }
}

TEST(CookBuildingLod, open_roofs_and_bus_shelters_keep_visible_static_parts) {
    BuildingTile source;
    source.add(402, {{35, 0}, {45, 0}, {45, 8}, {35, 8}}, {{"building", "roof"}, {"height", "4"}});
    source.add(403, {{-40, 0}, {-36, 0}, {-36, 3}, {-40, 3}},
        {{"building", "yes"}, {"r1:bus-shelter", "measured"}, {"height", "2.4"}});
    const auto tile = source.cook();
    CHECK(tile.buildings.size() == 1 && tile.buildings.front().id == 401);
    CHECK(tile.manifest.at("fuel").at("openRoofs") == 1);
    CHECK(tile.manifest.at("inference").at("busShelters") == 1);
    bool canopy = false, shelter = false;
    for (const auto& p : tile.parts) {
        canopy |= p.name.find("Canop") != std::string::npos || p.name.find("canop") != std::string::npos;
        shelter |= p.name.rfind("Bus shelter", 0) == 0;
    }
    CHECK(canopy && shelter);
    for (const auto& p : tile.buildingCollisions) CHECK(p.name.rfind("Bus shelter", 0) != 0);
    CHECK(tile.manifest.at("vertices").get<size_t>() == vertices(tile.parts) + vertices(tile.buildingCollisions));
    CHECK(tile.manifest.at("indices").get<size_t>() == indices(tile.parts) + indices(tile.buildingCollisions));
}

TEST(CookBuildingLod, portals_and_footprint_indices_survive_density_reduction) {
    const BuildingTile source;
    const auto full = source.cook(), reduced = source.cook(1);
    CHECK(full.manifest.at("footprints") == reduced.manifest.at("footprints"));
    CHECK(full.manifest.at("footprintTops") == reduced.manifest.at("footprintTops"));
    CHECK(full.manifest.at("interiors") == reduced.manifest.at("interiors"));
    CHECK(!full.manifest.at("interiors").empty());
    CHECK(full.manifest.at("buildingGeometryLod") == "full");
    CHECK(reduced.manifest.at("buildingGeometryLod") == "simple-roofline");
    CHECK(full.buildings.front().footprint == reduced.buildings.front().footprint);
    CHECK(full.buildings.front().id == reduced.buildings.front().id);
    valid(reduced.buildingCollisions);
}

TEST(CookBuildingLod, worker_variants_are_deterministic) {
    const BuildingTile source;
    const auto a = source.cook(), b = source.cook();
    CHECK(a.manifest == b.manifest);
    CHECK(writeGlb(a.buildingCollisions) == writeGlb(b.buildingCollisions));
    CHECK(a.buildings.size() == b.buildings.size());
    for (size_t building = 0; building < a.buildings.size(); ++building) {
        for (size_t level = 0; level < 3; ++level)
            CHECK(writeGlb(a.buildings[building].levels[level]) == writeGlb(b.buildings[building].levels[level]));
        CHECK(writeGlb(a.buildings[building].reducedNear) == writeGlb(b.buildings[building].reducedNear));
    }
}
