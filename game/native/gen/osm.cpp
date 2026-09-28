#include "osm.hpp"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace r1 {

namespace {
const char* kLandcoverKeys[] = {"landuse", "natural", "leisure", "water", "waterway", "amenity", "aeroway"};

Tags readTags(const nlohmann::json& element) {
    Tags tags;
    auto it = element.find("tags");
    if (it == element.end() || !it->is_object()) return tags;
    for (auto t = it->begin(); t != it->end(); ++t)
        tags[t.key()] = t->is_string() ? t->get<std::string>() : t->dump();
    return tags;
}

bool in(const std::string* value, std::initializer_list<const char*> set) {
    if (!value) return false;
    for (const char* s : set) if (*value == s) return true;
    return false;
}
}  // namespace

OsmData normalizeOsm(const nlohmann::json& document, const nlohmann::json* layer) {
    OsmData out;
    out.queryVersion = document.value("r1QueryVersion", 1);
    std::unordered_map<int64_t, P2> nodes;
    std::vector<const nlohmann::json*> rawWays;
    std::set<P2> trees;
    std::unordered_set<int64_t> featureIds, wayIds;
    for (const nlohmann::json* doc : {&document, layer}) {
        if (!doc) continue;
        auto elements = doc->find("elements");
        if (elements == doc->end() || !elements->is_array()) continue;
        nodes.reserve(nodes.size() + elements->size());
        for (const auto& e : *elements) {
            const std::string type = e.value("type", std::string());
            if (type == "node") {
                P2 p{e.at("lon").get<double>(), e.at("lat").get<double>()};
                const int64_t id = e.at("id").get<int64_t>();
                nodes[id] = p;
                Tags tags = readTags(e);
                if (tagOr(tags, "natural") == "tree") trees.insert(p);
                if (!tags.empty() && featureIds.insert(id).second) out.features.push_back({id, p.x, p.y, std::move(tags)});
            } else if (type == "area") {
                const Tags tags = readTags(e);
                if (out.country.empty()) out.country = tagOr(tags, "ISO3166-1", tagOr(tags, "ISO3166-1:alpha2"));
            } else if (type == "way") {
                if (wayIds.insert(e.at("id").get<int64_t>()).second) rawWays.push_back(&e);
            }
        }
    }
    for (const auto* e : rawWays) {
        OsmWay way;
        way.id = e->at("id").get<int64_t>();
        auto ids = e->find("nodes");
        if (ids != e->end())
            for (const auto& id : *ids) {
                auto n = nodes.find(id.get<int64_t>());
                if (n != nodes.end()) way.points.push_back(n->second);
            }
        if (way.points.size() < 2) continue;
        way.tags = readTags(*e);
        const Tags& t = way.tags;
        const bool closed = way.closed();
        if (tagOr(t, "natural") == "tree_row") out.treeRows.push_back(way);
        if (tagOr(t, "natural") == "coastline") out.coastlines.push_back(way);
        if (in(tag(t, "man_made"), {"pier", "breakwater", "groyne", "quay"}) ||
            tagOr(t, "leisure") == "marina" || has(t, "harbour"))
            out.maritime.push_back(way);
        if (has(t, "building") && closed) out.buildings.push_back(way);
        if (has(t, "highway")) out.roads.push_back(way);
        if (has(t, "aeroway")) out.aeroways.push_back(way);
        if (closed && (tagOr(t, "landuse") == "military" || has(t, "military"))) out.military.push_back(way);
        if ((in(tag(t, "landuse"), {"forest", "meadow", "grass", "village_green", "recreation_ground", "orchard"}) ||
             in(tag(t, "natural"), {"wood", "scrub", "grassland", "shrubbery", "wetland"}) ||
             in(tag(t, "leisure"), {"park", "garden"})) && closed)
            out.vegetation.push_back(way);
        if (has(t, "waterway") || tagOr(t, "natural") == "water" || has(t, "water")) out.waterways.push_back(way);
        if (closed && !has(t, "building")) {
            bool cover = false;
            for (const char* k : kLandcoverKeys) cover |= has(t, k);
            if (cover) out.landcover.push_back(way);
        }
    }
    auto byId = [](const auto& a, const auto& b) { return a.id < b.id; };
    for (auto* list : {&out.buildings, &out.roads, &out.vegetation, &out.waterways, &out.landcover,
                       &out.treeRows, &out.coastlines, &out.maritime, &out.aeroways, &out.military})
        std::stable_sort(list->begin(), list->end(), byId);
    std::stable_sort(out.features.begin(), out.features.end(), byId);
    out.trees.assign(trees.begin(), trees.end());
    return out;
}

}  // namespace r1
