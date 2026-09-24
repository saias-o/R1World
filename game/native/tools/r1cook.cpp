// Cook tiles headless, from the observations on disk, and say what came out.
//
//   r1cook --game <root> [--glb <dir>] [--repeat n] <row> <col> [<row> <col>...]
//
// It never touches the network: a tile whose observations are not cached is
// reported as such. `--glb` writes each tile's geometry for inspection. The
// output is one JSON line per tile: what came out, and how long it took.
#include "gen/cook.hpp"
#include "gen/palette.hpp"
#include "gen/sources.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    std::string game = ".", glb;
    int repeat = 1;
    std::vector<r1::Tile> tiles;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--glb" && i + 1 < argc) glb = argv[++i];
        else if (a == "--repeat" && i + 1 < argc) repeat = std::atoi(argv[++i]);
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
            auto document = store.osm(tile, shared);
            auto ground = store.ground(tile);
            if (!document || !ground) {
                line["error"] = "observations not cached";
                std::cout << line.dump() << std::endl;
                ++failures;
                continue;
            }
            r1::Observations in;
            in.tile = tile;
            const auto parseStart = std::chrono::steady_clock::now();
            in.osm = std::make_shared<const r1::OsmData>(r1::normalizeOsm(*document));
            line["parseMs"] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parseStart).count();
            in.elevations = ground->first;
            in.elevationSource = ground->second;
            r1::CookedTile cooked;
            double best = 1e300;
            for (int k = 0; k < repeat; ++k) {
                cooked = r1::cookTile(in);
                best = std::min(best, cooked.cookMs);
            }
            line["cookMs"] = best;
            line["vertices"] = cooked.manifest["vertices"];
            line["buildings"] = cooked.manifest["buildings"];
            nlohmann::json parts = nlohmann::json::array();
            for (const auto& p : cooked.parts)
                parts.push_back({{"name", p.name}, {"vertices", p.mesh.vertexCount()}, {"triangles", p.mesh.indices.size() / 3}});
            line["parts"] = parts;
            line["props"] = cooked.props.size();
            for (const char* k : {"streets", "inference", "props", "nature", "harbour", "ground", "water", "landmarks",
                                  "landmarkReplacedWays", "boats", "decks"})
                line["manifest"][k] = cooked.manifest[k];
            line["manifest"]["traffic"] = {{"nodes", cooked.manifest["traffic"]["nodes"].size()},
                                           {"lanes", cooked.manifest["traffic"]["lanes"].size()},
                                           {"cars", cooked.manifest["traffic"]["cars"]}};
            line["manifest"]["footprints"] = cooked.manifest["footprints"].size();
            if (!glb.empty()) {
                const auto bytes = r1::writeGlb(cooked.parts);
                std::ofstream f(glb + "/" + tile.key() + ".glb", std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
                std::ofstream s(glb + "/" + tile.key() + ".props.json", std::ios::binary);
                s << cooked.props.dump();
            }
        } catch (const std::exception& e) {
            line["error"] = e.what();
            ++failures;
        }
        std::cout << line.dump() << std::endl;
    }
    return failures ? 1 : 0;
}
