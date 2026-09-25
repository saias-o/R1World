// What the world is made of: the Atlas's regional profiles, the ground
// classes, and the photographed surfaces.
//
// All of it is data, in `assets/world/atlas.json` and
// `assets/textures/surfaces.json`, read once by `loadPalette` before any tile
// is cooked. Every colour in them is a measured albedo (CLAUDE.md rule 2).
#pragma once

#include "mesh.hpp"
#include "osm.hpp"

#include <optional>

namespace r1 {

struct Swatch {
    std::string name;
    std::array<double, 3> color{};
    double roughness = 0.9, weight = 1.0;
};

// Everything a generator needs to infer what the data does not say. A
// profile is only ever a fallback: anything measured beats it (PLAN §3 I5).
struct RegionProfile {
    std::string key, name, tier, climate;
    double storeyHeight = 2.75, groundStoreyHeight = 3.6;
    std::vector<std::pair<int, double>> storeyWeights;
    std::vector<std::pair<std::string, double>> roofShapeWeights;
    double roofPitchLow = 24, roofPitchHigh = 40, eaveOverhang = 0.85, parapetHeight = 0.45;
    double bayWidth = 3.1, windowWidth = 1.15, windowHeight = 1.45, windowSill = 0.95, windowInset = 0.12;
    double pierMin = 0.55, lintelMin = 0.35, shopfrontWidthRatio = 0.78, shopfrontHeight = 2.35;
    double shopfrontSill = 0.35, shopfrontPierMin = 0.28;
    std::vector<Swatch> walls, roofs;
    Swatch trim, glass, ground, foliage, bark;
    std::vector<std::string> treeModels, commercialKeys, commercialBuildingValues;

    const Swatch& wallSwatch(PyRandom& rng) const { return pick(walls, rng); }
    const Swatch& roofSwatch(PyRandom& rng) const { return pick(roofs, rng); }
    std::string roofShape(PyRandom& rng) const;
    double roofPitch(PyRandom& rng) const { return roofPitchLow + rng.random() * (roofPitchHigh - roofPitchLow); }
    int storeys(PyRandom& rng) const;
    bool isCommercial(const Tags& tags) const;
    static const Swatch& pick(const std::vector<Swatch>& swatches, PyRandom& rng);
};

// The profile that governs inference at a point: the first box that holds it.
const RegionProfile& profileFor(double lon, double lat);
const RegionProfile& profileByKey(const std::string& key);

// ── ground ──────────────────────────────────────────────────────────────────
constexpr const char* kInferred = "inferred";

// The ground class an OSM way declares, or empty.
std::string classifyWay(const Tags& tags);

// The classified polygons of one tile, sorted by class priority so the first
// that contains a point is also the winning one.
class Landcover {
public:
    explicit Landcover(const std::vector<OsmWay>& ways);
    // The class at (lon, lat), or nullptr where OSM mapped nothing.
    const std::string* at(double lon, double lat) const;
    size_t size() const { return areas_.size(); }

private:
    struct Area { int rank; const std::string* name; std::vector<P2> ring; double west, south, east, north; };
    std::vector<Area> areas_;
};

// The swatch a class renders with ("@snow"/"@frost" suffixes included).
const Swatch& groundSwatch(const std::string& name, const RegionProfile& profile);

// ── surfaces ────────────────────────────────────────────────────────────────
std::string climateAt(const std::string& profileClimate, double lat);
double snowline(double lat);
std::string coldSuffix(double lat, double altitude, const std::string& climate);
std::optional<std::string> groundFamily(const std::string& name, const std::string& inferredGround,
                                        const std::string& climate);
std::string wallFamily(const std::string& swatchName);
std::string roofFamily(const std::string& swatchName);

// A material showing `family` at its real size, averaging to `color`; without
// a family, the flat measured colour.
Material surfaceMaterial(const std::string& name, std::array<double, 3> color, double roughness,
                         const std::optional<std::string>& family, bool doubleSided = false);

// ── the data files ──────────────────────────────────────────────────────────
struct PropKind {
    std::string name;
    std::vector<std::string> models;
    double height = 1, share = 0, jitter = 0;
    int priority = 0;
    bool facesRoad = false;
    std::vector<std::pair<std::string, std::vector<std::string>>> tags;
};

struct BoatKind {
    std::string name, model, kind;
    double length = 0, top = 0, accel = 0, turn = 0, lengthRatio = 0, draft = 0.25;
};

// One aircraft of `assets/models/aircraft/fleet.json` (tools/r1/aircraft_fleet.py):
// its size for the stands it may park on, its two models, and the arcade
// handling the game flies it with. Speeds in m/s, rates in degrees a second.
struct AircraftType {
    std::string name, klass;  // klass: "airliner", "jet" or "helicopter"
    double length = 0, span = 0, height = 0, cg = 0;
    std::string nearModel, farModel;
    size_t nearVertices = 0, farVertices = 0;
    double top = 0, rotate = 0, stall = 0, accel = 0, spool = 0, brake = 0, turnRadius = 0;
    double rollRate = 0, maxBank = 0, pitchRate = 0, maxPitch = 0, climb = 0;
};

struct Palette {
    std::string gameRoot;
    std::vector<RegionProfile> profiles;
    std::vector<std::pair<std::array<double, 4>, const RegionProfile*>> regions;
    const RegionProfile* generic = nullptr;
    struct GroundClass { std::string name; Swatch swatch; std::vector<std::pair<std::string, std::vector<std::string>>> tags; };
    std::vector<GroundClass> groundClasses;
    Swatch snow, frost;
    // The pack's surfaces (gen/seaice.cpp), each with the photographed family
    // it is drawn with (empty: the flat measured colour).
    struct IceSwatch { Swatch swatch; std::string family; };
    std::map<std::string, IceSwatch> seaIce;
    std::vector<PropKind> props;
    std::vector<BoatKind> boats;
    std::vector<AircraftType> aircraft;
    // Traffic paints: albedos, not paint chips (CLAUDE.md rule 2).
    std::vector<std::array<double, 3>> carPaints;
    nlohmann::json surfaces;
};

// Reads the data files under `gameRoot` once; throws naming the file that is
// missing or unreadable (CLAUDE.md §3: a refusal says why).
void loadPalette(const std::string& gameRoot);
const Palette& palette();
// The aircraft type called `name`, or nullptr.
const AircraftType* aircraftType(const std::string& name);

}  // namespace r1
