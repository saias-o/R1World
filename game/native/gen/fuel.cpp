#include "fuel.hpp"
#include "clip.hpp"
#include "streets.hpp"
#include "appearance.hpp"
#include "palette.hpp"
#include "predict.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <map>

namespace r1 {
namespace {
// Canopy clearance and fascia depth of a European forecourt: 4.7 m under the
// fascia lets a lorry through, and the fascia is about 0.9 m deep.
constexpr double kClearance = 4.7, kFascia = 0.9;
// A lane each side of a 1.2 m island: rows 8 m apart, islands 9 m apart.
constexpr double kRowPitch = 8.0, kIslandPitch = 9.0, kIslandLength = 4.6, kIslandWidth = 1.2;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
// Metres east and north of `o`, for the few tens of metres a station spans.
P2 metres(P2 o, P2 p) {
    return {wrap(p.x - o.x) * kMetresPerDegree * std::cos(radians(o.y)), (p.y - o.y) * 111132.0};
}
P2 degrees(P2 o, P2 m) {
    return {o.x + m.x / (kMetresPerDegree * std::cos(radians(o.y))), o.y + m.y / 111132.0};
}
Ring metricRing(P2 o, const std::vector<P2>& points) {
    Ring r;
    for (size_t i = 0; i + 1 < points.size(); ++i) r.push_back(metres(o, points[i]));
    return r;
}
bool drivable(const Tags& t) {
    static const char* skip[] = {"footway", "path", "cycleway", "steps", "pedestrian", "track", "construction",
                                 "proposed", "bridleway", "corridor", "platform"};
    const auto h = tagOr(t, "highway");
    if (h.empty()) return false;
    for (const char* s : skip) if (h == s) return false;
    return true;
}
// A box whose `along` side follows `dir` (unit, engine x/z).
void block(Mesh& m, P2 c, double y0, double y1, double along, double across, P2 dir) {
    m.addBox({c.x, (y0 + y1) / 2, c.y}, {along, y1 - y0, across}, -std::atan2(dir.y, dir.x));
}
Material paint(const char* name, std::array<double, 3> c, double rough) {
    Material m; m.name = name; m.color = {c[0], c[1], c[2], 1.0}; m.roughness = rough; m.doubleSided = true;
    return m;
}
// Letters in `colour`: light on the livery fascia, livery on the totem's panel.
// `bottom` and `height` bound the whole glyph: the sign font's glyph is a
// textured cell from 0.36 of a capital below the baseline to 1.36 above it,
// so a capital is 1/1.72 of the band it must stay inside.
void letter(CanopyBook& book, const std::string& text, P2 at, double bottom, P2 normal, double width, double height,
            const std::array<double, 3>& colour) {
    if (text.empty() || width < 0.8) return;
    const double cap = height / 1.72;
    book.lettering.push_back({{"text", text}, {"at", {at.x, bottom + 0.36 * cap, at.y}}, {"normal", {normal.x, normal.y}},
                              {"width", width}, {"height", cap}, {"colour", {colour[0], colour[1], colour[2]}}});
}
}  // namespace

bool fuelStation(const Tags& t) { return tagOr(t, "amenity") == "fuel"; }

namespace {
// A station mapped on its canopy: the one raised a level, or the one that is
// no shop. A station building with a convenience store is its kiosk.
bool stationCanopy(const Tags& t) {
    if (!has(t, "building") || !fuelStation(t)) return false;
    if (tagOr(t, "building") == "roof") return true;
    const auto shop = tagOr(t, "shop");
    const auto layer = taggedLength(tagOr(t, "layer"));
    return (layer && *layer >= 1) || shop.empty() || shop == "gas";
}
// What may be a station's canopy. The French cadastre marks every light
// construction wall=no (sheds, lean-tos): one is a canopy only when a
// station stands in or right beside it.
bool canopyCandidate(const Tags& t) {
    return has(t, "building") && (tagOr(t, "building") == "roof" || tagOr(t, "wall") == "no" || stationCanopy(t));
}
}  // namespace

bool openRoof(const Tags& t) {
    return has(t, "building") && (tagOr(t, "building") == "roof" || has(t, "r1:fuel") || stationCanopy(t));
}

std::string fuelBrand(const Tags& t) { return tagOr(t, "brand", tagOr(t, "name", tagOr(t, "operator"))); }

std::string fuelTitle(const std::string& country) {
    // The word a forecourt shows, in the country's main language. The sign
    // font spells Latin capitals and falls back to the plain letter for an
    // accent it lacks (as town signs do); another script is not spelt, and
    // the fascia then reads the brand.
    static const std::map<std::string, std::string> words = [] {
        std::map<std::string, std::string> m;
        auto put = [&](std::initializer_list<const char*> codes, const char* word) { for (auto c : codes) m[c] = word; };
        put({"FR", "BE", "LU", "MC", "SN", "CI", "ML", "BF", "NE", "TG", "BJ", "GN", "CM", "GA", "CG", "CD", "MG",
             "HT", "PF", "NC", "RE", "GP", "MQ", "GF", "YT", "PM"}, "STATION-SERVICE");
        put({"DE", "AT", "CH", "LI"}, "TANKSTELLE");
        put({"NL", "SR", "DK"}, "TANKSTATION");
        put({"ES", "MX", "GT", "HN", "SV", "NI", "CR", "PA", "CU", "DO", "PR", "VE", "CO", "EC", "PE", "BO", "PY",
             "UY", "AR", "CL", "GQ"}, "ESTACIÓN DE SERVICIO");
        put({"IT", "SM", "VA"}, "STAZIONE DI SERVIZIO");
        put({"PT", "AO", "MZ", "CV", "GW", "ST", "TL"}, "POSTO DE COMBUSTÍVEL");
        put({"BR"}, "POSTO DE GASOLINA");
        put({"GB", "IE", "AU", "NZ", "ZA", "NG", "GH", "KE", "UG", "TZ", "ZM", "ZW", "BW", "NA", "MT", "CY", "SG",
             "MY", "IN", "PK", "LK", "JM", "TT"}, "PETROL STATION");
        put({"US", "CA", "PH", "LR", "BS", "BZ"}, "GAS STATION");
        put({"NO"}, "BENSINSTASJON");
        put({"SE"}, "BENSINSTATION");
        put({"FI"}, "HUOLTOASEMA");
        put({"IS"}, "BENSÍNSTÖÐ");
        put({"EE"}, "TANKLA");
        put({"LV"}, "DEGVIELAS UZPILDES STACIJA");
        put({"LT"}, "DEGALINĖ");
        put({"PL"}, "STACJA PALIW");
        put({"CZ"}, "ČERPACÍ STANICE");
        put({"SK"}, "ČERPACIA STANICA");
        put({"HU"}, "BENZINKÚT");
        put({"RO", "MD"}, "BENZINĂRIE");
        put({"SI"}, "BENCINSKI SERVIS");
        put({"HR", "BA", "ME"}, "BENZINSKA POSTAJA");
        put({"AL", "XK"}, "KARBURANT");
        put({"TR"}, "AKARYAKIT İSTASYONU");
        put({"ID"}, "SPBU");
        put({"VN"}, "CÂY XĂNG");
        return m;
    }();
    const auto it = words.find(country);
    return it == words.end() ? std::string() : it->second;
}

std::string countryAt(double lon, double lat) {
    // Loaded once, by whichever thread cooks first (a local static's
    // initialisation is thread-safe).
    static const CountryCrowd borders = [] {
        CountryCrowd c;
        std::ifstream in(palette().gameRoot + "/assets/world/countries.geojson");
        if (in) c.load(nlohmann::json::parse(in));
        return c;
    }();
    return borders.at(lon, lat).code;
}

std::array<double, 3> fuelLivery(const std::string& brand) {
    // Liveries as albedos, darkened to the range paint measures (CLAUDE.md §2).
    // The colour of a brand is what the fascia shows from the road; no logo.
    static const std::vector<std::pair<const char*, std::array<double, 3>>> liveries = {
        {"total", {0.30, 0.03, 0.03}}, {"intermarch", {0.28, 0.03, 0.03}}, {"avia", {0.30, 0.02, 0.02}},
        {"esso", {0.30, 0.02, 0.03}},  {"auchan", {0.28, 0.03, 0.03}},     {"carrefour", {0.02, 0.06, 0.20}},
        {"leclerc", {0.02, 0.10, 0.25}}, {"super u", {0.01, 0.08, 0.24}},  {"station u", {0.01, 0.08, 0.24}},
        {"système u", {0.01, 0.08, 0.24}}, {"bp", {0.02, 0.14, 0.04}},     {"casino", {0.03, 0.12, 0.04}},
        {"shell", {0.35, 0.26, 0.02}}, {"agip", {0.33, 0.25, 0.02}},       {"eni", {0.33, 0.25, 0.02}},
        {"dyneff", {0.02, 0.08, 0.18}}, {"elan", {0.02, 0.12, 0.05}},      {"netto", {0.33, 0.24, 0.02}}};
    const auto b = " " + lower(brand) + " ";
    if (b == " u ") return {0.01, 0.08, 0.24};
    // Whole words only ("eni" is no part of "Genin"); "intermarch" and
    // "total" also open longer words (Intermarché, TotalEnergies).
    auto word = [&](const std::string& key) {
        for (size_t at = b.find(key); at != std::string::npos; at = b.find(key, at + 1))
            if (!std::isalnum((unsigned char)b[at - 1]) &&
                (key == "intermarch" || key == "total" || !std::isalnum((unsigned char)b[at + key.size()])))
                return true;
        return false;
    };
    for (const auto& [key, colour] : liveries)
        if (word(key)) return colour;
    return {0.05, 0.10, 0.08};
}

std::vector<OsmWay> fuelCanopies(const std::vector<const OsmWay*>& ways, const OsmData& osm, const Tile& tile,
                                 const std::string& country, nlohmann::json& manifest) {
    // The fascia's title, in the country's language; the brand when the
    // country is not listed or the font has no letters for its script.
    const std::string title = fuelTitle(country);
    const bool spelt = !title.empty() && facadeLettering(title, 14.0, 0.4).has_value();
    struct Station { int64_t id; bool node; P2 at; const Tags* tags; const OsmWay* area; };
    std::vector<Station> stations;
    for (const auto& n : osm.features) if (fuelStation(n.tags)) stations.push_back({n.id, true, {n.lon, n.lat}, &n.tags, nullptr});
    for (const auto& w : osm.landcover)
        if (fuelStation(w.tags)) stations.push_back({w.id, false, centroid(w.points), &w.tags, &w});
    // Every open roof in the neighbourhood, so a station finds its canopy
    // wherever the tiles cut them.
    std::vector<const OsmWay*> roofs;
    for (const auto& w : osm.buildings) if (canopyCandidate(w.tags)) roofs.push_back(&w);
    // A canopy belongs to the station it is tagged as, the one inside it,
    // the area it stands in, else the nearest station within 30 m.
    std::map<int64_t, const Station*> owner;
    for (const auto& s : stations) {
        std::vector<const OsmWay*> mine;
        for (const auto* r : roofs) {
            if (stationCanopy(r->tags)) continue;
            if (pointInPolygon(s.at, r->points) || (s.area && pointInPolygon(centroid(r->points), s.area->points)))
                mine.push_back(r);
        }
        if (mine.empty() && s.node) {
            const OsmWay* best = nullptr; double nearest = 30.0;
            for (const auto* r : roofs) {
                if (stationCanopy(r->tags)) continue;
                const double d = std::hypot(metres(s.at, centroid(r->points)).x, metres(s.at, centroid(r->points)).y);
                if (d < nearest) { nearest = d; best = r; }
            }
            if (best) mine.push_back(best);
        }
        for (const auto* r : mine) if (!owner.count(r->id)) owner[r->id] = &s;
    }
    std::vector<OsmWay> out;
    out.reserve(ways.size() + 4);
    for (const auto* w : ways) {
        out.push_back(*w);
        auto& b = out.back();
        const Tags* station = stationCanopy(b.tags) ? &w->tags : nullptr;
        if (auto it = owner.find(b.id); it != owner.end() && !station) station = it->second->tags;
        if (!station) continue;
        b.tags["r1:fuel"] = "1";
        b.tags["r1:fuelBrand"] = fuelBrand(*station);
        b.tags["r1:fuelSource"] = "measured";
        b.tags["r1:fuelTitle"] = spelt ? title : fuelBrand(*station);
    }
    for (const auto* w : ways)
        if (stationCanopy(w->tags))
            manifest.push_back({{"osmId", w->id}, {"osmType", "way"}, {"brand", fuelBrand(w->tags)},
                                {"canopySource", "measured"}});
    // A station of this tile with no canopy: one is inferred, 18 by 9 m on a
    // node, or inside its area, along the nearest road and clear of buildings
    // and roads. It is said to be inferred, and a refusal says why.
    for (const auto& s : stations) {
        const P2 first = s.area ? s.area->points.front() : s.at;
        if (tileAt(first.x, first.y) != tile) continue;
        nlohmann::json entry = {{"osmId", s.id}, {"osmType", s.node ? "node" : "way"}, {"brand", fuelBrand(*s.tags)}};
        bool covered = false;
        for (const auto& [id, st] : owner) covered |= st == &s;
        for (const auto* r : roofs) covered |= stationCanopy(r->tags) && pointInPolygon(s.at, r->points);
        if (covered) { entry["canopySource"] = "measured"; manifest.push_back(entry); continue; }
        double length = 18, width = 9;
        P2 dir{1, 0};
        if (s.area) {
            const auto box = orientedBox(metricRing(s.at, s.area->points));
            length = std::min(2 * box.halfU - 4, 30.0); width = std::min(2 * box.halfV - 4, 12.0);
            dir = {box.ux, box.uz};
        } else {
            double nearest = 80;
            for (const auto& r : osm.roads) {
                if (!drivable(r.tags)) continue;
                for (size_t i = 1; i < r.points.size(); ++i) {
                    const P2 a = metres(s.at, r.points[i - 1]), c = metres(s.at, r.points[i]);
                    const double d = clip::distance({a, c}, {0, 0}), l = dist(a, c);
                    if (d < nearest && l > 1) { nearest = d; dir = {(c.x - a.x) / l, (c.y - a.y) / l}; }
                }
            }
        }
        if (length < 8 || width < 6) {
            entry["canopySource"] = "none"; entry["reason"] = "station area too small for a canopy";
            manifest.push_back(entry); continue;
        }
        const P2 across{-dir.y, dir.x};
        auto rect = [&](P2 c) {
            return Ring{{c.x - dir.x * length / 2 - across.x * width / 2, c.y - dir.y * length / 2 - across.y * width / 2},
                        {c.x + dir.x * length / 2 - across.x * width / 2, c.y + dir.y * length / 2 - across.y * width / 2},
                        {c.x + dir.x * length / 2 + across.x * width / 2, c.y + dir.y * length / 2 + across.y * width / 2},
                        {c.x - dir.x * length / 2 + across.x * width / 2, c.y - dir.y * length / 2 + across.y * width / 2}};
        };
        auto clear = [&](const Ring& r) {
            const auto region = clip::Paths64{clip::kMetres.path(r)};
            for (const auto& b : osm.buildings) {
                const P2 c = metres(s.at, b.points.front());
                if (std::hypot(c.x, c.y) > 150) continue;
                if (clip::area(clip::intersect(region, clip::bufferRing(metricRing(s.at, b.points), 1.0))) > 0.01)
                    return false;
            }
            for (const auto& road : osm.roads) {
                if (!drivable(road.tags)) continue;
                std::vector<P2> line;
                for (const auto& p : road.points) line.push_back(metres(s.at, p));
                bool near = false;
                for (const auto& p : line) near |= std::hypot(p.x, p.y) < 150;
                if (!near) continue;
                if (clip::area(clip::intersect(region, clip::bufferLine(line, roadWidth(road.tags) / 2 + 0.5))) > 0.01)
                    return false;
            }
            return true;
        };
        std::optional<Ring> placed;
        for (double shift : {0.0, 3.0, -3.0, 6.0, -6.0, 9.0, -9.0})
            if (clear(rect({across.x * shift, across.y * shift}))) { placed = rect({across.x * shift, across.y * shift}); break; }
        if (!placed) {
            entry["canopySource"] = "none"; entry["reason"] = "no room clear of buildings and roads";
            manifest.push_back(entry); continue;
        }
        OsmWay canopy;
        canopy.id = -(s.id * 2 + (s.node ? 1 : 0));
        for (const auto& p : *placed) canopy.points.push_back(degrees(s.at, p));
        canopy.points.push_back(canopy.points.front());
        canopy.tags = {{"building", "roof"}, {"r1:fuel", "1"}, {"r1:fuelBrand", fuelBrand(*s.tags)},
                       {"r1:fuelSource", "inferred"}, {"r1:fuelTitle", spelt ? title : fuelBrand(*s.tags)}};
        out.push_back(std::move(canopy));
        entry["canopySource"] = "inferred";
        manifest.push_back(entry);
    }
    return out;
}

Mesh& CanopyBook::liveryMesh(const std::array<double, 3>& colour) {
    for (auto& [c, m] : livery) if (c == colour) return m;
    livery.push_back({colour, Mesh()});
    return livery.back().second;
}

std::vector<MeshPart> CanopyBook::parts() {
    std::vector<MeshPart> out;
    auto add = [&](const char* name, Mesh& m, Material material) {
        if (!m.empty()) out.push_back({name, std::move(m), std::move(material)});
    };
    add("Canopy roof", top, paint("Canopy membrane", {0.24, 0.24, 0.23}, 0.9));
    add("Canopy soffit", underside, paint("Canopy soffit", {0.36, 0.36, 0.35}, 0.7));
    add("Canopy fascia", fascia, paint("Canopy fascia", {0.45, 0.45, 0.44}, 0.5));
    add("Canopy columns and frames", steel, paint("Painted steel", {0.40, 0.40, 0.39}, 0.45));
    add("Pump islands", concrete, paint("Island concrete", {0.22, 0.22, 0.21}, 0.9));
    add("Fuel dispensers", body, paint("Dispenser enamel", {0.42, 0.42, 0.41}, 0.35));
    add("Dispenser screens", screens, paint("Dispenser glass", {0.03, 0.035, 0.04}, 0.1));
    add("Island bollards", bollards, paint("Safety yellow", {0.35, 0.27, 0.03}, 0.6));
    add("Canopy lights", lights, paint("Light diffuser", {0.70, 0.68, 0.60}, 0.9));
    for (auto& [colour, mesh] : livery) add("Station livery", mesh, paint("Station livery", colour, 0.5));
    return out;
}

void buildOpenRoof(const Ring& ring, const OrientedBox& box, double ground, double wallHeight, const Tags& tags,
                   int64_t id, CanopyBook& book) {
    const bool fuel = has(tags, "r1:fuel");
    const auto height = taggedLength(tagOr(tags, "height"));
    const auto under = taggedLength(tagOr(tags, "min_height"));
    const double deck = fuel ? kFascia : 0.25;
    double top = height ? ground + *height : fuel ? ground + kClearance + kFascia : ground + std::clamp(wallHeight, 2.6, 6.0);
    double low = under ? ground + *under : top - deck;
    low = std::max(low, ground + (fuel ? 3.8 : 2.2));
    top = std::max(top, low + 0.15);
    for (const auto& t : triangulate(ring)) {
        const P2 a = ring[size_t(t[0])], b = ring[size_t(t[1])], c = ring[size_t(t[2])];
        book.top.addUpTriangle({a.x, top, a.y}, {b.x, top, b.y}, {c.x, top, c.y});
        book.underside.addTriangle({c.x, low, c.y}, {b.x, low, b.y}, {a.x, low, a.y});
    }
    const auto livery = fuelLivery(tagOr(tags, "r1:fuelBrand"));
    for (size_t e = 0; e < ring.size(); ++e) {
        const P2 a = ring[e], b = ring[(e + 1) % ring.size()];
        const double len = dist(a, b);
        if (len < 1e-3) continue;
        if (fuel) {
            // The fascia in the brand's livery between two white trims: a
            // white fascia reads as the overcast sky behind it.
            const P2 n{(b.y - a.y) / len * 0.015, -(b.x - a.x) / len * 0.015};
            book.liveryMesh(livery).addQuad({a.x, low, a.y}, {b.x, low, b.y}, {b.x, top, b.y}, {a.x, top, a.y});
            for (auto [y0, y1] : {std::pair<double, double>{low, low + 0.09}, {top - 0.09, top}})
                book.fascia.addQuad({a.x + n.x, y0, a.y + n.y}, {b.x + n.x, y0, b.y + n.y},
                                    {b.x + n.x, y1, b.y + n.y}, {a.x + n.x, y1, a.y + n.y});
        } else {
            book.fascia.addQuad({a.x, low, a.y}, {b.x, low, b.y}, {b.x, top, b.y}, {a.x, top, a.y});
            // A plain roof stands on posts at its corners and every 6 m.
            const P2 along{(b.x - a.x) / len, (b.y - a.y) / len}, in{-along.y, along.x};
            const int n = std::max(1, int(std::ceil(len / 6.0)));
            for (int i = 0; i < n; ++i) {
                const double s = len * i / n;
                const P2 p{a.x + along.x * s + in.x * 0.25, a.y + along.y * s + in.y * 0.25};
                if (pointInPolygon(p, ring)) block(book.steel, p, ground, low, 0.14, 0.14, along);
            }
        }
    }
    if (!fuel) return;

    // Pump islands along the canopy's long axis, one row per 8 m of depth.
    const P2 u{box.ux, box.uz}, v{box.vx(), box.vz()};
    const int rows = std::max(1, int(2 * box.halfV / kRowPitch));
    const int columns = std::max(1, int((2 * box.halfU - 2.0) / kIslandPitch));
    const double deckTop = ground + 0.18;
    int islands = 0, pumps = 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const double a = (c - (columns - 1) / 2.0) * kIslandPitch, b = (r - (rows - 1) / 2.0) * kRowPitch;
            const P2 centre = box.point(a, b);
            auto at = [&](double du, double dv) { return P2{centre.x + u.x * du + v.x * dv, centre.y + u.y * du + v.y * dv}; };
            const double half = kIslandLength / 2 + 0.35;
            const Ring footprint{at(-half, -kIslandWidth / 2), at(half, -kIslandWidth / 2), at(half, kIslandWidth / 2),
                                 at(-half, kIslandWidth / 2)};
            bool inside = true;
            for (const auto& p : footprint) inside &= pointInPolygon(p, ring);
            if (!inside) continue;
            ++islands;
            block(book.concrete, centre, ground - 0.1, deckTop, kIslandLength, kIslandWidth, u);
            block(book.steel, centre, deckTop, low, 0.35, 0.35, u);
            for (double du : {-2.55, 2.55}) block(book.bollards, at(du, 0), ground, ground + 0.9, 0.18, 0.18, u);
            for (double du : {-1.35, 1.35}) {
                ++pumps;
                const P2 p = at(du, 0);
                block(book.body, p, deckTop, deckTop + 1.75, 0.85, 0.5, u);
                block(book.liveryMesh(livery), p, deckTop + 1.75, deckTop + 1.95, 0.85, 0.5, u);
                for (double side : {-1.0, 1.0}) {
                    block(book.screens, at(du, side * 0.26), deckTop + 1.2, deckTop + 1.6, 0.6, 0.02, u);
                    block(book.liveryMesh(livery), at(du, side * 0.28), deckTop + 0.8, deckTop + 1.05, 0.55, 0.06, u);
                }
            }
            book.obstacles.push_back(footprint);
            book.obstacleTops.push_back(deckTop + 1.95);
        }
    // Soffit lights on a 4.5 m grid.
    int lamps = 0;
    for (double a = -box.halfU + 2.25; a < box.halfU && lamps < 48; a += 4.5)
        for (double b = -box.halfV + 2.25; b < box.halfV && lamps < 48; b += 4.5) {
            const P2 p = box.point(a, b);
            if (!pointInPolygon(p, ring)) continue;
            block(book.lights, p, low - 0.04, low - 0.005, 0.9, 0.9, u);
            ++lamps;
        }
    // The title on both long faces of the fascia, when the canopy is
    // the rectangle its box says.
    const std::string brand = tagOr(tags, "r1:fuelBrand");
    if (std::abs(polygonArea(ring)) / std::max(1e-6, box.area()) > 0.8)
        for (double side : {-1.0, 1.0}) {
            const P2 face = box.point(0, side * box.halfV), normal{v.x * side, v.y * side};
            letter(book, tagOr(tags, "r1:fuelTitle", brand), {face.x + normal.x * 0.03, face.y + normal.y * 0.03}, low + 0.12, normal,
                   std::min(2 * box.halfU - 2.0, 14.0), kFascia - 0.24, {0.65, 0.65, 0.61});
        }
    nlohmann::json outline = nlohmann::json::array();
    for (const auto& p : ring) outline.push_back({p.x, p.y});
    book.stations.push_back({{"id", id}, {"brand", brand}, {"canopySource", tagOr(tags, "r1:fuelSource", "measured")},
                             {"layoutSource", "synthesized"}, {"islands", islands}, {"pumps", pumps},
                             {"centre", {box.cx, box.cz}}, {"ground", ground}, {"ring", outline}});
}

void placeFuelTotems(const std::vector<OsmWay>& roads, const Anchor& anchor,
                     const std::function<P3(double, double)>& ground, const std::vector<Ring>& footprints,
                     CanopyBook& book) {
    struct Line { std::vector<P2> points; double half; };
    std::vector<Line> lines;
    for (const auto& r : roads) {
        if (!drivable(r.tags) || tagOr(r.tags, "highway") == "service") continue;
        Line l{{}, roadWidth(r.tags) / 2};
        for (const auto& p : r.points) { const P3 e = anchor.toEngine(p.x, p.y, 0); l.points.push_back({e.x, e.z}); }
        lines.push_back(std::move(l));
    }
    for (auto& station : book.stations) {
        const P2 c{station["centre"][0], station["centre"][1]};
        Ring canopy;
        for (const auto& p : station["ring"]) canopy.push_back({p[0], p[1]});
        // The nearest public road within 45 m of the canopy's middle.
        const Line* road = nullptr; P2 q{}, dir{};
        double nearest = 45;
        for (const auto& l : lines)
            for (size_t i = 1; i < l.points.size(); ++i) {
                const P2 a = l.points[i - 1], b = l.points[i];
                const double len = dist(a, b);
                if (len < 0.5) continue;
                const double t = std::clamp(((c.x - a.x) * (b.x - a.x) + (c.y - a.y) * (b.y - a.y)) / (len * len), 0.0, 1.0);
                const P2 p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (dist(p, c) < nearest) { nearest = dist(p, c); road = &l; q = p; dir = {(b.x - a.x) / len, (b.y - a.y) / len}; }
            }
        if (!road) { station["totem"] = "no public road within 45 m"; continue; }
        P2 n{c.x - q.x, c.y - q.y};
        const double nl = std::hypot(n.x, n.y);
        if (nl < 1e-6) { station["totem"] = "canopy stands on the road"; continue; }
        n = {n.x / nl, n.y / nl};
        // Beside the road on the station's side, clear of every footprint,
        // every canopy and every road: tried along the road, nearest first.
        std::optional<P2> spot;
        for (double slide : {0.0, 4.0, -4.0, 8.0, -8.0, 12.0, -12.0}) {
            const P2 p{q.x + n.x * (road->half + 2.0) + dir.x * slide, q.y + n.y * (road->half + 2.0) + dir.y * slide};
            bool free = !pointInPolygon(p, canopy);
            for (const auto& f : footprints) free &= !pointInPolygon(p, f);
            for (const auto& l : lines)
                for (size_t i = 1; i < l.points.size() && free; ++i)
                    free &= clip::distance({l.points[i - 1], l.points[i]}, p) > l.half + 1.2;
            if (free) { spot = p; break; }
        }
        if (!spot) { station["totem"] = "no clear spot beside the road"; continue; }
        const P3 geo = anchor.toGeodetic(spot->x, 0, spot->y);
        const double y = ground(geo.x, geo.y).y;
        const auto livery = fuelLivery(station["brand"].get<std::string>());
        // The panel faces the traffic: its width runs across the road.
        block(book.concrete, *spot, y - 0.2, y + 0.3, 0.6, 1.8, dir);
        block(book.liveryMesh(livery), *spot, y + 0.3, y + 5.4, 0.32, 1.3, dir);
        block(book.fascia, *spot, y + 4.2, y + 5.2, 0.36, 1.2, dir);
        const std::string brand = station["brand"];
        for (double side : {-1.0, 1.0})
            letter(book, brand, {spot->x + dir.x * side * 0.19, spot->y + dir.y * side * 0.19}, y + 4.3,
                   {dir.x * side, dir.y * side}, 1.05, 0.8, livery);
        const P2 across{-dir.y, dir.x};
        book.obstacles.push_back({{spot->x - dir.x * 0.3 - across.x * 0.9, spot->y - dir.y * 0.3 - across.y * 0.9},
                                  {spot->x + dir.x * 0.3 - across.x * 0.9, spot->y + dir.y * 0.3 - across.y * 0.9},
                                  {spot->x + dir.x * 0.3 + across.x * 0.9, spot->y + dir.y * 0.3 + across.y * 0.9},
                                  {spot->x - dir.x * 0.3 + across.x * 0.9, spot->y - dir.y * 0.3 + across.y * 0.9}});
        book.obstacleTops.push_back(y + 5.4);
        station["totem"] = "placed";
    }
}
}  // namespace r1
