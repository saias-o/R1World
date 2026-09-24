// The foundations: seeded draws, tiles, frames, polygons, the cache's names.
#include "check.hpp"

#include "gen/common.hpp"
#include "gen/inflate.hpp"
#include "gen/polygons.hpp"
#include "gen/sources.hpp"

using namespace r1;

// ── seeded draws: CPython's, to the bit ─────────────────────────────────────
// The values on the right were printed by CPython 3.12. They are what every
// building the Python worker ever cooked was drawn from; the C++ draws them
// again, so a street keeps the houses it had.

TEST(Random, a_seed_draws_what_python_drew) {
    PyRandom r(12345);
    NEAR(r.random(), 0.41661987254534116, 0);
}

TEST(Random, a_feature_seed_draws_what_python_drew) {
    PyRandom r = seeded(123456789, 0x57414C4C);
    NEAR(r.random(), 0.3295804851846986, 0);
    NEAR(r.random(), 0.19336247190390465, 0);
}

TEST(Random, a_seed_wider_than_64_bits_draws_what_python_drew) {
    const __int128 ident = -(__int128(987654321) * 1000003 + 5 * 73856093 + 7 * 19349663);
    PyRandom r = seeded(ident, 0x4E4154);
    NEAR(r.random(), 0.9309145786976246, 0);
}

TEST(Random, randrange_and_gauss_draw_what_python_drew) {
    PyRandom r(42);
    const uint32_t expected[] = {5, 0, 0, 5, 2};
    for (uint32_t e : expected) CHECK(r.randrange(7) == e);
    PyRandom g(7);
    NEAR(g.gauss(0, 1), -0.2558802884476004, 1e-15);
    NEAR(g.gauss(0, 1), 0.511431512516514, 1e-15);
}

// ── tiles ───────────────────────────────────────────────────────────────────

TEST(Tiles, paris_is_the_tile_python_named) {
    const Tile t = tileAt(2.3522, 48.8566);
    CHECK(t.row == 27771 && t.col == 23995);
    const Bounds b = t.bounds();
    NEAR(b.south, 48.85499999999999, 1e-12);
    NEAR(b.west, 2.352071942749774, 1e-12);
    NEAR(b.east, 2.359671528994511, 1e-12);
}

TEST(Tiles, the_whole_globe_and_the_date_line_resolve) {
    for (double lat : {-90.0, -89.999, -33.0, 0.0, 48.8566, 89.999, 90.0})
        for (double lon : {-180.0, -179.999, 2.3522, 179.999, 180.0}) {
            const Tile t = tileAt(lon, lat);
            CHECK(t.row >= 0 && t.row < kRows && t.col >= 0 && t.col < columns(t.row));
            const Bounds b = t.bounds();
            const double w = wrap(lon);
            CHECK_MSG(b.south - 1e-9 <= lat && lat <= b.north + 1e-9, lon << "," << lat);
            CHECK_MSG(b.west - 1e-9 <= w && w <= b.east + 1e-9, lon << "," << lat);
        }
}

TEST(Tiles, a_tile_centre_is_in_its_own_tile) {
    for (int row = 0; row < kRows; row += 113)
        for (int col : {0, columns(row) / 2, columns(row) - 1}) {
            const Tile t{row, col};
            const P2 c = t.center();
            CHECK(tileAt(c.x, c.y) == t);
        }
}

TEST(Tiles, tiles_are_about_555_metres_everywhere) {
    for (double lat : {0.0, 45.0, 70.0, 85.0}) {
        const Bounds b = tileAt(10.0, lat).bounds();
        const double wide = (b.east - b.west) * 111320.0 * std::cos(radians(lat));
        const double tall = (b.north - b.south) * 110574.0;
        CHECK_MSG(wide > 480 && wide < 620, lat << " " << wide);
        CHECK_MSG(tall > 540 && tall < 570, lat << " " << tall);
    }
}

TEST(Tiles, the_key_carries_the_generator_version) {
    CHECK((Tile{27771, 23995}.key() == "v" + std::to_string(kVersion) + "_27771_23995"));
}

// ── frames ──────────────────────────────────────────────────────────────────

TEST(Geodesy, geodetic_and_ecef_round_trip_under_a_micrometre) {
    for (double lat : {-89.9, -45.0, 0.0, 48.8566, 89.9})
        for (double lon : {-179.9, 0.0, 2.35, 139.7})
            for (double alt : {-400.0, 0.0, 8848.0}) {
                const P3 e = geodeticToEcef(lon, lat, alt);
                const P3 g = ecefToGeodetic(e.x, e.y, e.z);
                // Horizontal error on the sphere, and altitude error, each under a micrometre.
                const double dlat = radians(g.y - lat), dlon = radians(g.x - lon);
                const double h = std::sin(dlat / 2) * std::sin(dlat / 2) +
                                 std::cos(radians(lat)) * std::cos(radians(g.y)) * std::sin(dlon / 2) * std::sin(dlon / 2);
                NEAR(2 * kRMean * std::asin(std::sqrt(h)), 0.0, 1e-6);
                NEAR(g.z, alt, 1e-6);
            }
}

TEST(Geodesy, engine_axes_are_east_up_and_south) {
    const Anchor a = Anchor::at(2.35, 48.85, 0);
    const P3 east = a.toEngine(2.351, 48.85, 0), north = a.toEngine(2.35, 48.851, 0), up = a.toEngine(2.35, 48.85, 10);
    CHECK(east.x > 70 && std::abs(east.z) < 1);
    CHECK(north.z < -100 && std::abs(north.x) < 1);
    NEAR(up.y, 10.0, 1e-6);
}

TEST(Geodesy, an_anchor_round_trips_near_and_far_points) {
    const Anchor a = Anchor::at(6.87, 45.92, 1035.0);
    for (P3 p : {P3{6.871, 45.921, 1100.0}, P3{8.0, 46.5, 4000.0}}) {
        const P3 e = a.toEngine(p.x, p.y, p.z);
        const P3 g = a.toGeodetic(e.x, e.y, e.z);
        NEAR(g.x, p.x, 1e-9);
        NEAR(g.y, p.y, 1e-9);
        NEAR(g.z, p.z, 1e-4);
    }
}

// ── polygons ────────────────────────────────────────────────────────────────

TEST(Polygons, a_square_is_two_triangles_covering_it) {
    const Ring square{{0, 0}, {4, 0}, {4, 4}, {0, 4}};
    const auto tris = triangulate(square);
    CHECK(tris.size() == 2);
    double area = 0;
    for (const auto& t : tris) area += std::abs(cross2(square[size_t(t[0])], square[size_t(t[1])], square[size_t(t[2])])) / 2;
    NEAR(area, 16.0, 1e-12);
}

TEST(Polygons, a_concave_ring_triangulates_without_leaving_it) {
    const Ring el{{0, 0}, {16, 0}, {16, 5}, {6, 5}, {6, 14}, {0, 14}};
    double area = 0;
    for (const auto& t : triangulate(el)) {
        const P2 a = el[size_t(t[0])], b = el[size_t(t[1])], c = el[size_t(t[2])];
        area += std::abs(cross2(a, b, c)) / 2;
        CHECK(pointInPolygon({(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3}, el));
    }
    NEAR(area, std::abs(polygonArea(el)), 1e-9);
}

TEST(Polygons, the_hull_ignores_the_order_the_way_was_drawn_in) {
    Ring a{{0, 0}, {10, 0}, {10, 5}, {5, 2}, {0, 5}};
    Ring b(a.rbegin(), a.rend());
    CHECK(convexHull(a) == convexHull(b));
    CHECK(convexHull(a).size() == 4);
}

// ── the cache ───────────────────────────────────────────────────────────────

TEST(Sources, a_neighbourhood_file_has_the_name_python_gave_it) {
    // The same nine tiles the Python worker grouped around Paris: the file it
    // wrote must be the file the game finds, or a visited place goes online.
    const Tile c = tileAt(2.3522, 48.8566);
    std::vector<Tile> group;
    for (int dr = -1; dr <= 1; ++dr)
        for (int dc = -1; dc <= 1; ++dc) group.push_back({c.row + dr, c.col + dc});
    const auto shared = ObservationStore(".").shared(group);
    CHECK(shared.has_value());
    CHECK_MSG(shared->path.size() > 25 && shared->path.substr(shared->path.size() - 25) == "c991f49083d03c35900e.json",
              shared->path);
}

TEST(Sources, too_wide_a_group_is_not_one_query) {
    std::vector<Tile> group{{27771, 23990}, {27771, 24100}};
    CHECK(!ObservationStore(".").shared(group).has_value());
}

TEST(Sources, sha256_is_sha256) {
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// ── inflate ─────────────────────────────────────────────────────────────────

TEST(Inflate, a_zlib_stream_comes_back_whole) {
    // zlib.compress(bytes(range(256)) * 20 + b"R1World sea prior " * 50, 9)
    const std::string hex =
        "78da6360646266616563e7e0e4e2e6e1e5e3171014121611151397909492969195935750545256515553d7d0d4d2d6d1d5d3373034"
        "3236313533b7b0b4b2b6b1b5b37770747276717573f7f0f4f2f6f1f5f30f080c0a0e090d0b8f888c8a8e898d8b4f484c4a4e494d4bcf"
        "c8cccacec9cdcb2f282c2a2e292d2bafa8acaaaea9adab6f686c6a6e696d6befe8eceaeee9edeb9f3071d2e42953a74d9f3173d6ec39"
        "73e7cd5fb070d1e2254b972d5fb172d5ea356bd7addfb071d3e62d5bb76ddfb173d7ee3d7bf7ed3f70f0d0e123478f1d3f71f2d4e933"
        "67cf9dbf70f1d2e52b57af5dbf71f3d6ed3b77efdd7ff0f0d1e3274f9f3d7ff1f2d5eb376fdfbdfff0f1d3e72f5fbf7dfff1f3d7ef3f"
        "7ffffd6718f5ffa8ff47fd3feaff51ff8ffa7fd4ffa3fe1ff5ffa8ff47fd3feaff51ff8ffa7fd4ffa3fe1ff5ffa8ff87b1ff830cc3f3"
        "8b7252148a5313150a8a32f38b14464546454645e829020082d63246";
    std::vector<uint8_t> packed;
    for (size_t i = 0; i < hex.size(); i += 2) packed.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
    const auto out = inflateZlib(packed.data(), packed.size());
    std::string expected;
    for (int k = 0; k < 20; ++k) for (int b = 0; b < 256; ++b) expected += char(b);
    for (int k = 0; k < 50; ++k) expected += "R1World sea prior ";
    CHECK(std::string(out.begin(), out.end()) == expected);
}

TEST(Inflate, a_stream_that_is_not_zlib_is_refused_not_guessed) {
    const uint8_t junk[] = {1, 2, 3, 4, 5};
    bool refused = false;
    try { inflateZlib(junk, sizeof junk); } catch (const std::runtime_error&) { refused = true; }
    CHECK(refused);
}
