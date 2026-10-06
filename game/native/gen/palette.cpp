#include "palette.hpp"

#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>

namespace r1 {

namespace {
std::unique_ptr<Palette> gPalette;

nlohmann::json readJsonFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    return nlohmann::json::parse(f);
}

Swatch swatchFrom(const nlohmann::json& j) {
    Swatch s;
    s.name = j.at("name").get<std::string>();
    for (int i = 0; i < 3; ++i) s.color[i] = j.at("color")[i].get<double>();
    s.roughness = j.at("roughness").get<double>();
    s.weight = j.value("weight", 1.0);
    return s;
}

std::vector<std::pair<std::string, std::vector<std::string>>> selectors(const nlohmann::json& j) {
    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    for (const auto& pair : j) out.push_back({pair[0].get<std::string>(), pair[1].get<std::vector<std::string>>()});
    return out;
}

bool contains(const std::vector<std::string>& values, const std::string& v) {
    for (const auto& x : values) if (x == v) return true;
    return false;
}
}  // namespace

const Palette& palette() {
    if (!gPalette) throw std::runtime_error("the palette was not loaded (loadPalette)");
    return *gPalette;
}

void loadPalette(const std::string& gameRoot) {
    auto p = std::make_unique<Palette>();
    p->gameRoot = gameRoot;
    const auto atlas = readJsonFile(gameRoot + "/assets/world/atlas.json");
    p->surfaces = readJsonFile(gameRoot + "/assets/textures/surfaces.json");
    for (const auto& j : atlas.at("profiles")) {
        RegionProfile r;
        r.key = j.at("key"); r.name = j.at("name"); r.tier = j.at("tier"); r.climate = j.at("climate");
        r.storeyHeight = j.at("storey_height"); r.groundStoreyHeight = j.at("ground_storey_height");
        for (const auto& w : j.at("storey_weights")) r.storeyWeights.push_back({w[0].get<int>(), w[1].get<double>()});
        for (const auto& w : j.at("roof_shape_weights"))
            r.roofShapeWeights.push_back({w[0].get<std::string>(), w[1].get<double>()});
        r.roofPitchLow = j.at("roof_pitch_range")[0]; r.roofPitchHigh = j.at("roof_pitch_range")[1];
        r.eaveOverhang = j.at("eave_overhang"); r.parapetHeight = j.at("parapet_height");
        r.bayWidth = j.at("bay_width"); r.windowWidth = j.at("window_width");
        r.windowHeight = j.at("window_height"); r.windowSill = j.at("window_sill");
        r.windowInset = j.at("window_inset"); r.pierMin = j.at("pier_min"); r.lintelMin = j.at("lintel_min");
        r.shopfrontWidthRatio = j.at("shopfront_width_ratio"); r.shopfrontHeight = j.at("shopfront_height");
        r.shopfrontSill = j.at("shopfront_sill"); r.shopfrontPierMin = j.at("shopfront_pier_min");
        for (const auto& s : j.at("walls")) r.walls.push_back(swatchFrom(s));
        for (const auto& s : j.at("roofs")) r.roofs.push_back(swatchFrom(s));
        r.trim = swatchFrom(j.at("trim")); r.glass = swatchFrom(j.at("glass"));
        r.ground = swatchFrom(j.at("ground")); r.foliage = swatchFrom(j.at("foliage"));
        r.bark = swatchFrom(j.at("bark"));
        r.treeModels = j.at("tree_models").get<std::vector<std::string>>();
        r.commercialKeys = j.at("commercial_keys").get<std::vector<std::string>>();
        r.commercialBuildingValues = j.at("commercial_building_values").get<std::vector<std::string>>();
        p->profiles.push_back(std::move(r));
    }
    for (const auto& r : p->profiles) if (r.key == "GENERIC") p->generic = &r;
    if (!p->generic) throw std::runtime_error("atlas.json has no GENERIC profile");
    for (const auto& j : atlas.at("regions")) {
        const std::string key = j.at("profile");
        const RegionProfile* found = nullptr;
        for (const auto& r : p->profiles) if (r.key == key) found = &r;
        if (!found) throw std::runtime_error("atlas.json region names unknown profile " + key);
        std::array<double, 4> box{};
        for (int i = 0; i < 4; ++i) box[i] = j.at("box")[i].get<double>();
        p->regions.push_back({box, found});
    }
    for (const auto& j : atlas.at("ground").at("classes"))
        p->groundClasses.push_back({j.at("name"), swatchFrom(j.at("swatch")), selectors(j.at("tags"))});
    p->snow = swatchFrom(atlas.at("ground").at("snow"));
    p->frost = swatchFrom(atlas.at("ground").at("frost"));
    for (const auto& [key, j] : atlas.at("ground").at("seaIce").items()) {
        if (!j.is_object()) continue;  // the note
        p->seaIce[key] = {swatchFrom(j), j.at("family").is_null() ? std::string() : j.at("family").get<std::string>()};
    }
    for (const auto& j : atlas.at("props")) {
        PropKind k;
        k.name = j.at("name"); k.models = j.at("models").get<std::vector<std::string>>();
        k.height = j.at("height"); k.share = j.at("share"); k.priority = j.at("priority");
        k.facesRoad = j.at("facesRoad"); k.jitter = j.at("jitter"); k.tags = selectors(j.at("tags"));
        p->props.push_back(std::move(k));
    }
    for (const auto& j : atlas.at("boats")) {
        BoatKind b;
        b.name = j.at("name"); b.model = j.at("model"); b.kind = j.at("kind");
        b.length = j.at("length"); b.top = j.at("top"); b.accel = j.at("accel"); b.turn = j.at("turn");
        b.lengthRatio = j.at("length_ratio").is_null() ? 0.0 : j.at("length_ratio").get<double>();
        b.draft = j.at("draft");
        p->boats.push_back(std::move(b));
    }
    for (const auto& j : atlas.at("carPaints"))
        p->carPaints.push_back({j.at("albedo")[0].get<double>(), j.at("albedo")[1].get<double>(), j.at("albedo")[2].get<double>()});
    const auto signs = readJsonFile(gameRoot + "/assets/world/signs.json");
    for (const auto& [code, j] : signs.at("signs").items())
        for (const auto& [mount, m] : j.at("mounts").items()) p->signs[code].mounts[mount] = m.at("model").get<std::string>();
    if (p->signs.empty()) throw std::runtime_error("assets/world/signs.json lists no sign");
    auto kitOf = [](const nlohmann::json& j) {
        TownSignKit k;
        k.cap = j.at("cap"); k.line = j.at("line"); k.pad = j.at("pad"); k.end = j.at("end");
        k.lowerEdge = j.at("lowerEdge"); k.post = j.at("post"); k.bar = j.at("bar");
        for (const auto& [lines, plate] : j.at("plates").items())
            k.plates[std::stoi(lines)] = {plate.at("left"), plate.at("middle"), plate.at("right"), plate.at("height")};
        for (const auto& [ch, g] : j.at("glyphs").items()) k.glyphs[ch] = {g.at("model"), g.at("advance").get<double>()};
        return k;
    };
    for (const auto& [country, j] : signs.at("streets").items()) p->streetSigns[country] = kitOf(j);
    for (const auto& [country, j] : signs.at("towns").items()) {
        TownSignKit k;
        k.cap = j.at("cap"); k.line = j.at("line"); k.pad = j.at("pad"); k.end = j.at("end");
        k.lowerEdge = j.at("lowerEdge"); k.post = j.at("post"); k.bar = j.at("bar");
        for (const auto& [lines, plate] : j.at("plates").items())
            k.plates[std::stoi(lines)] = {plate.at("left"), plate.at("middle"), plate.at("right"), plate.at("height")};
        for (const auto& [ch, g] : j.at("glyphs").items()) k.glyphs[ch] = {g.at("model"), g.at("advance").get<double>()};
        p->townSigns[country] = std::move(k);
    }
    const auto fleet = readJsonFile(gameRoot + "/assets/models/aircraft/fleet.json");
    for (const auto& j : fleet.at("aircraft")) {
        AircraftType a;
        a.name = j.at("name"); a.klass = j.at("class");
        a.length = j.at("length"); a.span = j.at("span"); a.height = j.at("height"); a.cg = j.at("cg");
        a.nearModel = j.at("near").at("path"); a.farModel = j.at("far").at("path");
        a.nearVertices = j.at("near").at("vertices"); a.farVertices = j.at("far").at("vertices");
        const auto& h = j.at("handling");
        a.top = h.at("top"); a.rotate = h.at("rotate"); a.stall = h.at("stall"); a.accel = h.at("accel");
        a.spool = h.at("spool"); a.brake = h.at("brake"); a.turnRadius = h.at("turnRadius");
        a.rollRate = h.at("rollRate"); a.maxBank = h.at("maxBank"); a.pitchRate = h.at("pitchRate");
        a.maxPitch = h.at("maxPitch"); a.climb = h.at("climb");
        p->aircraft.push_back(std::move(a));
    }
    if (p->aircraft.empty()) throw std::runtime_error("assets/models/aircraft/fleet.json lists no aircraft");
    gPalette = std::move(p);
}

const AircraftType* aircraftType(const std::string& name) {
    for (const auto& a : palette().aircraft) if (a.name == name) return &a;
    return nullptr;
}

// ── profiles ────────────────────────────────────────────────────────────────

const Swatch& RegionProfile::pick(const std::vector<Swatch>& swatches, PyRandom& rng) {
    double total = 0;
    for (const auto& s : swatches) total += s.weight;
    double cursor = rng.random() * total;
    for (const auto& s : swatches) {
        cursor -= s.weight;
        if (cursor <= 0.0) return s;
    }
    return swatches.back();
}

std::string RegionProfile::roofShape(PyRandom& rng) const {
    double total = 0;
    for (const auto& [shape, weight] : roofShapeWeights) total += weight;
    double cursor = rng.random() * total;
    for (const auto& [shape, weight] : roofShapeWeights) {
        cursor -= weight;
        if (cursor <= 0.0) return shape;
    }
    return roofShapeWeights.back().first;
}

int RegionProfile::storeys(PyRandom& rng) const {
    double total = 0;
    for (const auto& [count, weight] : storeyWeights) total += weight;
    double cursor = rng.random() * total;
    for (const auto& [count, weight] : storeyWeights) {
        cursor -= weight;
        if (cursor <= 0.0) return count;
    }
    return storeyWeights.back().first;
}

bool RegionProfile::isCommercial(const Tags& tags) const {
    for (const auto& k : commercialKeys) if (tags.count(k)) return true;
    return contains(commercialBuildingValues, tagOr(tags, "building"));
}

const RegionProfile& profileFor(double lon, double lat) {
    const Palette& p = palette();
    for (const auto& [box, profile] : p.regions)
        if (box[0] <= lon && lon <= box[2] && box[1] <= lat && lat <= box[3]) return *profile;
    return *p.generic;
}

const RegionProfile& profileByKey(const std::string& key) {
    for (const auto& r : palette().profiles) if (r.key == key) return r;
    return *palette().generic;
}

// ── ground ──────────────────────────────────────────────────────────────────

std::string classifyWay(const Tags& tags) {
    for (const auto& c : palette().groundClasses)
        for (const auto& [key, values] : c.tags) {
            auto it = tags.find(key);
            if (it != tags.end() && (contains(values, "*") || contains(values, it->second))) return c.name;
        }
    return {};
}

Landcover::Landcover(const std::vector<OsmWay>& ways) {
    const auto& classes = palette().groundClasses;
    for (const auto& way : ways) {
        const std::string name = classifyWay(way.tags);
        if (name.empty() || (name == "water" && hiddenWater(way.tags))) continue;
        std::vector<P2> ring = way.points;
        if (ring.front() == ring.back()) ring.pop_back();
        if (ring.size() < 3) continue;
        Area a{0, nullptr, std::move(ring), 1e300, 1e300, -1e300, -1e300};
        for (size_t i = 0; i < classes.size(); ++i)
            if (classes[i].name == name) { a.rank = int(i); a.name = &classes[i].name; }
        for (const P2& p : a.ring) {
            a.west = std::min(a.west, p.x); a.east = std::max(a.east, p.x);
            a.south = std::min(a.south, p.y); a.north = std::max(a.north, p.y);
        }
        areas_.push_back(std::move(a));
    }
    std::stable_sort(areas_.begin(), areas_.end(), [](const Area& a, const Area& b) { return a.rank < b.rank; });
}

const std::string* Landcover::at(double lon, double lat) const {
    for (const Area& a : areas_) {
        if (!(a.west <= lon && lon <= a.east && a.south <= lat && lat <= a.north)) continue;
        bool inside = false;
        P2 previous = a.ring.back();
        for (const P2& current : a.ring) {
            if ((current.y > lat) != (previous.y > lat)) {
                const double span = previous.y - current.y;
                if (span != 0.0 && lon < current.x + (previous.x - current.x) * (lat - current.y) / span)
                    inside = !inside;
            }
            previous = current;
        }
        if (inside) return a.name;
    }
    return nullptr;
}

const Swatch& groundSwatch(const std::string& name, const RegionProfile& profile) {
    const auto at = name.find('@');
    const std::string base = name.substr(0, at), cold = at == std::string::npos ? "" : name.substr(at + 1);
    const Palette& p = palette();
    if (cold == "snow" && base != "water") return p.snow;
    if (cold == "frost" && base != "water" && base != "urban" && base != "rock" && base != "sand") return p.frost;
    if (base == kInferred) return profile.ground;
    for (const auto& c : p.groundClasses) if (c.name == base) return c.swatch;
    throw std::runtime_error("unknown ground class " + base);
}

// ── surfaces ────────────────────────────────────────────────────────────────

std::string climateAt(const std::string& profileClimate, double lat) {
    const double a = std::abs(lat);
    if (a >= 66.5) return "polar";
    if (a >= 58.0 && (profileClimate == "temperate" || profileClimate == "mediterranean")) return "boreal";
    return profileClimate;
}

double snowline(double lat) {
    const double a = std::abs(lat);
    if (a <= 20.0) return 5500.0;
    return std::max(0.0, 5500.0 * (72.0 - a) / 52.0);
}

std::string coldSuffix(double lat, double altitude, const std::string& climate) {
    const double line = snowline(lat);
    if (altitude >= line) return "@snow";
    if (altitude >= line - 350.0 || climate == "polar") return "@frost";
    return "";
}

namespace {
// The ground each class is made of, per climate (surfaces._GROUND).
const char* kClimates[] = {"tropical", "arid", "mediterranean", "temperate", "boreal", "polar"};
struct GroundRow { const char* base; const char* families[6]; };
const GroundRow kGround[] = {
    {"glacier", {"snow", "snow", "snow", "snow", "snow", "snow"}},
    {"rock", {"rock", "soil_arid", "rock", "rock", "rock", "rock"}},
    {"sand", {"sand_beach", "sand_desert", "sand_beach", "sand_beach", "sand_beach", "sand_beach"}},
    {"wetland", {"forest_tropical", "mud", "mud", "mud", "mud", "frost"}},
    {"forest", {"forest_tropical", "soil_arid", "forest_temperate", "forest_temperate", "forest_boreal", "forest_boreal"}},
    {"scrub", {"savanna", "soil_arid", "grass_dry", "grass_dry", "forest_boreal", "frost"}},
    {"orchard", {"grass_lush", "grass_dry", "grass_dry", "grass", "grass", "frost"}},
    {"grass", {"grass_lush", "grass_dry", "grass_dry", "grass", "grass", "frost"}},
    {"farmland", {"farmland", "cracked_earth", "farmland", "farmland", "farmland", "frost"}},
    {"bare", {"savanna", "cracked_earth", "bare", "bare", "bare", "bare"}},
    {"urban", {"made_ground", "made_ground", "made_ground", "made_ground", "made_ground", "made_ground"}},
    {"airfield", {"grass_lush", "grass_dry", "grass_dry", "grass", "grass", "frost"}},
};
const std::pair<const char*, const char*> kInferredGround[] = {
    {"Ground, temperate", "grass"}, {"Ground, dry grass", "grass_dry"},
    {"Ground, desert sand", "sand_desert"}, {"Ground, arid", "soil_arid"},
    {"Ground, stony arid", "soil_arid"}, {"Ground, dry savanna", "savanna"},
    {"Ground, humid tropics", "tropical_ground"}, {"Ground, subtropical", "grass_lush"},
    {"Ground, cultivated", "farmland"}, {"Ground, taiga", "forest_boreal"},
    {"Ground, boreal forest", "forest_boreal"}, {"Ground, dry bush", "sand_red"},
    {"Made ground", "made_ground"},
};
bool any(const std::string& n, std::initializer_list<const char*> words) {
    for (const char* w : words) if (n.find(w) != std::string::npos) return true;
    return false;
}
std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
}  // namespace

std::optional<std::string> groundFamily(const std::string& name, const std::string& inferredGround,
                                        const std::string& climate) {
    const auto at = name.find('@');
    const std::string base = name.substr(0, at), cold = at == std::string::npos ? "" : name.substr(at + 1);
    if (base == "water") return std::nullopt;
    if (cold == "snow") return std::string("snow");
    if (cold == "frost" && base != "urban" && base != "rock" && base != "sand") return std::string("frost");
    if (base == kInferred) {
        std::string family = "grass";
        for (const auto& [swatch, f] : kInferredGround) if (inferredGround == swatch) family = f;
        if (climate == "polar" && (family == "grass" || family == "grass_dry" || family == "farmland"))
            return std::string("frost");
        return family;
    }
    for (const auto& row : kGround)
        if (base == row.base)
            for (int i = 0; i < 6; ++i) if (climate == kClimates[i]) return std::string(row.families[i]);
    return std::nullopt;
}

std::string wallFamily(const std::string& swatchName) {
    const std::string n = lower(swatchName);
    if (any(n, {"glass", "curtain", "glazing"})) return "concrete_panel";
    if (any(n, {"corrugated"})) return "corrugated";
    if (any(n, {"painted brick"})) return "render";
    if (any(n, {"brick", "brique"})) {
        if (any(n, {"dark"})) return "brick_dark";
        if (any(n, {"buff", "stock", "cream", "yellow"})) return "brick_buff";
        return "brick_red";
    }
    if (any(n, {"rendered stone"})) return "render_rough";
    if (any(n, {"pierre", "stone", "limestone", "travertine"})) return "stone";
    if (any(n, {"clapboard", "weatherboard", "siding", "painted timber", "falu"})) return "siding";
    if (any(n, {"timber", "larch"})) return "timber";
    if (any(n, {"earth"})) return "earth";
    if (any(n, {"panel", "tile cladding", "blockwork"})) return "concrete_panel";
    if (any(n, {"concrete"})) return "concrete";
    if (any(n, {"whitewash", "cal,", "lime", "pebbledash"})) return "render_rough";
    return "render";
}

std::string roofFamily(const std::string& swatchName) {
    const std::string n = lower(swatchName);
    if (any(n, {"slate", "ardoise"})) return "slate";
    if (any(n, {"kawara", "glazed"})) return "tile_grey";
    if (any(n, {"corrugated"})) return "corrugated";
    if (any(n, {"zinc", "seam", "sheet", "galvan", "metal"})) return "metal_seam";
    if (any(n, {"shingle"})) return "shingle";
    if (any(n, {"concrete tile"})) return "concrete_tile";
    if (any(n, {"coppi", "teja", "pantile", "terracotta"})) return "tile_canal";
    if (any(n, {"tile", "tuile"})) return "tile_flat";
    if (any(n, {"membrane", "bitumen", "tar ", "tar and", "waterproof"})) return "membrane";
    if (any(n, {"deck", "terrace", "flat"})) return "terrace";
    return "tile_flat";
}

namespace {

// How a family varies across a surface (saida::SurfaceVariation), in its own
// repeats. A scan repeated over a square or a facade reads as wallpaper: the
// warp shifts every repeat off the last, the macro noise lays the slow
// patches of wear, damp and moss that no single scan holds. Its mean is zero,
// so the measured albedo stays the average (CLAUDE.md rule 2).
//  - Natural ground warps like the far rings' (terrain_material.glsl), and
//    varies over 64 m, 32 m and 16 m.
//  - Laid surfaces whose joints are straight lines -- slabs, setts, planks,
//    bricks, roof tiles -- never warp: a bent joint is worse than a repeat.
//  - A facade sheet is one bay by one storey: its patches span two bays.
struct Variation { double warp, macroMetres, albedo, slope; };
constexpr double kGroundWarp = 1.5;
constexpr Variation kGroundVariation{kGroundWarp, 64.0, 0.30, 0.0};
constexpr Variation kOpenStreetVariation{0.8, 24.0, 0.18, 0.0};
constexpr Variation kJointedVariation{0.0, 24.0, 0.15, 0.0};
constexpr Variation kRoofVariation{0.0, 8.0, 0.18, 0.0};
constexpr double kSheetMacroBays = 2.0, kSheetAlbedo = 0.10;

bool jointed(const std::string& family) {
    static const std::set<std::string> laid = {"pavement", "cobbles", "deck", "step_stone", "ashlar",
                                               "ashlar_rough", "ashlar_large", "brick_bond", "quay"};
    return laid.count(family) > 0;
}

std::array<double, 4> variationOf(const std::string& family, const nlohmann::json& entry) {
    const std::string kind = entry.at("kind").get<std::string>();
    const double uvSize = entry.at("uvSize").get<double>();
    if (kind == "wall") return {0.0, 1.0 / kSheetMacroBays, kSheetAlbedo, 0.0};
    const Variation v = kind == "ground" ? kGroundVariation
                        : kind == "roof" ? kRoofVariation
                        : jointed(family) ? kJointedVariation : kOpenStreetVariation;
    return {v.warp, uvSize / v.macroMetres, v.albedo, v.slope};
}

}  // namespace

Material surfaceMaterial(const std::string& name, std::array<double, 3> color, double roughness,
                         const std::optional<std::string>& family, bool doubleSided) {
    Material m;
    m.name = name; m.roughness = roughness; m.doubleSided = doubleSided;
    m.color = {color[0], color[1], color[2], 1.0};
    if (!family) return m;
    const auto& families = palette().surfaces.at("families");
    auto it = families.find(*family);
    if (it == families.end()) return m;
    const double level = it->at("level").get<double>();
    m.color = {color[0] / level, color[1] / level, color[2] / level, 1.0};
    m.roughness = 1.0;
    m.baseColorTexture = it->at("albedo").get<std::string>();
    m.normalTexture = it->at("normal").get<std::string>();
    m.metallicRoughnessTexture = it->at("mr").get<std::string>();
    m.uvScale = 1.0 / it->at("uvSize").get<double>();
    m.variation = variationOf(*family, *it);
    if (it->contains("height")) {
        // Depth in metres, as repeats: a wall's repeat is a bay and a storey.
        const auto& bay = palette().surfaces.at("bay");
        const double repeat = it->at("kind").get<std::string>() == "wall"
                                  ? (bay.at(0).get<double>() + bay.at(1).get<double>()) / 2.0
                                  : it->at("uvSize").get<double>();
        m.heightTexture = it->at("height").get<std::string>();
        m.parallaxDepth = it->at("depth").get<double>() / repeat;
    }
    // A facade's window is glass: it shows the sky (its roughness keeps the
    // wall around it matt). From a street, about half of what a window faces
    // is the street and the buildings across it, darker than the sky the
    // engine reflects: the sky's share of the reflection.
    constexpr double kStreetSkyShare = 0.6;
    if (it->contains("window")) m.environmentReflection = kStreetSkyShare;
    return m;
}

}  // namespace r1
