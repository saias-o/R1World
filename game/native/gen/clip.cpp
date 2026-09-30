#include "clip.hpp"

#include "mapbox/earcut.hpp"

namespace r1::clip {

using namespace Clipper2Lib;

Path64 Grid::path(const std::vector<P2>& points) const {
    Path64 out;
    out.reserve(points.size());
    for (const P2& p : points) {
        Point64 q = at(p);
        if (out.empty() || !(out.back() == q)) out.push_back(q);
    }
    return out;
}

Ring Grid::ring(const Path64& path) const {
    Ring out;
    out.reserve(path.size());
    for (const auto& p : path) out.push_back(back(p));
    return out;
}

Paths64 unite(const Paths64& a) { return Union(a, FillRule::NonZero); }
Paths64 unite(const Paths64& a, const Paths64& b) { return Union(a, b, FillRule::NonZero); }
Paths64 subtract(const Paths64& a, const Paths64& b) {
    if (a.empty() || b.empty()) return a;
    return Difference(a, b, FillRule::NonZero);
}
Paths64 intersect(const Paths64& a, const Paths64& b) {
    if (a.empty() || b.empty()) return {};
    return Intersect(a, b, FillRule::NonZero);
}

namespace {
void collect(const PolyPath64& node, const Grid& grid, std::vector<Polygon>& out) {
    for (const auto& child : node) {
        // Children of the root and of holes are outers.
        Polygon polygon;
        polygon.outer = grid.ring(child->Polygon());
        for (const auto& hole : *child) {
            polygon.holes.push_back(grid.ring(hole->Polygon()));
            collect(*hole, grid, out);  // islands inside the hole
        }
        out.push_back(std::move(polygon));
    }
}
}  // namespace

std::vector<Polygon> polygons(const Paths64& region, const Grid& grid) {
    std::vector<Polygon> out;
    if (region.empty()) return out;
    Clipper64 c;
    c.AddSubject(region);
    PolyTree64 tree;
    c.Execute(ClipType::Union, FillRule::NonZero, tree);
    collect(tree, grid, out);
    return out;
}

double area(const Paths64& region, const Grid& grid) {
    return std::abs(Area(region)) / (grid.scale * grid.scale);
}

bool contains(const Paths64& region, Point64 point) {
    bool inside = false;
    for (const auto& path : region)
        if (path.size() >= 3 && PointInPolygon(point, path) == PointInPolygonResult::IsInside) inside = !inside;
    return inside;
}
bool contains(const Paths64& region, P2 point, const Grid& grid) { return contains(region, grid.at(point)); }

Paths64 bufferLine(const std::vector<P2>& line, double half, const Grid& grid) {
    Path64 path = grid.path(line);
    if (path.size() < 2) return {};
    // shapely's mitre limit is 5, Clipper's default 2.
    return InflatePaths({path}, half * grid.scale, JoinType::Miter, EndType::Butt, 5.0);
}

Paths64 bufferLineRoundJoins(const std::vector<P2>& line, double half, const Grid& grid) {
    Path64 path = grid.path(line);
    if (path.size() < 2) return {};
    const double tolerance = half * grid.scale * (1.0 - std::cos(kPi / 16.0));
    return InflatePaths({path}, half * grid.scale, JoinType::Round, EndType::Butt, 2.0, tolerance);
}

Paths64 bufferRound(const std::vector<P2>& line, double distance, const Grid& grid) {
    Path64 path = grid.path(line);
    if (path.empty()) return {};
    // Eight segments a quarter circle, as GEOS draws them.
    const double tolerance = distance * grid.scale * (1.0 - std::cos(kPi / 16.0));
    return InflatePaths({path}, distance * grid.scale, JoinType::Round, EndType::Round, 2.0, tolerance);
}

Paths64 bufferRing(const Ring& ring, double distance, const Grid& grid) {
    Path64 path = grid.path(ring);
    if (path.size() >= 2 && path.front() == path.back()) path.pop_back();
    if (path.size() < 3) return {};
    // buffer(0) repairs; a positive distance also rounds the corners.
    Paths64 clean = Union({path}, FillRule::NonZero);
    if (distance == 0.0) return clean;
    const double tolerance = std::abs(distance) * grid.scale * (1.0 - std::cos(kPi / 16.0));
    return InflatePaths(clean, distance * grid.scale, JoinType::Round, EndType::Polygon, 2.0, tolerance);
}

Paths64 singleSided(const std::vector<P2>& line, double distance, const Grid& grid) {
    std::vector<P2> pts;
    for (const P2& p : line) if (pts.empty() || dist(pts.back(), p) > 1e-9) pts.push_back(p);
    if (pts.size() < 2) return {};
    // Union the segment bands first. At a bend the outer side needs a join;
    // joining the two offset endpoints with a circular sector avoids the
    // spikes and self-intersections of a long mitre on a hairpin.
    Paths64 pieces;
    auto addPiece = [&](const std::vector<P2>& outline) {
        Path64 p = grid.path(outline);
        if (Area(p) < 0) std::reverse(p.begin(), p.end());
        if (p.size() >= 3) pieces.push_back(std::move(p));
    };
    const double radius = std::abs(distance);
    const double sign = distance > 0 ? 1.0 : -1.0;
    std::vector<P2> normals;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const double dx = pts[i + 1].x - pts[i].x, dy = pts[i + 1].y - pts[i].y;
        const double len = std::hypot(dx, dy);
        const P2 n{-dy / len * sign, dx / len * sign};
        normals.push_back(n);
        addPiece({pts[i], pts[i + 1],
                  {pts[i + 1].x + n.x * radius, pts[i + 1].y + n.y * radius},
                  {pts[i].x + n.x * radius, pts[i].y + n.y * radius}});
    }
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        const P2 a = normals[i - 1], b = normals[i];
        const double cross = a.x * b.y - a.y * b.x;
        if (cross * sign >= -1e-12) continue;  // the inner bands overlap
        const double sweep = std::atan2(cross, a.x * b.x + a.y * b.y);
        const int steps = std::max(1, int(std::ceil(std::abs(sweep) / (kPi / 16.0))));
        const double angle = std::atan2(a.y, a.x);
        std::vector<P2> sector{pts[i]};
        for (int j = 0; j <= steps; ++j) {
            const double t = angle + sweep * j / steps;
            sector.push_back({pts[i].x + radius * std::cos(t), pts[i].y + radius * std::sin(t)});
        }
        addPiece(sector);
    }
    return Union(pieces, FillRule::NonZero);
}

std::vector<std::array<P2, 3>> triangles(const Polygon& polygon) {
    using Point = std::array<double, 2>;
    std::vector<std::vector<Point>> rings;
    std::vector<P2> flat;
    auto add = [&](const Ring& r) {
        std::vector<Point> ring;
        for (const P2& p : r) { ring.push_back({p.x, p.y}); flat.push_back(p); }
        rings.push_back(std::move(ring));
    };
    add(polygon.outer);
    for (const Ring& h : polygon.holes) add(h);
    const std::vector<uint32_t> index = mapbox::earcut<uint32_t>(rings);
    std::vector<std::array<P2, 3>> out;
    out.reserve(index.size() / 3);
    for (size_t i = 0; i + 2 < index.size(); i += 3) out.push_back({flat[index[i]], flat[index[i + 1]], flat[index[i + 2]]});
    return out;
}

namespace {
double segmentDistance(P2 p, P2 a, P2 b, double* along = nullptr) {
    const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    double t = l2 < 1e-24 ? 0.0 : ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2;
    t = std::max(0.0, std::min(1.0, t));
    if (along) *along = t * std::sqrt(l2);
    return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

void douglasPeucker(const std::vector<P2>& line, size_t first, size_t last, double tolerance, std::vector<bool>& keep) {
    if (last <= first + 1) return;
    double worst = -1;
    size_t index = first;
    for (size_t i = first + 1; i < last; ++i) {
        const double d = segmentDistance(line[i], line[first], line[last]);
        if (d > worst) { worst = d; index = i; }
    }
    if (worst > tolerance) {
        keep[index] = true;
        douglasPeucker(line, first, index, tolerance, keep);
        douglasPeucker(line, index, last, tolerance, keep);
    }
}
}  // namespace

std::vector<P2> simplify(const std::vector<P2>& line, double tolerance) {
    if (line.size() < 3) return line;
    std::vector<bool> keep(line.size(), false);
    keep.front() = keep.back() = true;
    douglasPeucker(line, 0, line.size() - 1, tolerance, keep);
    std::vector<P2> out;
    for (size_t i = 0; i < line.size(); ++i) if (keep[i]) out.push_back(line[i]);
    return out;
}

double length(const std::vector<P2>& line) {
    double total = 0;
    for (size_t i = 1; i < line.size(); ++i) total += dist(line[i - 1], line[i]);
    return total;
}

double distance(const std::vector<P2>& line, P2 p) {
    if (line.size() == 1) return dist(line[0], p);
    double best = 1e300;
    for (size_t i = 1; i < line.size(); ++i) best = std::min(best, segmentDistance(p, line[i - 1], line[i]));
    return best;
}

double project(const std::vector<P2>& line, P2 p) {
    double best = 1e300, bestAlong = 0, run = 0;
    for (size_t i = 1; i < line.size(); ++i) {
        double along = 0;
        const double d = segmentDistance(p, line[i - 1], line[i], &along);
        if (d < best) { best = d; bestAlong = run + along; }
        run += dist(line[i - 1], line[i]);
    }
    return bestAlong;
}

P2 interpolate(const std::vector<P2>& line, double along) {
    if (line.empty()) return {};
    if (along <= 0) return line.front();
    double run = 0;
    for (size_t i = 1; i < line.size(); ++i) {
        const double seg = dist(line[i - 1], line[i]);
        if (run + seg >= along && seg > 0) {
            const double t = (along - run) / seg;
            return {line[i - 1].x + (line[i].x - line[i - 1].x) * t, line[i - 1].y + (line[i].y - line[i - 1].y) * t};
        }
        run += seg;
    }
    return line.back();
}

double distance(const std::vector<Polygon>& region, P2 p) {
    double best = 1e300;
    for (const Polygon& poly : region) {
        bool inside = pointInPolygon(p, poly.outer);
        for (const Ring& h : poly.holes) if (pointInPolygon(p, h)) inside = false;
        if (inside) return 0.0;
        auto edges = [&](const Ring& r) {
            for (size_t i = 0; i < r.size(); ++i) best = std::min(best, segmentDistance(p, r[i], r[(i + 1) % r.size()]));
        };
        edges(poly.outer);
        for (const Ring& h : poly.holes) edges(h);
    }
    return best;
}

}  // namespace r1::clip
