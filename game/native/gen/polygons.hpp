// Planar polygon primitives shared by the generators. Points are (x, y)
// pairs; the generators pass the engine's (x, z).
#pragma once

#include "common.hpp"

namespace r1 {

using Ring = std::vector<P2>;

inline double cross2(P2 a, P2 b, P2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }
// Signed area; positive is counter-clockwise in the numeric plane.
double polygonArea(const Ring& points);
P2 centroid(const Ring& points);
bool pointInTriangle(P2 p, P2 a, P2 b, P2 c);
// Deterministic ear clipping for simple footprints: index triples.
std::vector<std::array<int, 3>> triangulate(const Ring& points);
bool pointInPolygon(P2 point, const Ring& polygon);
// Monotone chain hull, counter-clockwise, first point not repeated.
Ring convexHull(Ring points);
// Shrink a ring toward its centroid by roughly `distance`; the ring itself
// when it is too small to inset without folding.
Ring insetPolygon(const Ring& points, double distance);

}  // namespace r1
