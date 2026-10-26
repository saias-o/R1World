#include "fauna.hpp"

#include "palette.hpp"
#include "spatial.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace r1 {
namespace {

const char* kHabitatNames[kHabitats] = {
    "none", "water", "building", "forest", "scrub", "meadow", "park", "farmland", "farmyard", "orchard",
    "wetland", "beach", "desert", "savanna", "rock", "ice", "urban", "bare", "open"};

FaunaCatalog gCatalog;
bool gLoaded = false;

// FNV-1a: the same residents on every machine, unlike std::hash (PLAN §3 I3).
struct StableHash {
    uint64_t h = 1469598103934665603ull;
    StableHash& add(const std::string& s) {
        for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
        h = (h ^ 0xff) * 1099511628211ull;
        return *this;
    }
    StableHash& add(int64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ uint64_t((v >> (8 * i)) & 0xff)) * 1099511628211ull;
        return *this;
    }
};

bool monthIn(int month, std::pair<int, int> span) {
    return span.first <= span.second ? month >= span.first && month <= span.second
                                     : month >= span.first || month <= span.second;
}

std::optional<std::pair<int, int>> monthsOf(const nlohmann::json& j, const std::string& what) {
    if (j.is_null()) return std::nullopt;
    if (!j.is_array() || j.size() != 2) throw std::runtime_error("fauna.json: " + what + " months must be [first, last]");
    const int a = j[0].get<int>(), b = j[1].get<int>();
    if (a < 1 || a > 12 || b < 1 || b > 12) throw std::runtime_error("fauna.json: " + what + " months out of 1..12");
    return std::make_pair(a, b);
}

std::array<double, kHabitats> habitatTable(const nlohmann::json& j, const std::string& what) {
    std::array<double, kHabitats> out{};
    for (auto it = j.begin(); it != j.end(); ++it) {
        const Habitat h = habitatNamed(it.key());
        if (h == Habitat::None) throw std::runtime_error("fauna.json: " + what + " names no habitat \"" + it.key() + "\"");
        out[size_t(h)] = it.value().get<double>();
    }
    return out;
}

std::vector<FaunaModel> modelsOf(const nlohmann::json& list, const std::string& root, const std::string& what) {
    std::vector<FaunaModel> out;
    for (const auto& m : list) {
        FaunaModel model;
        model.model = m.at("model").get<std::string>();
        model.weight = m.value("weight", 1.0);
        model.young = m.value("young", false);
        for (const auto& c : m.value("countries", nlohmann::json::array())) model.countries.insert(c.get<std::string>());
        const auto path = std::filesystem::path(root) / "assets/animals" / (model.model + ".glb");
        if (!std::filesystem::exists(path))
            throw std::runtime_error("fauna.json: " + what + " places " + model.model + ", which has no GLB in assets/animals");
        out.push_back(std::move(model));
    }
    return out;
}

// A cell's habitat from the ground class the terrain is drawn with, its
// photographed family, and the land cover's own tags where the class lumps
// together what animals tell apart (a park and a pasture are both "grass").
struct CoverIndex {
    struct Area { Habitat habitat; Ring ring; Box box; };
    std::vector<Area> areas;
    explicit CoverIndex(const std::vector<OsmWay>* ways) {
        if (!ways) return;
        for (const OsmWay& w : *ways) {
            Habitat h = Habitat::None;
            const auto tag = [&](const char* k) { auto it = w.tags.find(k); return it == w.tags.end() ? std::string() : it->second; };
            const std::string landuse = tag("landuse"), leisure = tag("leisure"), natural = tag("natural");
            if (landuse == "farmyard") h = Habitat::Farmyard;
            else if (leisure == "park" || leisure == "garden" || leisure == "pitch" || leisure == "golf_course" ||
                     leisure == "dog_park" || leisure == "playground" || landuse == "cemetery" ||
                     landuse == "recreation_ground" || landuse == "village_green" || landuse == "flowerbed" ||
                     tag("amenity") == "grave_yard")
                h = Habitat::Park;
            else if (landuse == "meadow" || landuse == "pasture" || natural == "grassland" || natural == "fell")
                h = Habitat::Meadow;
            if (h == Habitat::None || w.points.size() < 4) continue;
            Area a{h, w.points, boxOf(w.points)};
            if (a.ring.front() == a.ring.back()) a.ring.pop_back();
            areas.push_back(std::move(a));
        }
    }
    // The finest cover tagged at (lon, lat): a park inside a meadow is a park.
    Habitat at(double lon, double lat) const {
        Habitat found = Habitat::None;
        for (const Area& a : areas)
            if (a.box.contains({lon, lat}) && pointInPolygon({lon, lat}, a.ring)) {
                if (a.habitat == Habitat::Park || a.habitat == Habitat::Farmyard) return a.habitat;
                found = a.habitat;
            }
        return found;
    }
};

Habitat habitatOfClass(const std::string& drawn, const std::optional<std::string>& family, Habitat cover,
                       const std::string& climate) {
    std::string base = drawn.substr(0, drawn.find('@'));
    const bool cold = drawn.find("@snow") != std::string::npos;
    if (base == "water") return Habitat::Water;
    if (cold) return Habitat::Ice;
    if (base.rfind("inferred:", 0) == 0) base = base.substr(9);
    const std::string f = family.value_or("");
    if (base == "glacier") return Habitat::Ice;
    if (base == "rock") return Habitat::Rock;
    if (base == "sand") return f == "sand_desert" ? Habitat::Desert : Habitat::Beach;
    if (base == "wetland") return Habitat::Wetland;
    if (base == "forest") return f == "soil_arid" ? Habitat::Scrub : Habitat::Forest;
    if (base == "scrub") return f == "savanna" ? Habitat::Savanna : Habitat::Scrub;
    if (base == "orchard") return Habitat::Orchard;
    if (base == "grass") return cover == Habitat::Park ? Habitat::Park : Habitat::Meadow;
    if (base == "farmland") return cover == Habitat::Farmyard ? Habitat::Farmyard : Habitat::Farmland;
    if (base == "bare") return climate == "arid" ? Habitat::Desert : Habitat::Bare;
    if (base == "urban") return Habitat::Urban;
    if (base == "airfield") return Habitat::Open;
    // Unmapped ground: what the region's ground is drawn as (gen/palette.cpp).
    if (f == "forest_boreal" || f == "forest_temperate" || f == "forest_tropical") return Habitat::Forest;
    if (f == "savanna") return Habitat::Savanna;
    if (f == "sand_desert" || f == "soil_arid" || f == "cracked_earth" || f == "sand_red") return Habitat::Desert;
    if (f == "made_ground") return Habitat::Urban;
    if (f == "frost") return Habitat::Scrub;  // tundra
    if (f == "snow") return Habitat::Ice;
    if (cover == Habitat::Park || cover == Habitat::Meadow || cover == Habitat::Farmyard) return cover;
    return Habitat::Open;
}

}  // namespace

const char* habitatName(Habitat h) { return kHabitatNames[size_t(h) < kHabitats ? size_t(h) : 0]; }

Habitat habitatNamed(const std::string& name) {
    for (size_t i = 1; i < kHabitats; ++i)
        if (name == kHabitatNames[i]) return Habitat(i);
    return Habitat::None;
}

void loadFauna(const std::string& gameRoot) {
    const auto path = std::filesystem::path(gameRoot) / "assets/animals/fauna.json";
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Missing assets/animals/fauna.json");
    nlohmann::json doc;
    try { doc = nlohmann::json::parse(in); }
    catch (const std::exception& e) { throw std::runtime_error(std::string("assets/animals/fauna.json: ") + e.what()); }
    FaunaCatalog cat;
    for (auto it = doc.at("activity").begin(); it != doc.at("activity").end(); ++it) {
        if (it.value().size() != 24) throw std::runtime_error("fauna.json: activity " + it.key() + " needs 24 hours");
        std::array<double, 24> hours{};
        for (size_t h = 0; h < 24; ++h) hours[h] = it.value()[h].get<double>();
        cat.activity[it.key()] = hours;
    }
    for (auto it = doc.at("settlement").begin(); it != doc.at("settlement").end(); ++it) {
        if (!it.value().is_object()) continue;
        FaunaCatalog::Settlement s;
        s.falloff = it.value().value("falloff", 0.0);
        s.power = it.value().value("power", 1.0);
        s.needs = it.value().value("needs", 0.0);
        cat.settlement[it.key()] = s;
    }
    std::map<std::string, std::vector<std::string>> regions;
    for (auto it = doc.at("regions").begin(); it != doc.at("regions").end(); ++it)
        regions[it.key()] = it.value().get<std::vector<std::string>>();
    auto countries = [&](const nlohmann::json& list, const std::string& what) {
        std::set<std::string> out;
        for (const auto& c : list) {
            const std::string code = c.get<std::string>();
            if (code.rfind("@", 0) == 0) {
                const auto found = regions.find(code.substr(1));
                if (found == regions.end()) throw std::runtime_error("fauna.json: " + what + " names no region " + code);
                out.insert(found->second.begin(), found->second.end());
            } else out.insert(code);
        }
        return out;
    };
    std::set<std::string> ids;
    for (const auto& j : doc.at("species")) {
        Species s;
        s.id = j.at("id").get<std::string>();
        if (!ids.insert(s.id).second) throw std::runtime_error("fauna.json: species " + s.id + " twice");
        s.name = j.at("name").get<std::string>();
        s.scientific = j.value("scientific", std::string());
        s.klass = j.value("class", std::string());
        s.domestic = j.value("domestic", false);
        s.activity = j.at("activity").get<std::string>();
        if (!cat.activity.count(s.activity)) throw std::runtime_error("fauna.json: " + s.id + " has no activity " + s.activity);
        s.settlement = j.at("settlement").get<std::string>();
        if (!cat.settlement.count(s.settlement)) throw std::runtime_error("fauna.json: " + s.id + " has no settlement " + s.settlement);
        for (const auto& r : j.at("range")) {
            FaunaRange range;
            range.countries = countries(r.at("countries"), s.id);
            range.exclude = countries(r.value("exclude", nlohmann::json::array()), s.id);
            for (const auto& c : range.exclude) range.countries.erase(c);
            if (r.contains("box")) {
                const auto b = r.at("box").get<std::vector<double>>();
                if (b.size() != 4 || b[0] >= b[2] || b[1] >= b[3]) throw std::runtime_error("fauna.json: " + s.id + " has a bad box");
                range.box = std::array<double, 4>{b[0], b[1], b[2], b[3]};
            }
            if (r.contains("absLat")) {
                range.absLatLow = r.at("absLat")[0].get<double>();
                range.absLatHigh = r.at("absLat")[1].get<double>();
            }
            range.factor = r.value("factor", 1.0);
            const nlohmann::json overrides = r.value("habitats", nlohmann::json::object());
            for (auto h = overrides.begin(); h != overrides.end(); ++h) {
                const Habitat named = habitatNamed(h.key());
                if (named == Habitat::None) throw std::runtime_error("fauna.json: " + s.id + " range names no habitat " + h.key());
                range.habitats[named] = h.value().get<double>();
            }
            range.months = monthsOf(r.value("months", nlohmann::json()), s.id + " range");
            s.range.push_back(std::move(range));
        }
        for (const auto& c : j.value("climates", nlohmann::json::array())) s.climates.insert(c.get<std::string>());
        s.habitats = habitatTable(j.at("habitats"), s.id);
        s.water = j.value("water", 0.0);
        s.density = j.at("density").get<double>();
        s.detectability = j.at("detectability").get<double>();
        s.groupMin = j.at("group")[0].get<int>();
        s.groupMax = j.at("group")[1].get<int>();
        if (s.groupMin < 1 || s.groupMax < s.groupMin) throw std::runtime_error("fauna.json: " + s.id + " has a bad group size");
        s.months = monthsOf(j.value("months", nlohmann::json()), s.id);
        s.offSeason = j.value("offSeason", 0.0);
        s.rain = j.value("rain", 1.0);
        const auto& b = j.at("behaviour");
        s.flee = b.value("flee", 0.0);
        s.dart = b.value("dart", false);
        s.spread = b.value("spread", 4.0);
        s.home = b.value("home", 60.0);
        s.surface = b.value("surface", std::string("ground"));
        if (b.contains("hover")) {
            s.hoverLow = b.at("hover")[0].get<double>();
            s.hoverHigh = b.at("hover")[1].get<double>();
        }
        s.scale = j.value("scale", 1.0);
        s.models = modelsOf(j.at("models"), gameRoot, s.id);
        if (s.models.empty()) throw std::runtime_error("fauna.json: " + s.id + " places no model");
        s.indoorModels = modelsOf(j.value("indoorModels", nlohmann::json::array()), gameRoot, s.id);
        if (j.contains("indoor")) {
            const auto& i = j.at("indoor");
            s.indoor.homes = i.value("homes", 0.0);
            s.indoor.night = i.value("night", 1.0);
            s.indoor.rural = i.value("rural", 1.0);
            s.indoor.rooms = i.value("rooms", std::vector<std::string>{});
            s.indoor.place = i.value("place", std::string("floor"));
            const nlohmann::json months = i.value("months", nlohmann::json::object());
            for (auto m = months.begin(); m != months.end(); ++m)
                s.indoor.months[std::stoi(m.key())] = m.value().get<double>();
        }
        cat.species.push_back(std::move(s));
    }
    gCatalog = std::move(cat);
    gLoaded = true;
}

const FaunaCatalog& fauna() {
    if (!gLoaded) throw std::runtime_error("fauna() before loadFauna()");
    return gCatalog;
}

const Species* speciesById(const std::string& id) {
    for (const auto& s : fauna().species)
        if (s.id == id) return &s;
    return nullptr;
}

RangeAnswer rangeAt(const Species& s, const std::string& country, double lon, double lat, const std::string& climate) {
    RangeAnswer out;
    if (!s.climates.empty() && !s.climates.count(climate)) return out;
    const double absLat = std::abs(lat);
    for (const FaunaRange& r : s.range) {
        if (!r.countries.count(country)) continue;
        if (r.box && !((*r.box)[0] <= lon && lon <= (*r.box)[2] && (*r.box)[1] <= lat && lat <= (*r.box)[3])) continue;
        if (absLat < r.absLatLow || absLat > r.absLatHigh) continue;
        if (!out.inside) out.factor = r.factor;
        else out.factor = std::max(out.factor, r.factor);
        out.inside = true;
        for (const auto& [h, w] : r.habitats) out.habitats[h] = w;
        if (r.months) out.months = r.months;
    }
    return out;
}

double activityAt(const std::string& activity, double hour) {
    const auto found = fauna().activity.find(activity);
    if (found == fauna().activity.end()) return 1.0;
    hour = std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0);
    const int h = int(hour) % 24;
    const double f = hour - std::floor(hour);
    return found->second[size_t(h)] * (1 - f) + found->second[size_t((h + 1) % 24)] * f;
}

double seasonAt(const Species& s, const RangeAnswer& range, int month, double lat) {
    // A range's own months hold wherever they are (monarchs in Michoacán in
    // winter); a species' calendar is the temperate one and means nothing
    // in the tropics.
    if (range.months) return monthIn(month, *range.months) ? 1.0 : 0.0;
    if (!s.months || std::abs(lat) < 23.44) return 1.0;
    const int local = lat < 0 ? (month + 5) % 12 + 1 : month;
    return monthIn(local, *s.months) ? 1.0 : s.offSeason;
}

double settlementFactor(const Species& s, double built) {
    const auto found = fauna().settlement.find(s.settlement);
    if (found == fauna().settlement.end()) return 1.0;
    const auto& rule = found->second;
    if (rule.needs > 0) return std::min(1.0, built / rule.needs);
    if (rule.falloff > 0) return std::pow(std::max(0.0, 1.0 - built / rule.falloff), rule.power);
    return 1.0;
}

// ── the habitat grid ────────────────────────────────────────────────────────

int HabitatGrid::index(double x, double z) const {
    const int c = int(std::floor((x - x0) / cell)), r = int(std::floor((z - z0) / cell));
    if (c < 0 || r < 0 || c >= cols || r >= rows) return -1;
    return r * cols + c;
}

Habitat HabitatGrid::at(double x, double z) const {
    const int i = index(x, z);
    return i < 0 ? Habitat::None : Habitat(cells[size_t(i)]);
}

double HabitatGrid::waterDistance(double x, double z) const {
    const int i = index(x, z);
    return i < 0 || water.empty() ? 1e9 : water[size_t(i)] * cell;
}

double HabitatGrid::builtShare(double x, double z) const {
    const int i = index(x, z);
    return i < 0 || built.empty() ? 0.0 : built[size_t(i)] / 255.0;
}

void HabitatGrid::finish() {
    const size_t n = cells.size();
    // Cells to water: a breadth-first wave from every water cell.
    water.assign(n, 255);
    std::deque<int> wave;
    for (size_t i = 0; i < n; ++i)
        if (Habitat(cells[i]) == Habitat::Water) { water[i] = 0; wave.push_back(int(i)); }
    while (!wave.empty()) {
        const int i = wave.front();
        wave.pop_front();
        const int r = i / cols, c = i % cols;
        for (int dr = -1; dr <= 1; ++dr)
            for (int dc = -1; dc <= 1; ++dc) {
                const int rr = r + dr, cc = c + dc;
                if ((!dr && !dc) || rr < 0 || cc < 0 || rr >= rows || cc >= cols) continue;
                const size_t j = size_t(rr * cols + cc);
                if (water[j] > water[size_t(i)] + 1) { water[j] = uint8_t(water[size_t(i)] + 1); wave.push_back(int(j)); }
            }
    }
    // Built share: buildings and urban ground within kFaunaBuiltRadius,
    // through a summed-area table.
    std::vector<int> sums(size_t(rows + 1) * size_t(cols + 1), 0);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const Habitat h = Habitat(cells[size_t(r * cols + c)]);
            const int v = h == Habitat::Building || h == Habitat::Urban;
            sums[size_t(r + 1) * size_t(cols + 1) + size_t(c + 1)] = v + sums[size_t(r) * size_t(cols + 1) + size_t(c + 1)] +
                sums[size_t(r + 1) * size_t(cols + 1) + size_t(c)] - sums[size_t(r) * size_t(cols + 1) + size_t(c)];
        }
    const int k = std::max(1, int(std::lround(kFaunaBuiltRadius / cell)));
    built.assign(n, 0);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const int r0 = std::max(0, r - k), r1 = std::min(rows, r + k + 1), c0 = std::max(0, c - k), c1 = std::min(cols, c + k + 1);
            const int area = (r1 - r0) * (c1 - c0);
            const int count = sums[size_t(r1) * size_t(cols + 1) + size_t(c1)] - sums[size_t(r0) * size_t(cols + 1) + size_t(c1)] -
                              sums[size_t(r1) * size_t(cols + 1) + size_t(c0)] + sums[size_t(r0) * size_t(cols + 1) + size_t(c0)];
            built[size_t(r * cols + c)] = uint8_t(std::lround(255.0 * count / std::max(1, area)));
        }
}

nlohmann::json HabitatGrid::json() const {
    std::string text(cells.size(), 'a');
    for (size_t i = 0; i < cells.size(); ++i) text[i] = char('a' + cells[i]);
    return {{"cell", cell}, {"x0", x0}, {"z0", z0}, {"cols", cols}, {"rows", rows}, {"cells", text},
            {"legend", [] { nlohmann::json l = nlohmann::json::array(); for (const char* n : kHabitatNames) l.push_back(n); return l; }()}};
}

HabitatGrid HabitatGrid::from(const nlohmann::json& j) {
    HabitatGrid g;
    if (!j.is_object() || !j.contains("cells")) return g;
    g.cell = j.value("cell", kFaunaCell);
    g.x0 = j.value("x0", 0.0);
    g.z0 = j.value("z0", 0.0);
    g.cols = j.value("cols", 0);
    g.rows = j.value("rows", 0);
    const std::string text = j.at("cells").get<std::string>();
    if (text.size() != size_t(g.cols) * size_t(g.rows)) return HabitatGrid{};
    g.cells.resize(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const int h = text[i] - 'a';
        g.cells[i] = uint8_t(h >= 0 && h < int(kHabitats) ? h : 0);
    }
    g.finish();
    return g;
}

double suitability(const Species& s, const RangeAnswer& range, const HabitatGrid& grid, int cell) {
    const Habitat h = Habitat(grid.cells[size_t(cell)]);
    if (h == Habitat::None || h == Habitat::Water || h == Habitat::Building) return 0.0;
    const auto over = range.habitats.find(h);
    const double w = over != range.habitats.end() ? over->second : s.habitats[size_t(h)];
    if (w <= 0) return 0.0;
    if (s.water > 0 && grid.water[size_t(cell)] * grid.cell > s.water) return 0.0;
    return w * settlementFactor(s, grid.built[size_t(cell)] / 255.0);
}

// ── cook ────────────────────────────────────────────────────────────────────

nlohmann::json planFauna(const FaunaInput& in) {
    HabitatGrid grid;
    nlohmann::json residents = nlohmann::json::array(), expected = nlohmann::json::object();
    nlohmann::json out = {{"revision", kFaunaRevision},
                          {"source", "inferred: fauna.json range x habitat x density x detectability"},
                          {"country", in.country}, {"climate", in.climate}};
    if (in.ocean && !in.seaIce) {
        out["grid"] = nullptr;
        out["residents"] = residents;
        return out;
    }
    const Anchor& anchor = *in.anchor;
    const Bounds& b = in.bounds;
    Box extent;
    for (const P2 corner : {P2{b.west, b.south}, P2{b.east, b.south}, P2{b.west, b.north}, P2{b.east, b.north}}) {
        const P3 e = anchor.toEngine(corner.x, corner.y, 0.0);
        extent.add(P2{e.x, e.z});
    }
    grid.x0 = std::floor(extent.x0 / kFaunaCell) * kFaunaCell;
    grid.z0 = std::floor(extent.y0 / kFaunaCell) * kFaunaCell;
    grid.cols = int(std::ceil((extent.x1 - grid.x0) / kFaunaCell));
    grid.rows = int(std::ceil((extent.y1 - grid.z0) / kFaunaCell));
    grid.cells.assign(size_t(grid.cols) * size_t(grid.rows), uint8_t(Habitat::None));
    BoxIndex buildings(32.0);
    if (in.footprints)
        for (size_t i = 0; i < in.footprints->size(); ++i) buildings.add(i, boxOf((*in.footprints)[i]));
    const CoverIndex cover(in.landcover);
    for (int i = 0; i < int(grid.cells.size()); ++i) {
        const P2 c = grid.centre(i);
        const P3 geo = anchor.toGeodetic(c.x, 0.0, c.y);
        if (geo.x < b.west || geo.x > b.east || geo.y < b.south || geo.y > b.north) continue;
        Habitat h = Habitat::None;
        if (in.seaIce) h = Habitat::Ice;
        else {
            bool inside = false;
            if (in.footprints)
                for (size_t f : buildings.near({c.x, c.y, c.x, c.y}))
                    if (pointInPolygon(c, (*in.footprints)[f])) { inside = true; break; }
            if (inside) h = Habitat::Building;
            else {
                const std::string drawn = in.classify(geo.x, geo.y);
                h = habitatOfClass(drawn, in.family ? in.family(drawn) : std::nullopt, cover.at(geo.x, geo.y), in.climate);
            }
        }
        grid.cells[size_t(i)] = uint8_t(h);
    }
    grid.finish();

    const P2 centre = in.tile.center();
    const std::string key = in.tile.key();
    const double cellKm2 = kFaunaCell * kFaunaCell / 1e6;
    struct Candidate { const Species* species; int group; uint64_t identity; FaunaResident resident; };
    std::vector<Candidate> drawn;
    for (const Species& s : fauna().species) {
        if (s.density <= 0 || s.detectability <= 0) continue;
        const RangeAnswer range = rangeAt(s, in.country, centre.x, centre.y, in.climate);
        if (!range.inside) continue;
        std::vector<double> cumulative(grid.cells.size(), 0.0);
        double total = 0;
        for (int i = 0; i < int(grid.cells.size()); ++i) {
            total += suitability(s, range, grid, i);
            cumulative[size_t(i)] = total;
        }
        if (total <= 0) continue;
        const double individuals = s.density * range.factor * total * cellKm2 * s.detectability;
        const double groups = individuals / (0.5 * (s.groupMin + s.groupMax));
        PyRandom rng(__int128(StableHash().add(key).add(s.id).h));
        int count = int(std::floor(groups));
        if (rng.random() < groups - count) ++count;
        count = std::min(count, kFaunaSpeciesGroups);
        expected[s.id] = {{"individuals", pyround(individuals, 4)}, {"groups", count}};
        std::vector<const FaunaModel*> adults, all;
        for (const FaunaModel& m : s.models)
            if (m.countries.empty() || m.countries.count(in.country)) {
                all.push_back(&m);
                if (!m.young) adults.push_back(&m);
            }
        if (adults.empty()) continue;
        auto pick = [&](const std::vector<const FaunaModel*>& from) {
            double sum = 0;
            for (const auto* m : from) sum += m->weight;
            double u = rng.random() * sum;
            for (const auto* m : from) if ((u -= m->weight) <= 0) return m->model;
            return from.back()->model;
        };
        for (int g = 0; g < count; ++g) {
            const double u = rng.random() * total;
            const int cell = int(std::lower_bound(cumulative.begin(), cumulative.end(), u) - cumulative.begin());
            const P2 home = grid.centre(std::min(cell, int(grid.cells.size()) - 1));
            FaunaResident r;
            r.species = s.id;
            r.home = {home.x + (rng.random() - 0.5) * kFaunaCell * 0.8, home.y + (rng.random() - 0.5) * kFaunaCell * 0.8};
            const int size = s.groupMin + int(rng.randrange(uint32_t(s.groupMax - s.groupMin + 1)));
            for (int m = 0; m < size; ++m) r.models.push_back(pick(m == 0 ? adults : all));
            r.identity = StableHash().add(key).add(s.id).add(int64_t(g)).h;
            drawn.push_back({&s, g, r.identity, std::move(r)});
        }
    }
    // A tile is never a zoo: past the cap, a stable choice keeps the rest.
    if (int(drawn.size()) > kFaunaTileGroups) {
        std::stable_sort(drawn.begin(), drawn.end(), [](const Candidate& a, const Candidate& b) { return a.identity < b.identity; });
        drawn.resize(size_t(kFaunaTileGroups));
    }
    nlohmann::json histogram = nlohmann::json::object();
    std::array<int, kHabitats> counts{};
    for (uint8_t c : grid.cells) ++counts[c];
    for (size_t h = 1; h < kHabitats; ++h)
        if (counts[h]) histogram[kHabitatNames[h]] = counts[h];
    for (const auto& c : drawn) {
        nlohmann::json models = nlohmann::json::array();
        for (const auto& m : c.resident.models) models.push_back(m);
        residents.push_back({{"species", c.resident.species}, {"home", {pyround(c.resident.home.x, 2), pyround(c.resident.home.y, 2)}},
                             {"models", models}, {"id", std::to_string(c.identity)}});
    }
    out["grid"] = grid.json();
    out["habitats"] = histogram;
    out["expected"] = expected;
    out["residents"] = residents;
    return out;
}

std::vector<FaunaResident> readResidents(const nlohmann::json& fauna) {
    std::vector<FaunaResident> out;
    if (!fauna.is_object()) return out;
    for (const auto& r : fauna.value("residents", nlohmann::json::array())) {
        FaunaResident f;
        f.species = r.at("species").get<std::string>();
        f.home = {r.at("home")[0].get<double>(), r.at("home")[1].get<double>()};
        f.models = r.at("models").get<std::vector<std::string>>();
        f.identity = std::stoull(r.at("id").get<std::string>());
        out.push_back(std::move(f));
    }
    return out;
}

// ── indoors ─────────────────────────────────────────────────────────────────

std::vector<IndoorAnimal> planIndoor(const InteriorPlan& plan, const InteriorLayout& layout, const IndoorContext& context) {
    std::vector<IndoorAnimal> out;
    if (plan.recipe != "home") return out;
    const bool night = context.hour < 6.0 || context.hour >= 21.0;
    // Fixtures stand in the plan's own axes; a floor animal keeps clear of them.
    auto clear = [&](P2 p) {
        const P2 local = plan.local(p);
        for (const auto& f : layout.fixtures)
            if (std::abs(local.x - f.at.x) < f.size.x * 0.5 + 0.08 && std::abs(local.y - f.at.y) < f.size.y * 0.5 + 0.08) return false;
        return true;
    };
    for (const Species& s : fauna().species) {
        if (s.indoor.homes <= 0) continue;
        const RangeAnswer range = rangeAt(s, context.country, context.lon, context.lat, context.climate);
        if (!range.inside) continue;
        double p = s.indoor.homes * seasonAt(s, range, context.month, context.lat);
        const auto month = s.indoor.months.find(context.month);
        if (month != s.indoor.months.end()) p *= month->second;
        if (night) p *= s.indoor.night;
        if (context.rural) p *= s.indoor.rural;
        PyRandom rng(__int128(StableHash().add(plan.id).add(s.id).add(context.day).h));
        if (rng.random() >= p) continue;
        std::vector<const InteriorRoom*> rooms;
        for (const auto& room : layout.rooms)
            if (room.ring.size() >= 3 && std::find(s.indoor.rooms.begin(), s.indoor.rooms.end(), room.use) != s.indoor.rooms.end())
                rooms.push_back(&room);
        if (rooms.empty()) continue;
        const InteriorRoom& room = *rooms[rng.randrange(uint32_t(rooms.size()))];
        P2 middle{0, 0};
        for (const P2& q : room.ring) middle = {middle.x + q.x / room.ring.size(), middle.y + q.y / room.ring.size()};
        const auto& models = s.indoorModels.empty() ? s.models : s.indoorModels;
        double sum = 0;
        for (const auto& m : models) sum += m.weight;
        double u = rng.random() * sum;
        std::string model = models.back().model;
        for (const auto& m : models) if ((u -= m.weight) <= 0) { model = m.model; break; }
        IndoorAnimal a;
        a.species = s.id;
        a.model = model;
        a.place = s.indoor.place;
        const double ceiling = plan.ceiling - plan.floor;
        bool placed = false;
        for (int attempt = 0; attempt < 12 && !placed; ++attempt) {
            const size_t edge = rng.randrange(uint32_t(room.ring.size()));
            const P2 e0 = room.ring[edge], e1 = room.ring[(edge + 1) % room.ring.size()];
            const P2 along{e1.x - e0.x, e1.y - e0.y};
            const double length = std::hypot(along.x, along.y);
            if (length < 1.2) continue;
            const P2 dir{along.x / length, along.y / length};
            P2 normal{-dir.y, dir.x};
            if ((middle.x - e0.x) * normal.x + (middle.y - e0.y) * normal.y < 0) normal = {-normal.x, -normal.y};
            const double t = 0.2 + 0.6 * rng.random();
            const P2 onWall{e0.x + along.x * t, e0.y + along.y * t};
            if (a.place == "wall") {
                // Up in a corner of the wall, where a house spider waits.
                a.at = {onWall.x + normal.x * 0.01, onWall.y + normal.y * 0.01};
                a.height = std::max(1.2, ceiling - 0.25 - 0.6 * rng.random());
                a.normal = normal;
                a.yaw = std::atan2(dir.x, dir.y) + (rng.random() < 0.5 ? 0.0 : kPi);
                placed = true;
            } else if (a.place == "air") {
                a.at = {middle.x + (rng.random() - 0.5) * 0.8, middle.y + (rng.random() - 0.5) * 0.8};
                a.height = std::min(ceiling - 0.4, 1.5 + 0.5 * rng.random());
                a.yaw = rng.random() * 2 * kPi;
                placed = true;
            } else {
                const double inset = a.place == "trail" ? 0.04 : 0.12;
                a.at = {onWall.x + normal.x * inset, onWall.y + normal.y * inset};
                if (!clear(a.at) && a.place != "trail") continue;
                a.yaw = std::atan2(dir.x, dir.y) + (rng.random() < 0.5 ? 0.0 : kPi);
                if (a.place == "trail") {
                    a.count = s.groupMin + int(rng.randrange(uint32_t(s.groupMax - s.groupMin + 1)));
                    a.length = std::min(length * 0.6, 1.2 + 1.3 * rng.random());
                }
                placed = true;
            }
        }
        if (placed) out.push_back(std::move(a));
    }
    return out;
}

}  // namespace r1
