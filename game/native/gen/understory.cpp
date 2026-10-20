#include "understory.hpp"
#include "spatial.hpp"
#include "streets.hpp"
#include "waterways.hpp"
#include "gardens.hpp"
#include "grass.hpp"

#include <set>
#include <cstdlib>

namespace r1 {
namespace {
constexpr const char* kDirectory = "assets/models/external/understory/";
struct Obstacle { std::vector<P2> points; double margin; bool area, settlement; };
struct Planting {
    P2 position;
    int64_t source;
    uint64_t order;
    double height, width, yaw;
    int species, rank;
    const char* reason;
};
uint64_t hash(uint64_t h) {
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    return h ^ (h >> 31);
}
double random(uint64_t h, uint64_t salt) { return double(hash(h ^ salt) >> 11) * (1.0 / 9007199254740992.0); }
uint64_t identity(int64_t id, size_t segment, int step, int side, int row) {
    return hash(uint64_t(id)) ^ hash(segment + 1237) ^ hash(uint64_t(step) * 17 + side + 2 + row * 199);
}
}

Scatter planUnderstory(const OsmData& osm, const Tile& tile, const Anchor& anchor,
                      const GroundAt& ground, const std::vector<OsmWay>& roadside,
                      const clip::Paths64& driveways, int budget,
                      const std::function<bool(P2)>& permitted) {
    const std::string climate = climateAt(profileFor(tile.center().x, tile.center().y).climate, tile.center().y);
    Scatter out;
    out.stats = {{"revision", 1}, {"budget", std::max(0, budget)}, {"placed", 0},
                 {"nearTriangles", 176}, {"farTriangles", 44}, {"bySource", nlohmann::json::object()},
                 {"byModel", nlohmann::json::object()}, {"climate", climate}};
    // Automated A/B captures use the same binary, observations and renderer.
    // This opt-out is deliberately absent from saved options and the game UI.
    if (std::getenv("R1WORLD_SMOKE_NO_UNDERSTORY")) {
        out.stats["benchmarkOmitted"] = true;
        return out;
    }
    // These scans represent temperate broadleaved undergrowth. Do not plant
    // European hedges in deserts, permanent ice or above the alpine treeline.
    if (budget <= 0) return out;
    const bool infer = climate != "polar" && climate != "arid" && climate != "desert" && std::abs(tile.center().y) <= 68;
    auto xy = [&](P2 p) { const auto e = anchor.toEngine(p.x, p.y, 0); return P2{e.x, e.z}; };
    auto line = [&](const std::vector<P2>& points) {
        std::vector<P2> result;
        for (auto p : points) result.push_back(xy(p));
        return result;
    };
    std::vector<Obstacle> obstacles;
    BoxIndex index(32);
    auto obstacle = [&](std::vector<P2> points, double margin, bool area, bool settlement = false) {
        if (points.size() < 2) return;
        index.add(obstacles.size(), boxOf(points).grown(margin));
        obstacles.push_back({std::move(points), margin, area, settlement});
    };
    for (const auto& b : osm.buildings) obstacle(line(b.points), 1.0, true, true);
    for (const auto& r : roadside) {
        if (taggedYes(r.tags, "tunnel")) continue;
        obstacle(line(r.points), roadWidth(r.tags) * .5 + .45, false);
    }
    for (const auto& p : driveways) obstacle(clip::kMetres.ring(p), .45, true);
    for (const auto& a : osm.aeroways) {
        const auto kind = tagOr(a.tags,"aeroway");
        const double width = lengthTag(tag(a.tags,"width"),kind=="runway"?45:kind=="taxiway"?18:12);
        obstacle(line(a.points), a.closed()?2:width*.5+2, a.closed());
    }
    for (const auto& a : osm.waterways) obstacle(line(a.points), lengthTag(tag(a.tags,"width"),tagOr(a.tags,"waterway")=="river"?10:1.5)*.5+1, a.closed());
    for (const auto& a : osm.landcover) {
        const auto natural = tagOr(a.tags, "natural"), leisure = tagOr(a.tags, "leisure"), use = tagOr(a.tags, "landuse");
        if (natural == "water" || natural == "beach" || natural == "bare_rock" || natural == "sand" ||
            has(a.tags, "water") || leisure == "pitch" || leisure == "playground" ||
            use == "cemetery" || use == "construction" || tagOr(a.tags, "amenity") == "parking" || has(a.tags, "parking"))
            obstacle(line(a.points), .6, true);
    }
    const auto relationWater = inlandWaterRegion({}, anchor, false, osm.waterAreas);
    auto clear = [&](P2 p, double radius) {
        if (permitted && !permitted(p)) return false;
        if (clip::contains(relationWater, p)) return false;
        const Box query{p.x - radius, p.y - radius, p.x + radius, p.y + radius};
        for (size_t i : index.near(query)) {
            const auto& o = obstacles[i];
            if ((o.area && pointInPolygon(p, o.points)) || clip::distance(o.points, p) < o.margin + radius) return false;
        }
        return true;
    };
    std::vector<Planting> candidates;
    auto widthAt = [&](uint64_t seed, double height, double hedgeWidth = 0.) {
        return hedgeWidth > 0 ? hedgeWidth / height : .85 + random(seed,193)*.4;
    };
    auto radiusAt = [&](uint64_t seed, double height, int species) {
        return height * widthAt(seed,height) * (species == 5 ? 1.2 : .72);
    };
    auto add = [&](P2 p, uint64_t seed, int64_t source, double height, int species, int rank, const char* reason, double yaw, double hedgeWidth = 0) {
        const auto geo = anchor.toGeodetic(p.x, 0, p.y);
        if (tileAt(geo.x, geo.y) != tile) return;
        const double width = widthAt(seed,height,hedgeWidth);
        // Half of the largest clump's footprint, plus room for its sway.
        const double radius = height * width * (species == 5 ? 1.2 : .72);
        if (!clear(p, radius)) return;
        const auto at = ground(geo.x, geo.y);
        if (anchor.toGeodetic(at.x, at.y, at.z).z > 2500) return;
        candidates.push_back({p, source, hash(seed), height, width, yaw, species, rank, reason});
        auto& sourceCounts=out.stats["candidateSources"];
        if(sourceCounts.is_null())sourceCounts=nlohmann::json::object();
        sourceCounts[reason]=sourceCounts.value(reason,0)+1;
    };
    // Mapped hedge lines have priority over every inferred verge. Explicit
    // height is retained, including unusually tall surveyed hedges.
    for (const auto& hedge : osm.barriers) {
        if (tagOr(hedge.tags, "barrier") != "hedge") continue;
        const auto points = line(hedge.points);
        double travelled = 0;
        for (size_t s = 0; s + 1 < points.size(); ++s) {
            const auto a = points[s], b = points[s + 1];
            const double length = dist(a, b);
            if (length < .01) continue;
            const double angle = std::atan2(-(b.y - a.y), b.x - a.x);
            constexpr double spacing = 1.05;
            for (int step = int(std::ceil(travelled / spacing)); step * spacing < travelled + length; ++step) {
                const double t = (step * spacing - travelled) / length;
                const auto seed = identity(hedge.id, 0, step, 0, 0);
                const double height = lengthTag(tag(hedge.tags, "height"), 1.0 + .4 * random(seed, 17));
                add({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, seed, hedge.id, height,
                    int(random(seed, 33) * 4), 0, "osm-hedge", angle + random(seed, 65), lengthTag(tag(hedge.tags,"width"), 1.1));
            }
            travelled += length;
        }
    }
    // Keep the short maintained shoulder, then herbs, then an irregular woody
    // fringe. The coherent along-road envelope makes patches rather than rows
    // of uniformly spaced identical spheres. Junctions/accesses use all roads.
    for (const auto& road : roadside) {
        if (!infer) break;
        const auto highway = tagOr(road.tags, "highway");
        const bool motorway = highway == "motorway" || highway == "motorway_link" || highway == "trunk" || highway == "trunk_link";
        const bool rural = highway == "secondary" || highway == "tertiary" || highway == "unclassified" ||
                           highway == "residential" || highway == "service" || highway == "track";
        if ((!motorway && !rural) || taggedYes(road.tags, "bridge") || taggedYes(road.tags, "tunnel") ||
            has(road.tags, "r1:raised")) continue;
        if (!motorway && (tagOr(road.tags, "sidewalk") == "both" || tagOr(road.tags, "sidewalk") == "yes")) continue;
        const auto points = line(road.points);
        double travelled = 0;
        for (size_t s = 0; s + 1 < points.size(); ++s) {
            const auto a = points[s], b = points[s + 1];
            const double length = dist(a, b);
            if (length < .01) continue;
            const P2 normal{-(b.y - a.y) / length, (b.x - a.x) / length};
            constexpr double spacing = 2.15;
            for (int step = int(std::ceil(travelled / spacing)); step * spacing < travelled + length; ++step) {
                const double distance = step * spacing;
                const double patch = .5 + .5 * std::sin(distance * .047 + random(uint64_t(road.id), 901) * kTau);
                for (int side : {-1, 1}) for (int row = 0; row < 2; ++row) {
                    const uint64_t segmentKey=hash(uint64_t(int64_t(std::llround(road.points[s].x*1e7)))) ^
                        hash(uint64_t(int64_t(std::llround(road.points[s].y*1e7))));
                    const auto seed = identity(road.id, segmentKey, step, side, row);
                    const double density = climate == "mediterranean" ? .64 : .9;
                    if (random(seed, 17) > density * (.55 + patch * .45)) continue;
                    const double along = std::clamp(distance + (random(seed, 41) - .5) * 1.6 - travelled, 0.0, length);
                    const double t = along / length;
                    // A nearby house without a mapped sidewalk still vetoes
                    // guessed wild scrub. Explicit hedges remain eligible.
                    P2 axis{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                    bool settled = false;
                    if (!motorway) for (size_t i : index.near(Box{axis.x, axis.y, axis.x, axis.y}.grown(12))) {
                        const auto& o = obstacles[i];
                        if (o.settlement && clip::distance(o.points, axis) < 12) { settled = true; break; }
                    }
                    if (settled) continue;
                    const bool herbs = row == 0;
                    const double height = herbs ? .30 + random(seed, 73) * (motorway ? .65 : .40)
                        : (motorway ? 1.25 : .65) + random(seed, 73) * (motorway ? 1.0 : .70);
                    const double offset = roadWidth(road.tags) * .5 + (motorway ? 2.3 : .65) +
                                          (herbs ? (motorway ? .8 : .5) : motorway ? 2.0 : 1.2) + random(seed, 57) * (herbs ? .5 : 1.3);
                    const double herb = random(seed, 99);
                    int species = herbs ? (herb < .32 ? 7 : herb < .72 ? 6 : 4) : int(herb * 4);
                    if (!herbs && climate == "temperate" && random(seed, 109) < .12) species = 5;
                    P2 p{axis.x + normal.x * side * offset, axis.y + normal.y * side * offset};
                    // A separately mapped path can occupy the first strip.
                    // Find the field side of it instead of deleting the verge.
                    for(int retry=0;retry<3 && !clear(p,radiusAt(seed,height,species));++retry) {
                        p.x+=normal.x*side*1.25;p.y+=normal.y*side*1.25;
                    }
                    add(p, seed, road.id, height, species, motorway ? 2 : 3,
                        motorway ? "inferred-motorway-verge" : "inferred-rural-verge", random(seed, 83) * kTau);
                }
            }
            travelled += length;
        }
    }
    if (infer) {
        const auto homes=planResidential(osm,anchor);
        std::vector<std::vector<P2>> boundaries;
        BoxIndex boundaryIndex(64);
        for(const auto& boundary : osm.barriers) {
            auto points=line(boundary.points);
            boundaryIndex.add(boundaries.size(),boxOf(points));
            boundaries.push_back(std::move(points));
        }
        for (const auto& home : homes.homes) {
            if (!home.gardenEligible || !home.style || home.style->plotGround=="mineral") continue;
            bool surveyed=false;
            for(size_t i : boundaryIndex.near(Box{home.centre.x,home.centre.y,home.centre.x,home.centre.y}.grown(home.halfWidth+12)))
                if(clip::distance(boundaries[i],home.centre)<home.halfWidth+12) {surveyed=true;break;}
            if(surveyed)continue;
            for(int side : {-1,1})for(int patch=0;patch<3;++patch) {
                const auto seed=identity(home.id,0,patch,side,7);
                const double along=side*(home.halfWidth-1.1-patch*.35);
                if(std::abs(along)<home.style->accessWidth*.5+1.0)continue;
                const double inward=home.front+1.1+patch*.8;
                const P2 p{home.frontage.x+home.along.x*along+home.inward.x*inward,
                           home.frontage.y+home.along.y*along+home.inward.y*inward};
                add(p,seed,home.id,.35+random(seed,333)*.55,int(random(seed,335)*4),3,
                    "inferred-front-garden",random(seed,337)*kTau);
            }
        }
    }
    // OSM scrub and woodland margins add low cover beyond roads. Do not turn
    // a mown park, a sports field or a crop field into a guessed jungle.
    std::set<int64_t> seen;
    for (const auto* ways : {&osm.vegetation, &osm.landcover}) for (const auto& area : *ways) {
        if (!infer) continue;
        const auto natural = tagOr(area.tags, "natural"), use = tagOr(area.tags, "landuse");
        const bool scrub = natural == "scrub" || natural == "heath";
        const bool wood = natural == "wood" || use == "forest";
        if ((!scrub && !wood) || area.points.size() < 4 || !seen.insert(area.id).second) continue;
        const auto ring = line(area.points);
        const auto box = boxOf(ring);
        const auto bounds = tile.bounds();
        const auto sw = xy({bounds.west, bounds.south}), ne = xy({bounds.east, bounds.north});
        const double x0 = std::max(box.x0, std::min(sw.x, ne.x)), x1 = std::min(box.x1, std::max(sw.x, ne.x));
        const double z0 = std::max(box.y0, std::min(sw.y, ne.y)), z1 = std::min(box.y1, std::max(sw.y, ne.y));
        for (int x = int(std::floor(x0 / 4)); x * 4 < x1; ++x) for (int z = int(std::floor(z0 / 4)); z * 4 < z1; ++z) {
            const auto seed = hash(uint64_t(area.id)) ^ hash(uint64_t(int64_t(x))) ^ hash(uint64_t(int64_t(z)) + 739);
            if (random(seed, 121) > (scrub ? .7 : .20)) continue;
            P2 p{x * 4 + random(seed, 137) * 4, z * 4 + random(seed, 149) * 4};
            if (!pointInPolygon(p, ring) || (!scrub && clip::distance(ring, p) > 12)) continue;
            const double height = scrub ? .45 + random(seed, 167) * 1.1 : .35 + random(seed, 167) * .7;
            const int species = wood && random(seed, 183) < .45 ? 5 : int(random(seed, 183) * 4);
            add(p, seed, area.id, height, species, scrub ? 1 : 4, scrub ? "osm-scrub" : "osm-woodland-margin", random(seed, 197) * kTau);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::tie(a.rank, a.order) < std::tie(b.rank, b.order);
    });
    const char* names[] = {"shrub_01", "shrub_02", "shrub_03", "shrub_04", "nettle_plant", "fern_02", "grass_medium_02", "grass_dry"};
    BoxIndex occupied(4);
    std::vector<P2> accepted;
    size_t overlaps = 0;
    std::set<uint64_t> chosen;
    size_t roadLimit=size_t(budget);
    // Surveyed hedges first. Reserve two thirds of the remaining space for
    // verges/gardens; large scrub polygons cannot consume every nearby slot.
    for(int pass=0;pass<3;++pass) {
      if(pass==1)roadLimit=std::min(size_t(budget),out.nodes.size()+size_t(budget)*2/3);
      for (const auto& c : candidates) {
        if (out.nodes.size() >= size_t(budget) || (pass==1 && out.nodes.size()>=roadLimit)) break;
        if(chosen.count(c.order) || (pass==0 && c.rank!=0) ||
           (pass==1 && c.rank!=2 && c.rank!=3))continue;
        const double spacing = c.rank == 0 ? .75 : .9;
        bool overlap = false;
        for (size_t i : occupied.near(Box{c.position.x, c.position.y, c.position.x, c.position.y}.grown(spacing)))
            if (dist(accepted[i], c.position) < spacing) { overlap = true; break; }
        if (overlap) { ++overlaps; chosen.insert(c.order); continue; }
        occupied.add(accepted.size(), {c.position.x, c.position.y, c.position.x, c.position.y});
        accepted.push_back(c.position);
        chosen.insert(c.order);
        const auto geo = anchor.toGeodetic(c.position.x, 0, c.position.y);
        const auto at = ground(geo.x, geo.y);
        const std::string model = names[c.species];
        nlohmann::json children = nlohmann::json::array();
        for (const auto& level : {std::pair{"Near", "near"}, std::pair{"Far", "mid"}})
            children.push_back({{"type", "Node"}, {"name", level.first}, {"importedFrom", std::string(kDirectory) + model + "_" + level.second + ".glb"}});
        out.nodes.push_back({{"type", "Node"}, {"name", std::string("Understory ") + c.reason + " " + std::to_string(c.order)},
            {"groups", {"vegetation", "understory"}}, {"children", children},
            {"transform", {{"position", {at.x, at.y - .045, at.z}},
                {"scale", {c.height * c.width, c.height, c.height * c.width}},
                {"rotation", {0, std::sin(c.yaw / 2), 0, std::cos(c.yaw / 2)}}}}});
        auto& sources = out.stats["bySource"];
        sources[c.reason] = sources.value(c.reason, 0) + 1;
        auto& models = out.stats["byModel"];
        models[model] = models.value(model, 0) + 1;
      }
    }
    out.stats["placed"] = out.nodes.size();
    out.stats["candidates"] = candidates.size();
    out.stats["rejectedOverlap"] = overlaps;
    out.stats["droppedForBudget"] = candidates.size() > out.nodes.size() + overlaps ? candidates.size() - out.nodes.size() - overlaps : 0;
    out.stats["inferencePolicy"] = "mapped hedges first; reserved verges/gardens; mapped scrub; woodland margins; measured height/width and ground/access vetoes";
    return out;
}

void maskUnderstoryGrass(GrassCover& cover, const nlohmann::json& plants) {
    if(cover.empty() || cover.coverSize<=0)return;
    const int size=cover.coverSize;
    const auto& m=cover.uvFromEngine;
    const double cell=1.0/(size*std::max(1e-9,std::hypot(m[0],m[1])));
    for(const auto& n : plants) {
        if(n.value("name",std::string()).rfind("Understory ",0)!=0)continue;
        const auto& t=n["transform"];const auto& p=t["position"];const auto& scale=t["scale"];
        const double x=p[0],z=p[2];
        const double cx=(m[0]*x+m[1]*z+m[2])*size-.5,cy=(m[3]*x+m[4]*z+m[5])*size-.5;
        const double radius=std::max(.8,scale[0].get<double>()*.7);
        const double pixels=radius/cell;
        for(int j=std::max(0,int(std::floor(cy-pixels)));j<=std::min(size-1,int(std::ceil(cy+pixels)));++j)
          for(int i=std::max(0,int(std::floor(cx-pixels)));i<=std::min(size-1,int(std::ceil(cx+pixels)));++i) {
            const double r=std::hypot(i-cx,j-cy)/pixels;
            if(r>=1)continue;
            const double blend=r*r*(3-2*r);
            auto& texel=cover.cover[size_t(j)*size+i];
            const auto density=uint32_t(std::lround((texel>>24)*(.08+.92*blend)));
            texel=(texel&0x00ffffffu)|(density<<24);
          }
    }
}
}
