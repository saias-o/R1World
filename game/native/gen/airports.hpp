// Airports and military bases.
//
// The paved surfaces are what OSM traced -- runways, taxiways, aprons,
// helipads -- laid on the terrain the way the streets are (`Drape`), with the
// paint an aerodrome standard puts on them: thresholds, designators, centre
// lines, aiming points, taxi lines. Grass and earth strips are left as the
// ground they are.
//
// The aircraft are inferred, and each one says so (§3 I5): OSM maps where an
// aircraft may park (`aeroway=parking_position`, aprons), never which one is
// parked there. The airport's longest runway decides what it can receive; a
// stand's room decides what fits on it. Military bases get one helicopter
// each and nothing else, whatever they are: a base is its own update.
#pragma once

#include "mesh.hpp"
#include "osm.hpp"
#include "polygons.hpp"

#include <functional>

namespace r1 {

struct AirportOutput {
    std::vector<MeshPart> parts;
    // One entry per aircraft whose centre is in the tile: what the game
    // instantiates and boards, and where the inference came from.
    nlohmann::json aircraft = nlohmann::json::array();
    nlohmann::json stats = nlohmann::json::object();
};

// `wet(lon, lat)` answers whether a point is water, so nothing parks in it.
AirportOutput buildAirports(const OsmData& osm, const Tile& tile, const ElevationGrid& elevations,
                            const Anchor& anchor, const std::function<bool(double, double)>& wet);

// The designator painted at the threshold a pilot lands over: "09", "27L".
// `ref` is the way's OSM tag ("09L/27R"), `bearing` the true bearing of the
// landing. The tag wins where one of its halves is within 20 degrees of the
// bearing (runways are named by magnetic heading); otherwise the bearing.
std::string runwayDesignator(const std::string& ref, double bearing);

}  // namespace r1
