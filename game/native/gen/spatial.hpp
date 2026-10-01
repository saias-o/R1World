// Spatial lookups that answer exactly what a full scan would.
//
// A neighbourhood carries tens of thousands of nodes, roads and outlines, and
// a stage asking "which of them is in this building" of every building pays
// for the product. These indices only skip what cannot match: candidates come
// back in the order of the scan they replace, so the first match, every tie
// and every string built from them stay the same (PLAN §3 I3).
#pragma once

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace r1 {

struct Box {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    void add(P2 p) { x0 = std::min(x0, p.x); y0 = std::min(y0, p.y); x1 = std::max(x1, p.x); y1 = std::max(y1, p.y); }
    void add(const Box& b) { x0 = std::min(x0, b.x0); y0 = std::min(y0, b.y0); x1 = std::max(x1, b.x1); y1 = std::max(y1, b.y1); }
    bool empty() const { return x0 > x1 || y0 > y1; }
    Box grown(double m) const { return {x0 - m, y0 - m, x1 + m, y1 + m}; }
    bool contains(P2 p) const { return x0 <= p.x && p.x <= x1 && y0 <= p.y && p.y <= y1; }
    bool overlaps(const Box& b) const { return x0 <= b.x1 && b.x0 <= x1 && y0 <= b.y1 && b.y0 <= y1; }
};
inline Box boxOf(const std::vector<P2>& points) {
    Box b;
    for (const P2& p : points) b.add(p);
    return b;
}

// Items bucketed on a uniform grid by their boxes. `near` returns every item
// whose box may meet the query's, ascending and once each.
class BoxIndex {
public:
    explicit BoxIndex(double cell) : cell_(cell) {}
    void add(size_t item, const Box& box) {
        all_.push_back(item);
        if (box.empty()) return;
        const long cx0 = cellOf(box.x0), cx1 = cellOf(box.x1), cy0 = cellOf(box.y0), cy1 = cellOf(box.y1);
        // A park or a coastline spanning the whole neighbourhood is always a candidate.
        if (cx1 - cx0 > kMaxSpan || cy1 - cy0 > kMaxSpan) { wide_.push_back(item); return; }
        for (long x = cx0; x <= cx1; ++x)
            for (long y = cy0; y <= cy1; ++y) cells_[key(x, y)].push_back(item);
    }
    std::vector<size_t> near(const Box& box) const {
        if (box.empty()) return {};
        const long cx0 = cellOf(box.x0), cx1 = cellOf(box.x1), cy0 = cellOf(box.y0), cy1 = cellOf(box.y1);
        if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) > kMaxSpan * kMaxSpan) {
            std::vector<size_t> out = all_;
            std::sort(out.begin(), out.end());
            return out;
        }
        std::vector<size_t> out = wide_;
        for (long x = cx0; x <= cx1; ++x)
            for (long y = cy0; y <= cy1; ++y) {
                const auto it = cells_.find(key(x, y));
                if (it != cells_.end()) out.insert(out.end(), it->second.begin(), it->second.end());
            }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

private:
    static constexpr long kMaxSpan = 64;
    static int64_t key(long x, long y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); }
    long cellOf(double v) const { return long(std::floor(v / cell_)); }
    double cell_;
    std::unordered_map<int64_t, std::vector<size_t>> cells_;
    std::vector<size_t> wide_, all_;
};

// Points in longitude and latitude, for "the first one nearest to here" under
// a metric of the form hypot(wrap(dx) * kx, dy * ky). The answer is the one a
// scan in order keeping strict improvements gives: the least distance, and of
// the points at that distance, the first.
class NearestPoint {
public:
    static constexpr size_t npos = std::numeric_limits<size_t>::max();
    NearestPoint(std::vector<P2> points, double cellDegrees) : points_(std::move(points)), cell_(cellDegrees) {
        if (points_.empty()) return;
        reference_ = points_.front();
        for (size_t i = 0; i < points_.size(); ++i) {
            const long cx = cellOf(wrap(points_[i].x - reference_.x)), cy = cellOf(points_[i].y - reference_.y);
            lo_x_ = std::min(lo_x_, cx); hi_x_ = std::max(hi_x_, cx);
            lo_y_ = std::min(lo_y_, cy); hi_y_ = std::max(hi_y_, cy);
            cells_[key(cx, cy)].push_back(i);
        }
    }
    const std::vector<P2>& points() const { return points_; }
    // `distance(q)` is the metric to `at`; `low` is a scale it never falls
    // below in degrees (the least of kx and ky). `best` is the scan's starting
    // value and comes back as the distance found; npos when none beats it.
    template <class Distance>
    size_t nearest(P2 at, double low, const Distance& distance, double& best) const {
        size_t found = npos;
        if (points_.empty()) return found;
        const long cx = cellOf(wrap(at.x - reference_.x)), cy = cellOf(at.y - reference_.y);
        const long rings = std::max({cx - lo_x_, hi_x_ - cx, cy - lo_y_, hi_y_ - cy, 0L});
        auto visit = [&](long x, long y) {
            const auto it = cells_.find(key(x, y));
            if (it == cells_.end()) return;
            for (size_t i : it->second) {
                const double d = distance(points_[i]);
                if (d < best || (found != npos && d == best && i < found)) { best = d; found = i; }
            }
        };
        for (long k = 0; k <= rings; ++k) {
            if (k == 0) visit(cx, cy);
            else {
                for (long x = cx - k; x <= cx + k; ++x) { visit(x, cy - k); visit(x, cy + k); }
                for (long y = cy - k + 1; y <= cy + k - 1; ++y) { visit(cx - k, y); visit(cx + k, y); }
            }
            // Every point not yet seen is at least k cells away on one axis.
            if (best < double(k) * cell_ * low * (1.0 - 1e-6)) break;
        }
        return found;
    }

private:
    static int64_t key(long x, long y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); }
    long cellOf(double v) const { return long(std::floor(v / cell_)); }
    std::vector<P2> points_;
    double cell_;
    P2 reference_;
    long lo_x_ = std::numeric_limits<long>::max(), hi_x_ = std::numeric_limits<long>::min();
    long lo_y_ = std::numeric_limits<long>::max(), hi_y_ = std::numeric_limits<long>::min();
    std::unordered_map<int64_t, std::vector<size_t>> cells_;
};

}  // namespace r1
