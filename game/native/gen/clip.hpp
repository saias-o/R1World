// Polygon booleans, buffers and triangulation, on Clipper2 and earcut.
//
// This is what shapely/GEOS did for the Python worker. Coordinates are
// snapped to an integer grid (`Grid`): a millimetre for engine metres, which
// is exactly the `set_precision(.001)` the street surfaces always asked for,
// and ~0.1 mm for longitudes and latitudes.
#pragma once

#include "polygons.hpp"

#include "clipper2/clipper.h"

namespace r1::clip {
// Offset vertices with bounded mitres; shared by walked axes and soft edges.
std::vector<P2> offsetLine(const std::vector<P2>& line, double distance);

using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;

struct Grid {
    double scale = 1000.0;
    Point64 at(P2 p) const { return Point64(int64_t(std::llround(p.x * scale)), int64_t(std::llround(p.y * scale))); }
    P2 back(const Point64& p) const { return {double(p.x) / scale, double(p.y) / scale}; }
    Path64 path(const std::vector<P2>& points) const;
    Ring ring(const Path64& path) const;
};
inline const Grid kMetres{1000.0};
inline const Grid kDegrees{1e9};

// One polygon with its holes, outer ring first.
struct Polygon {
    Ring outer;
    std::vector<Ring> holes;
};

Paths64 unite(const Paths64& a);
Paths64 unite(const Paths64& a, const Paths64& b);
Paths64 subtract(const Paths64& a, const Paths64& b);
Paths64 intersect(const Paths64& a, const Paths64& b);
// The polygons of a region, holes attached, via a union into a tree.
std::vector<Polygon> polygons(const Paths64& region, const Grid& grid = kMetres);
double area(const Paths64& region, const Grid& grid = kMetres);
// Even-odd over every path: inside an outer ring and not inside its holes.
bool contains(const Paths64& region, P2 point, const Grid& grid = kMetres);
bool contains(const Paths64& region, Point64 point);

// shapely's line.buffer(half, cap_style=flat, join_style=mitre).
Paths64 bufferLine(const std::vector<P2>& line, double half, const Grid& grid = kMetres);
// Flat ends and round joins, so tight road bends keep a continuous edge.
Paths64 bufferLineRoundJoins(const std::vector<P2>& line, double half, const Grid& grid = kMetres);
// A line's one-sided band out to `distance`: left of the direction of travel
// in the numeric (x, y) plane when positive, right when negative.
Paths64 singleSided(const std::vector<P2>& line, double distance, const Grid& grid = kMetres);
// shapely's default buffer (round joins and caps), on a ring.
Paths64 bufferRing(const Ring& ring, double distance, const Grid& grid = kMetres);
Paths64 bufferRound(const std::vector<P2>& line, double distance, const Grid& grid = kMetres);

// Triangles covering a polygon with holes (earcut).
std::vector<std::array<P2, 3>> triangles(const Polygon& polygon);

// Douglas-Peucker, keeping the ends.
std::vector<P2> simplify(const std::vector<P2>& line, double tolerance);

// ── polylines ───────────────────────────────────────────────────────────────
double length(const std::vector<P2>& line);
double distance(const std::vector<P2>& line, P2 p);
// Distance along the line to the point nearest p (shapely's project).
double project(const std::vector<P2>& line, P2 p);
P2 interpolate(const std::vector<P2>& line, double along);
// Distance from a point to a region's boundary, or 0 inside it.
double distance(const std::vector<Polygon>& region, P2 p);

}  // namespace r1::clip
