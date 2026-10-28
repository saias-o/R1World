#pragma once
#include "gen/common.hpp"
#include <glm/glm.hpp>
#include <tuple>
#include <vector>

namespace r1::world {
constexpr double rad = 3.141592653589793 / 180.;
constexpr double kTwoPi = 6.283185307179586;
double wrap(double x);
// The tile grid is the generator's (gen/common.hpp), never a copy of it: a
// second copy rounded a row boundary differently and put the player on a
// tile the generator had not cooked under him.
using r1::columns;
constexpr int kLastRow=r1::kRows-1;
// Metres in a degree of the equator: the spherical shortcut local estimates use.
constexpr double kMetresPerDegree=r1::kMetresPerDegree;
struct Tile {
    int r,c;
    // The generator's own key (gen/common.hpp), so the tile the game streams
    // and the tile the generator cooks cannot disagree about their version.
    std::string key() const { return r1::Tile{r,c}.key(); }
    r1::Tile gen() const { return {r,c}; }
    bool operator<(const Tile& b) const { return std::tie(r,c)<std::tie(b.r,b.c); }
    bool operator==(const Tile& b) const { return r==b.r&&c==b.c; }
};

Tile tileAt(double lon, double lat);
double metresBetween(double lon1, double lat1, double lon2, double lat2);
// Within some 18 km of a pole a ring is a few wedges, and the three rows
// around the player no longer hold what he sees: standing on the pole, the
// ground a hundred metres away across it is in a column no row offset
// reaches. There the neighbourhood is every tile whose nearest point is
// within reach, in metres, nearest first -- twelve at the pole itself.
constexpr int kPolarColumns=200;
constexpr double kPolarReach=650.;
constexpr size_t kPolarTiles=12;

std::vector<Tile> polarNearby(double lon, double lat);
std::vector<Tile> nearby(double lon, double lat);
glm::dvec3 ecef(double lon, double lat, double alt=0);
struct Frame {
    glm::dvec3 origin; glm::dmat3 basis;
    Frame(double lon=0,double lat=0,double alt=0):origin(ecef(lon,lat,alt)) {
        double l=lon*rad,p=lat*rad;
        basis=glm::dmat3(glm::dvec3(-sin(l),cos(l),0),
                        glm::dvec3(cos(p)*cos(l),cos(p)*sin(l),sin(p)),
                        glm::dvec3(sin(p)*cos(l),sin(p)*sin(l),-cos(p)));
    }
    glm::dvec3 local(glm::dvec3 p) const { return glm::transpose(basis)*(p-origin); }
};

glm::dvec2 advance(double lon, double lat, double east, double north);

} // namespace r1::world
