// Console-only: this target links GLM, never the engine or a window backend.
#include "world/geography.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <set>

namespace w = r1::world;

int main() {
    for (double lat : {-90., -89.999, -33., 0., 48.8566, 89.999, 90.}) {
        for (double lon : {-180., -179.999, 2.3522, 179.999, 180.}) {
            const w::Frame frame(lon, lat, 123.);
            const auto point = w::ecef(w::wrap(lon + .001), std::clamp(lat + .001, -90., 90.), 456.);
            assert(glm::length(frame.origin + frame.basis * frame.local(point) - point) < 1e-8);
            assert(glm::length(frame.local(frame.origin)) == 0.);
            assert(std::abs(glm::determinant(frame.basis) - 1.) < 1e-12);
            const auto generated = r1::tileAt(lon, lat);
            assert(w::tileAt(lon, lat).key() == generated.key());
            const auto ring = w::nearby(lon, lat);
            assert(!ring.empty() && ring.front() == w::tileAt(lon, lat));
            assert(std::set<w::Tile>(ring.begin(), ring.end()).size() == ring.size());
            assert(ring.size() <= w::kPolarTiles);
            for (const auto tile : ring) {
                assert(tile.r >= 0 && tile.r <= w::kLastRow);
                assert(tile.c >= 0 && tile.c < r1::columns(tile.r));
            }
            const auto step = w::advance(lon, lat, 500., 500.);
            assert(std::isfinite(step.x) && std::isfinite(step.y));
            assert(step.x >= -180. && step.x < 180. && std::abs(step.y) <= 90.);
            assert(std::abs(w::metresBetween(lon, lat, step.x, step.y) - std::hypot(500., 500.)) < 1e-6);
            assert(w::advance(lon, lat, 0., 0.) == glm::dvec2(lon, lat));
        }
    }
    assert(w::nearby(2.3522, 48.8566).size() == 9);
    assert(w::nearby(0., 90.).size() == 12);
    assert(w::nearby(0., -90.).size() == 12);
    const auto crossing = w::advance(179.999, 0., 500., 0.);
    assert(crossing.x < -179.99);
    for (double pole : {-90., 90.}) {
        const auto centimetres = w::advance(0., pole, .01, 0.);
        assert(std::abs(centimetres.y) < 90.);
        assert(std::abs(w::metresBetween(0., pole, centimetres.x, centimetres.y) - .01) < 1e-8);
    }
    const w::Frame before(2.3522, 48.8566), after(2.36, 48.86);
    const auto point = w::ecef(2.355, 48.859, 42.);
    const auto rebased = glm::transpose(after.basis) *
        (before.origin - after.origin + before.basis * before.local(point));
    assert(glm::length(rebased - after.local(point)) < 1e-8);
    std::cout << "PASS world geography: grid, neighbourhoods, poles, date line and rebasing\n";
}
