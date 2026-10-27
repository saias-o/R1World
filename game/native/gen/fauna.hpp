// The land animals: who lives where, cooked with each tile, and who is out
// at a given hour, season and weather.
//
// Which species can be met is decided by where the tile really is: its
// country and, inside a country, the boxes a species keeps to (wolves in the
// French Alps, not in Paris), its climate, and the habitat under each patch
// of ground -- the OSM land cover and the buildings around it, sampled on a
// grid of kFaunaCell. How many is a published density per km2 of optimal
// habitat times the share a walker can meet (`assets/animals/fauna.json`):
// most tiles hold no wolf, a forest tile holds a few roe deer, and a spider
// shows in a few houses in a hundred. Who lives on a tile is drawn from its
// key, so every player meets the same residents (PLAN §3 I3); it is inferred
// and the manifest says so (I5).
//
// The game draws and animates them (native/world.cpp) through the engine's
// wildlife add-on (engine/plugins/wildlife), which knows nothing of this.
#pragma once

#include "interiors.hpp"
#include "osm.hpp"
#include "polygons.hpp"

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace r1 {

constexpr int kFaunaRevision = 2;
// Residents retain their v61 identity across later terrain recooks, so saved
// deaths are not undone when bird content increments the world generator.
constexpr int kFaunaIdentityVersion = 61;
// The habitat grid's cell: a hedge-bound meadow is a few cells across.
constexpr double kFaunaCell = 8.0;
// Built share is the buildings and urban ground within this of a cell.
constexpr double kFaunaBuiltRadius = 64.0;
// Most resident groups a tile keeps, and of one species: a tile is never a zoo.
constexpr int kFaunaTileGroups = 48, kFaunaSpeciesGroups = 10;

enum class Habitat : uint8_t {
    None, Water, Building, Forest, Scrub, Meadow, Park, Farmland, Farmyard, Orchard,
    Wetland, Beach, Desert, Savanna, Rock, Ice, Urban, Bare, Open, Count
};
constexpr size_t kHabitats = size_t(Habitat::Count);
const char* habitatName(Habitat h);
// The habitat fauna.json calls `name`; Habitat::None when it names none.
Habitat habitatNamed(const std::string& name);

struct FaunaModel {
    std::string model;
    double weight = 1;
    bool young = false;               // never leads a group, never alone
    std::set<std::string> countries;  // empty: anywhere in the range
};

struct FaunaRange {
    std::set<std::string> countries, exclude;
    std::optional<std::array<double, 4>> box;  // west, south, east, north
    double absLatLow = 0, absLatHigh = 90;
    double factor = 1;
    std::map<Habitat, double> habitats;        // overrides, merged
    std::optional<std::pair<int, int>> months; // overrides the species'
};

struct Species {
    struct Bird {
        bool enabled = false, flies = true;
        double speed = 9, acceleration = 4, climb = 3, turn = 1.5;
        double low = 8, high = 25, flyMin = 15, flyMax = 50, restMin = 10, restMax = 35;
        double glide = .2, flapHz = 4, groundShare = .3, perchLow = 1, perchHigh = 30;
        double maxWind = 12;
        std::array<double,12> seasonalFlight{1,1,1,1,1,1,1,1,1,1,1,1};
    } bird;
    std::string id, name, scientific, klass, activity, settlement;
    bool domestic = false;
    std::vector<FaunaRange> range;
    std::set<std::string> climates;            // empty: every climate
    std::array<double, kHabitats> habitats{};
    double water = 0;                          // must live within this of water, metres
    double density = 0, detectability = 0;
    int groupMin = 1, groupMax = 1;
    std::optional<std::pair<int, int>> months; // northern temperate calendar, inclusive
    double offSeason = 0, rain = 1;
    // How it behaves (engine/plugins/wildlife Kind).
    double flee = 0, spread = 4, home = 60, hoverLow = 0, hoverHigh = 0;
    bool dart = false;
    std::string surface = "ground";
    double scale = 1;
    std::vector<FaunaModel> models, indoorModels;
    struct Indoor {
        double homes = 0, night = 1, rural = 1;
        std::vector<std::string> rooms;
        std::string place;                     // wall, floor, air, trail
        std::map<int, double> months;
    } indoor;
};

struct FaunaCatalog {
    std::vector<Species> species;
    std::map<std::string, std::array<double, 24>> activity;
    struct Settlement { double falloff = 0, power = 1, needs = 0; };
    std::map<std::string, Settlement> settlement;
};

// Reads assets/animals/fauna.json once; throws naming what is wrong in it
// (CLAUDE.md §3).
void loadFauna(const std::string& gameRoot);
const FaunaCatalog& fauna();
const Species* speciesById(const std::string& id);

// Whether `species` lives at (lon, lat) in `country` under `climate`, and the
// range entries' factor, habitat and season overrides there.
struct RangeAnswer {
    bool inside = false;
    double factor = 1;
    std::map<Habitat, double> habitats;
    std::optional<std::pair<int, int>> months;
};
RangeAnswer rangeAt(const Species& s, const std::string& country, double lon, double lat, const std::string& climate);

// Share of a species out at a local solar hour (0..24).
double activityAt(const std::string& activity, double hour);
// Share out in a calendar month (1..12) at `lat`: the northern temperate
// calendar flipped south of the equator, every month in the tropics.
double seasonAt(const Species& s, const RangeAnswer& range, int month, double lat);
// Daylight-adjusted solar hour and species' seasonal flight effort. Rain and
// wind are host weather inputs in mm/h and m/s. All parameters are inferred.
double birdActivityAt(const Species& s, double hour, int month, double lat, double rain, double wind);
// How the built share around a point (0..1) suits a species.
double settlementFactor(const Species& s, double built);

// The ground a tile's animals live on, in the tile's frame (x east, z south).
struct HabitatGrid {
    double cell = kFaunaCell, x0 = 0, z0 = 0;  // corner of cell (0, 0)
    int cols = 0, rows = 0;
    std::vector<uint8_t> cells;   // Habitat
    std::vector<uint8_t> water;   // cells to the nearest water, capped at 255
    std::vector<uint8_t> built;   // built share around, 0..255
    int index(double x, double z) const;       // -1 outside
    Habitat at(double x, double z) const;
    double waterDistance(double x, double z) const;
    double builtShare(double x, double z) const;
    P2 centre(int i) const { return {x0 + (i % cols + 0.5) * cell, z0 + (i / cols + 0.5) * cell}; }
    // Water distance and built share from the cells (run once filled).
    void finish();
    nlohmann::json json() const;
    static HabitatGrid from(const nlohmann::json& j);
};

// How well a cell suits a species where `range` holds, 0..1-ish.
double suitability(const Species& s, const RangeAnswer& range, const HabitatGrid& grid, int cell);

// A group that lives on a tile: what it is, where its home is, and its
// members' models. Drawn from the tile's key.
struct FaunaResident {
    std::string species;
    P2 home;                       // tile frame (x, z)
    std::vector<std::string> models;
    uint64_t identity = 0;         // stable across visits and players
};

// Cook side: the tile's grid and residents, as the manifest's "fauna" entry.
// `classify` names the ground class at (lon, lat) as the terrain is drawn
// (gen/cook.cpp), `family` the photographed family of a class.
struct FaunaInput {
    Tile tile;
    Bounds bounds;
    const Anchor* anchor = nullptr;
    std::function<std::string(double, double)> classify;
    std::function<std::optional<std::string>(const std::string&)> family;
    const std::vector<Ring>* footprints = nullptr;   // engine frame
    const std::vector<OsmWay>* landcover = nullptr;
    std::string country, climate;
    bool ocean = false, seaIce = false;
};
nlohmann::json planFauna(const FaunaInput& in);
std::vector<FaunaResident> readResidents(const nlohmann::json& fauna);

// Indoor occupants of one furnished interior: rare, and drawn from the
// building, the species and the day, so a house keeps its spider all day.
struct IndoorAnimal {
    std::string species, model, place;  // place: wall, floor, air, trail
    P2 at;                              // tile frame
    double height = 0;                  // above the floor
    P2 normal{0, 0};                    // a wall's, pointing into the room
    double yaw = 0;                     // radians, atan2(x, z) of its facing
    int count = 1;                      // a trail's ants
    double length = 0;                  // a trail's length, metres
};
struct IndoorContext {
    std::string country, climate;
    double lon = 0, lat = 0, hour = 12;
    int month = 6;
    int64_t day = 0;                    // days since 1970, local
    bool rural = false;
};
std::vector<IndoorAnimal> planIndoor(const InteriorPlan& plan, const InteriorLayout& layout, const IndoorContext& context);

}  // namespace r1
