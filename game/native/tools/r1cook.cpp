// Cook tiles headless, from the observations on disk, and say what came out.
//
//   r1cook --game <root> [--glb <dir>] [--repeat n] [--tile-target n] [--fetch] <row> <col> [<row> <col>...]
//   r1cook --game <root> --at <lon> <lat> ...   the tile under a point, instead of a row and column
//
// It never touches the network unless `--fetch` says it may: a tile whose
// observations are not cached is then downloaded exactly as the game would
// (its neighbourhood's query, its terrain) and kept; without it, it is
// reported as not cached. `--glb` writes each tile's geometry for inspection. The
// output is one JSON line per tile: what came out, and how long it took.
#include "gen/cook.hpp"
#include "gen/palette.hpp"
#include "gen/relief.hpp"
#include "gen/sources.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    std::string game = ".", glb;
    int repeat = 1;
    bool fetch = false;
    size_t tileTarget = r1::kTileVertexBudget;
    std::vector<r1::Tile> tiles;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--glb" && i + 1 < argc) glb = argv[++i];
        else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (a == "--tile-target" && i + 1 < argc) tileTarget = std::stoull(argv[++i]);
        else if (a == "--fetch") fetch = true;
        else if (a == "--at" && i + 2 < argc) { tiles.push_back(r1::tileAt(std::stod(argv[i + 1]), std::stod(argv[i + 2]))); i += 2; }
        else if (i + 1 < argc) { tiles.push_back({std::atoi(argv[i]), std::atoi(argv[i + 1])}); ++i; }
    }
    try {
        r1::loadPalette(game);
    } catch (const std::exception& e) {
        std::cerr << "PALETTE-FAILED " << e.what() << "\n";
        return 2;
    }
    r1::ObservationStore store(game);
    int failures = 0;
    for (const r1::Tile& tile : tiles) {
        nlohmann::json line = {{"key", tile.key()}, {"row", tile.row}, {"col", tile.col}};
        try {
            // The neighbourhood's shared query, as the game would group it.
            std::vector<r1::Tile> group;
            for (int dr = -1; dr <= 1; ++dr) {
                const int row = tile.row + dr, n = r1::columns(row);
                const r1::P2 c = tile.center();
                const int col = int(std::floor((r1::wrap(c.x) + 180.0) / 360.0 * n));
                for (int dc = -1; dc <= 1; ++dc) group.push_back({row, (col + dc + n) % n});
            }
            const auto shared = store.shared(group);
            bool stale = false;
            auto document = store.osm(tile, shared, &stale);
            auto ground = store.ground(tile);
            if (fetch && (!document || stale)) {
                if (shared) store.fetchOsm(shared->region, shared->path);
                else store.fetchOsm(tile.bounds(), store.tileFolder(tile) + "/osm.json");
                document = store.osm(tile, shared, &stale);
            }
            if (fetch && !ground) ground = store.fetchGround(tile);
            // The aero layer beside an answer older than it, as the game reads it.
            std::optional<nlohmann::json> aero;
            if (const auto path = store.osmPath(tile, shared)) {
                line["osm"] = *path;
                bool needed = false;
                auto layer = store.aeroPath(tile, shared, *path, needed);
                if (needed && !layer && fetch) {
                    const auto target = store.aeroTarget(tile, shared);
                    store.fetchAero(target.region, target.path);
                    layer = target.path;
                }
                if (layer) { aero = r1::readJson(*layer); line["aero"] = *layer; }
                else if (needed) line["airportsPending"] = true;
            }
            if (!document || !ground) {
                line["error"] = "observations not cached";
                std::cout << line.dump() << std::endl;
                ++failures;
                continue;
            }
            r1::Observations in;
            in.tile = tile;
            const auto parseStart = std::chrono::steady_clock::now();
            std::optional<nlohmann::json> retail;
            if(const auto path=store.osmPath(tile,shared)) {
                auto layer=store.retailPath(tile,shared,*path);
                if((!layer||!store.retailCurrent(*layer))&&fetch&&store.queryVersion(*path)<r1::kOsmRetailVersion) {
                    auto target=store.retailTarget(tile,shared);retail=store.fetchRetail(target.region,target.path);
                } else if(layer)retail=r1::readJson(*layer);
            }
            in.osm = std::make_shared<const r1::OsmData>(r1::normalizeOsm(*document, aero ? &*aero : nullptr,retail?&*retail:nullptr));
            in.canopy = r1::storedCanopy(game, tile);
            if (!in.canopy && fetch) {
                for (const auto& [t, c] : r1::fetchCanopyBand(tile)) r1::storeCanopy(game, t, c);
                in.canopy = r1::storedCanopy(game, tile);
            }
            // The surveyed summits, as the game reads them (gen/peaks).
            in.peaks = store.peaks(tile, *in.osm, &in.peaksSource);
            in.airportsPending = line.value("airportsPending", false);
            line["parseMs"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parseStart).count();
            // Joined to the neighbours' ground as the game joins it (gen/seams),
            // from what is cached: the survey on disk, else the installed layer.
            r1::JoinedGround joined = r1::joinedGround(tile, {ground->first, r1::groundRank(ground->second)},
                [&](const r1::Tile& t) -> std::optional<r1::RankedGround> {
                    try {
                        if (auto g = store.ground(t)) return r1::RankedGround{g->first, r1::groundRank(g->second)};
                        if (auto g = r1::installedGround(t.bounds(), game)) return r1::RankedGround{*g, r1::groundRank(r1::kInstalledReliefSource)};
                    } catch (const std::exception&) {
                    }
                    return std::nullopt;
                });
            in.elevations = std::move(joined.own);
            in.around = std::move(joined.around);
            in.seams = joined.seams;
            if (const auto path = store.osmPath(tile, shared)) in.osmExtent = store.regionOf(tile, shared, *path);
            in.elevationSource = ground->second;
            in.targetVertices = tileTarget;
            r1::CookedTile cooked;
            double best = 1e300;
            for (int k = 0; k < repeat; ++k) {
                cooked = r1::cookTile(in);
                best = std::min(best, cooked.cookMs);
            }
            line["cookMs"] = best;
            line["vertices"] = cooked.manifest["vertices"];
            line["buildingGeometryLod"] = cooked.manifest["buildingGeometryLod"];
            line["buildings"] = cooked.manifest["buildings"];
            line["predicted"] = cooked.manifest["predicted"];
            line["bridges"] = cooked.manifest["bridges"];
            line["elevationSource"] = cooked.manifest["elevationSource"];
            line["grass"] = cooked.manifest["grass"];
            line["peaks"] = cooked.manifest["peaks"];
            nlohmann::json parts = nlohmann::json::array();
            for (const auto& p : cooked.parts)
                parts.push_back({{"name", p.name}, {"vertices", p.mesh.vertexCount()}, {"triangles", p.mesh.indices.size() / 3}});
            line["parts"] = parts;
            line["props"] = cooked.props.size();
            for (const char* k : {"streets", "inference", "props", "nature", "harbour", "ground", "water", "landmarks",
                                  "landmarkReplacedWays", "boats", "decks", "airports", "aircraft", "osmQueryVersion", "retail", "interiors",
                                  "fuel", "lettering", "interiorStreaming"})
                line["manifest"][k] = cooked.manifest[k];
            line["manifest"]["traffic"] = {{"nodes", cooked.manifest["traffic"]["nodes"].size()},
                                           {"lanes", cooked.manifest["traffic"]["lanes"].size()},
                                           {"cars", cooked.manifest["traffic"]["cars"]}};
            line["manifest"]["footprints"] = cooked.manifest["footprints"].size();
            const auto& crowd = cooked.manifest["crowd"];
            line["manifest"]["crowd"] = {{"people",crowd["people"]}, {"inputs",crowd["inputs"]},
                                            {"nodes",crowd["nodes"].size()}, {"links",crowd["links"].size()}};
            if (!glb.empty()) {
                const auto bytes = r1::writeGlb(cooked.parts);
                std::ofstream f(glb + "/" + tile.key() + ".glb", std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                std::ofstream s(glb + "/" + tile.key() + ".props.json", std::ios::binary);
                s << cooked.props.dump();
                std::ofstream r(glb + "/" + tile.key() + ".raised.json", std::ios::binary);
                r << cooked.manifest["raised"].dump();
            }
        } catch (const std::exception& e) {
            line["error"] = e.what();
            ++failures;
        }
        std::cout << line.dump() << std::endl;
    }
    return failures ? 1 : 0;
}
