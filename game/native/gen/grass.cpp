#include "grass.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace r1 {

namespace {

// Of the ground drawn as a family, the share grown over by tufts: lawns and
// meadows close; fields of crops and stubble nearly so, in the field's own
// straw colour; marsh reeds, dry grass and savanna thinner; the floor of a
// wood sparse.
const std::map<std::string, double> kGrassy = {
    {"grass", 1.0}, {"grass_lush", 1.0}, {"farmland", 0.85}, {"grass_dry", 0.75}, {"mud", 0.6},
    {"savanna", 0.6}, {"tropical_ground", 0.45}, {"frost", 0.35}, {"forest_tropical", 0.3},
    {"forest_temperate", 0.25}, {"forest_boreal", 0.2},
};

uint8_t srgbByte(double linear) {
    const double c = std::clamp(linear, 0.0, 1.0);
    const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return uint8_t(std::lround(s * 255.0));
}

// Texel space: x along u, y along v, texel (i, j)'s centre at (i, j).
struct ToTexels {
    std::array<double, 6> uv;
    int size;
    P2 at(double x, double z) const {
        return {(uv[0] * x + uv[1] * z + uv[2]) * size - 0.5, (uv[3] * x + uv[4] * z + uv[5]) * size - 0.5};
    }
};

// Even-odd fill of closed rings: the texels whose centre is inside, set.
void fill(std::vector<uint8_t>& mask, int size, const std::vector<std::vector<P2>>& rings) {
    double top = 1e300, bottom = -1e300;
    for (const auto& r : rings)
        for (const P2& p : r) { top = std::min(top, p.y); bottom = std::max(bottom, p.y); }
    const int j0 = std::max(0, int(std::ceil(top))), j1 = std::min(size - 1, int(std::floor(bottom)));
    std::vector<double> crossings;
    for (int j = j0; j <= j1; ++j) {
        crossings.clear();
        for (const auto& r : rings)
            for (size_t k = 0; k < r.size(); ++k) {
                const P2 a = r[k], b = r[(k + 1) % r.size()];
                if ((a.y <= j) == (b.y <= j)) continue;
                crossings.push_back(a.x + (j - a.y) / (b.y - a.y) * (b.x - a.x));
            }
        std::sort(crossings.begin(), crossings.end());
        for (size_t k = 0; k + 1 < crossings.size(); k += 2) {
            const int i0 = std::max(0, int(std::ceil(crossings[k])));
            const int i1 = std::min(size - 1, int(std::floor(crossings[k + 1])));
            for (int i = i0; i <= i1; ++i) mask[size_t(j) * size + i] = 1;
        }
    }
}

}  // namespace

double grassDensity(const std::string& family) {
    auto it = kGrassy.find(family);
    return it == kGrassy.end() ? 0.0 : it->second;
}

GrassCover grassCover(const TerrainGrid& grid,
                      const std::function<std::optional<std::string>(const std::string&)>& familyOf,
                      const std::function<std::array<double, 3>(const std::string&)>& colourOf,
                      const std::vector<const MeshPart*>& laid, const std::vector<Ring>& footprints) {
    GrassCover out;
    const int n = kTerrainMeshSize;
    if (grid.points.size() != size_t(n) * n || grid.classes.size() != size_t(n - 1) * (n - 1) * 2) return out;

    // The grid is nearly a parallelogram of the engine plane -- its north
    // edge is shorter by the meridians' convergence, 5 cm in 500 m at 47 N --
    // so an affine map from three corners places a blade within centimetres
    // of its triangle, and on its height to well under one.
    const P3 o = grid.points[0], du = grid.points[size_t(n - 1)], dv = grid.points[size_t(n - 1) * n];
    const double ax = du.x - o.x, az = du.z - o.z, bx = dv.x - o.x, bz = dv.z - o.z;
    const double det = ax * bz - az * bx;
    if (std::abs(det) < 1e-9) return out;
    const double ua = bz / det, ub = -bx / det, va = -az / det, vb = ax / det;
    out.uvFromEngine = {ua, ub, -(ua * o.x + ub * o.z), va, vb, -(va * o.x + vb * o.z)};

    // Grass where the drawn ground is grassy, by the triangle under each texel.
    struct Kind { double density; std::array<uint8_t, 3> colour; };
    std::map<std::string, Kind> kinds;
    auto kindOf = [&](const std::string& cls) -> const Kind& {
        auto it = kinds.find(cls);
        if (it != kinds.end()) return it->second;
        const auto family = familyOf(cls);
        const double density = family ? grassDensity(*family) : 0.0;
        const auto c = density > 0.0 ? colourOf(cls) : std::array<double, 3>{};
        return kinds.emplace(cls, Kind{density, {srgbByte(c[0]), srgbByte(c[1]), srgbByte(c[2])}}).first->second;
    };
    const int size = kGrassCoverSize;
    out.cover.assign(size_t(size) * size, 0u);
    bool any = false;
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) {
            const double u = (i + 0.5) / size * (n - 1), v = (j + 0.5) / size * (n - 1);
            const int col = std::min(n - 2, int(u)), row = std::min(n - 2, int(v));
            const bool lower = u - col >= v - row;
            const Kind& k = kindOf(grid.classes[size_t(2 * (row * (n - 1) + col) + (lower ? 0 : 1))]);
            if (k.density <= 0.0) continue;
            const uint32_t a = uint32_t(std::lround(k.density * 255.0));
            out.cover[size_t(j) * size + i] =
                k.colour[0] | uint32_t(k.colour[1]) << 8 | uint32_t(k.colour[2]) << 16 | a << 24;
            any = true;
        }
    if (!any) return GrassCover{};

    // Nothing grows under what is laid on the ground.
    const ToTexels texels{out.uvFromEngine, size};
    auto groundAt = [&](double x, double z) {
        const double u = std::clamp(ua * x + ub * z + out.uvFromEngine[2], 0.0, 1.0) * (n - 1);
        const double v = std::clamp(va * x + vb * z + out.uvFromEngine[5], 0.0, 1.0) * (n - 1);
        const int col = std::min(n - 2, int(u)), row = std::min(n - 2, int(v));
        const double fu = u - col, fv = v - row;
        auto y = [&](int r, int c) { return grid.points[size_t(r) * n + c].y; };
        return fu >= fv ? y(row, col) * (1 - fu) + y(row, col + 1) * (fu - fv) + y(row + 1, col + 1) * fv
                        : y(row, col) * (1 - fv) + y(row + 1, col + 1) * fu + y(row + 1, col) * (fv - fu);
    };
    std::vector<uint8_t> covered(size_t(size) * size, 0);
    for (const MeshPart* part : laid) {
        const Mesh& m = part->mesh;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            const P3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]];
            const P3 c = m.positions[m.indices[t + 2]];
            // A face seen from above: its plan area is not nil.
            const double plan = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
            if (std::abs(plan) < 1e-6) continue;
            const double cx = (a.x + b.x + c.x) / 3.0, cz = (a.z + b.z + c.z) / 3.0;
            if ((a.y + b.y + c.y) / 3.0 - groundAt(cx, cz) > kLaidHeight) continue;
            fill(covered, size, {{texels.at(a.x, a.z), texels.at(b.x, b.z), texels.at(c.x, c.z)}});
        }
    }
    for (const Ring& r : footprints) {
        std::vector<P2> ring;
        for (const P2& p : r) ring.push_back(texels.at(p.x, p.y));
        fill(covered, size, {ring});
    }
    for (size_t k = 0; k < covered.size(); ++k)
        if (covered[k]) out.cover[k] = 0u;

    out.groundSamples = n;
    out.coverSize = size;
    out.heights.reserve(grid.points.size());
    for (const P3& p : grid.points) out.heights.push_back(float(p.y));
    return out;
}

}  // namespace r1
