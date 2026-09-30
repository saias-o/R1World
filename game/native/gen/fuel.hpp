// Fuel stations. OSM maps the station (amenity=fuel, a node or an area) and,
// most of the time, its canopy as an open roof (building=roof, or a cadastre
// building with wall=no). The canopy is drawn as one: a slab on columns that
// cars and people pass under, never a closed block, and never a shop. Under
// it the pump islands, their dispensers and the canopy's lettering are
// synthesized from the canopy's own rectangle: nothing mapped says where the
// pumps stand. Beside the road a totem carries the brand, and no price: a
// price is not observed.
#pragma once
#include "buildings.hpp"
#include "osm.hpp"

namespace r1 {
// A roof with no walls: a canopy, a carport, a shelter.
bool openRoof(const Tags& tags);
// A fuel station, as a node or an area.
bool fuelStation(const Tags& tags);
// What the canopy's fascia and the totem read: the brand, else the name.
std::string fuelBrand(const Tags& tags);
// What a canopy's fascia reads in the country (ISO 3166-1 alpha-2): the word
// for a fuel station in its language; empty for a country not listed.
std::string fuelTitle(const std::string& country);
// The country at a point from the bundled borders (assets/world/countries.geojson).
std::string countryAt(double lon, double lat);
// The brand's livery colour as an albedo; a neutral one for an unknown brand.
std::array<double, 3> fuelLivery(const std::string& brand);

// Copies of the tile's buildings with every station's canopy marked
// ("r1:fuel", "r1:fuelBrand", "r1:fuelSource", and "r1:fuelTitle", what the
// fascia reads in `country`'s language), plus an inferred canopy for a
// station of this tile that has none mapped nearby. `manifest` receives one
// entry per station of the tile, saying which of the two it got, or why none.
std::vector<OsmWay> fuelCanopies(const std::vector<const OsmWay*>& ways, const OsmData& osm, const Tile& tile,
                                 const std::string& country, nlohmann::json& manifest);

// The geometry of the tile's open roofs, accumulated, then turned into parts.
struct CanopyBook {
    Mesh top{UvMode::Slope}, underside, fascia, steel, concrete, body, screens, bollards, lights;
    std::vector<std::pair<std::array<double, 3>, Mesh>> livery;  // one mesh per brand colour
    std::vector<Ring> obstacles;       // engine (x, z): islands and totems, what a car or a walker bumps
    std::vector<double> obstacleTops;  // engine y
    // {text, at [x, y, z], normal [x, z], width, height, colour}: glyphs built by the game.
    nlohmann::json lettering = nlohmann::json::array();
    // One per fuel canopy: {id, brand, canopySource, ring, islands, pumps}.
    nlohmann::json stations = nlohmann::json::array();
    Mesh& liveryMesh(const std::array<double, 3>& colour);
    std::vector<MeshPart> parts();
};
// One open roof: a fuel canopy when the tags carry "r1:fuel", else a plain
// roof on posts. `wallHeight` is the building's own, tagged or inferred.
void buildOpenRoof(const Ring& ring, const OrientedBox& box, double ground, double wallHeight, const Tags& tags,
                   int64_t id, CanopyBook& book);
// A totem by the nearest road for each canopy the book recorded, clear of
// every footprint. The book records "totem" on each station: placed, or why not.
void placeFuelTotems(const std::vector<OsmWay>& roads, const Anchor& anchor,
                     const std::function<P3(double, double)>& ground, const std::vector<Ring>& footprints,
                     CanopyBook& book);
}  // namespace r1
