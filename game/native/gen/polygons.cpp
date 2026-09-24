#include "polygons.hpp"

#include <algorithm>

namespace r1 {

double polygonArea(const Ring& p) {
    double sum = 0;
    const size_t n = p.size();
    for (size_t i = 0; i < n; ++i) sum += p[i].x * p[(i + 1) % n].y - p[(i + 1) % n].x * p[i].y;
    return 0.5 * sum;
}

P2 centroid(const Ring& p) {
    const double area = polygonArea(p);
    if (std::abs(area) < 1e-9) {
        const double count = double(std::max<size_t>(1, p.size()));
        double sx = 0, sy = 0;
        for (const P2& q : p) { sx += q.x; sy += q.y; }
        return {sx / count, sy / count};
    }
    double cx = 0, cy = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const P2 c = p[i], n = p[(i + 1) % p.size()];
        const double w = c.x * n.y - n.x * c.y;
        cx += (c.x + n.x) * w;
        cy += (c.y + n.y) * w;
    }
    return {cx / (6.0 * area), cy / (6.0 * area)};
}

bool pointInTriangle(P2 p, P2 a, P2 b, P2 c) {
    const double ab = cross2(a, b, p), bc = cross2(b, c, p), ca = cross2(c, a, p);
    return (ab >= -1e-8 && bc >= -1e-8 && ca >= -1e-8) || (ab <= 1e-8 && bc <= 1e-8 && ca <= 1e-8);
}

std::vector<std::array<int, 3>> triangulate(const Ring& points) {
    std::vector<std::array<int, 3>> triangles;
    const int n = int(points.size());
    if (n < 3) return triangles;
    std::vector<int> order(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) order[size_t(i)] = i;
    if (polygonArea(points) < 0.0) std::reverse(order.begin(), order.end());
    long guard = long(order.size()) * long(order.size());
    while (order.size() > 3 && guard > 0) {
        --guard;
        bool clipped = false;
        const int m = int(order.size());
        for (int cursor = 0; cursor < m; ++cursor) {
            const int i0 = order[size_t((cursor - 1 + m) % m)], i1 = order[size_t(cursor)],
                      i2 = order[size_t((cursor + 1) % m)];
            if (cross2(points[size_t(i0)], points[size_t(i1)], points[size_t(i2)]) <= 1e-8) continue;
            bool inside = false;
            for (int index : order) {
                if (index == i0 || index == i1 || index == i2) continue;
                if (pointInTriangle(points[size_t(index)], points[size_t(i0)], points[size_t(i1)], points[size_t(i2)])) {
                    inside = true;
                    break;
                }
            }
            if (inside) continue;
            triangles.push_back({i0, i1, i2});
            order.erase(order.begin() + cursor);
            clipped = true;
            break;
        }
        if (!clipped) break;
    }
    if (order.size() == 3) triangles.push_back({order[0], order[1], order[2]});
    if (triangles.empty())
        for (int i = 1; i < n - 1; ++i) triangles.push_back({0, i, i + 1});
    return triangles;
}

bool pointInPolygon(P2 point, const Ring& polygon) {
    bool inside = false;
    size_t j = polygon.size() - 1;
    for (size_t i = 0; i < polygon.size(); ++i) {
        const P2 current = polygon[i], previous = polygon[j];
        if ((current.y > point.y) != (previous.y > point.y)) {
            const double edgeX = (previous.x - current.x) * (point.y - current.y) / (previous.y - current.y) + current.x;
            if (point.x < edgeX) inside = !inside;
        }
        j = i;
    }
    return inside;
}

Ring convexHull(Ring points) {
    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) return points;
    Ring lower, upper;
    for (const P2& p : points) {
        while (lower.size() >= 2 && cross2(lower[lower.size() - 2], lower.back(), p) <= 0.0) lower.pop_back();
        lower.push_back(p);
    }
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        while (upper.size() >= 2 && cross2(upper[upper.size() - 2], upper.back(), *it) <= 0.0) upper.pop_back();
        upper.push_back(*it);
    }
    lower.pop_back(); upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

Ring insetPolygon(const Ring& points, double distance) {
    if (points.size() < 3) return points;
    const P2 c = centroid(points);
    Ring out;
    for (const P2& p : points) {
        const double dx = p.x - c.x, dy = p.y - c.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        if (length <= distance * 1.5) return points;
        const double scale = (length - distance) / length;
        out.push_back({c.x + dx * scale, c.y + dy * scale});
    }
    return out;
}

}  // namespace r1
