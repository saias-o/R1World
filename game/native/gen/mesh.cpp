#include "mesh.hpp"

#include <nlohmann/json.hpp>

namespace r1 {

P3 faceNormal(P3 a, P3 b, P3 c) {
    const P3 u{b.x - a.x, b.y - a.y, b.z - a.z}, v{c.x - a.x, c.y - a.y, c.z - a.z};
    const P3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    const double length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (length < 1e-12) return {0.0, 1.0, 0.0};
    return {n.x / length, n.y / length, n.z / length};
}

namespace {
// "planar" projects on the ground plane, (x, -z): streets, where neighbouring
// triangles on different slopes must continue one pattern. "slope" lays each
// face out in its own frame, across the fall line and up it: roofs.
void derivedUvs(UvMode mode, P3 n, const P3 p[3], UV out[3], UV repeat) {
    if (mode == UvMode::Planar || std::abs(n.y) > 0.999) {
        const UV r = mode == UvMode::Facade ? repeat : UV{1.0, 1.0};
        for (int i = 0; i < 3; ++i) out[i] = {p[i].x / r.u, -p[i].z / r.v};
        return;
    }
    // From the normal as the weld key rounds it, so coplanar triangles whose
    // normals differ by roundoff get the same frame.
    n = {pyround(n.x, 6), pyround(n.y, 6), pyround(n.z, 6)};
    double tx = n.z, tz = -n.x;
    const double length = std::hypot(tx, tz);
    tx /= length; tz /= length;
    const double bx = n.y * tz, by = n.z * tx - n.x * tz, bz = -n.y * tx;
    if (mode == UvMode::Facade) {
        // A sheet runs down its v as the wall goes up, like `face` lays it.
        for (int i = 0; i < 3; ++i)
            out[i] = {pyround((p[i].x * tx + p[i].z * tz) / repeat.u, 5), pyround(-p[i].y / repeat.v, 5)};
        return;
    }
    for (int i = 0; i < 3; ++i)
        out[i] = {pyround(p[i].x * tx + p[i].z * tz, 5), pyround(p[i].x * bx + p[i].y * by + p[i].z * bz, 5)};
}
}  // namespace

uint32_t Mesh::vertex(const P3& position, const P3& normal, UV uv) {
    // The key rounds the normal (sub-microradian) so coplanar triangles that
    // differ by roundoff share their corners; the stored normal is exact.
    Key key{{position.x, position.y, position.z, pyround(normal.x, 6), pyround(normal.y, 6),
             pyround(normal.z, 6), uv.u, uv.v}};
    auto [it, inserted] = lookup_.emplace(key, uint32_t(positions.size()));
    if (inserted) {
        positions.push_back(position);
        normals.push_back(normal);
        texcoords.push_back(uv);
    }
    return it->second;
}

void Mesh::addTriangle(P3 a, P3 b, P3 c, const UV* uvs) {
    const P3 n = faceNormal(a, b, c);
    UV derived[3];
    const UV zero[3] = {};
    if (!uvs && uvMode != UvMode::None) {
        const P3 p[3] = {a, b, c};
        derivedUvs(uvMode, n, p, derived, facadeRepeat);
        uvs = derived;
    }
    if (!uvs) uvs = zero;
    indices.push_back(vertex(a, n, uvs[0]));
    indices.push_back(vertex(b, n, uvs[1]));
    indices.push_back(vertex(c, n, uvs[2]));
}

void Mesh::addUpTriangle(P3 a, P3 b, P3 c, const UV* uvs) {
    if (faceNormal(a, b, c).y < 0.0) {
        std::swap(b, c);
        if (uvs) {
            const UV swapped[3] = {uvs[0], uvs[2], uvs[1]};
            addTriangle(a, b, c, swapped);
            return;
        }
    }
    addTriangle(a, b, c, uvs);
}

void Mesh::addQuad(P3 a, P3 b, P3 c, P3 d, const UV* uvs) {
    if (!uvs) {
        addTriangle(a, b, c);
        addTriangle(a, c, d);
        return;
    }
    const UV first[3] = {uvs[0], uvs[1], uvs[2]}, second[3] = {uvs[0], uvs[2], uvs[3]};
    addTriangle(a, b, c, first);
    addTriangle(a, c, d, second);
}

void Mesh::addUpQuad(P3 a, P3 b, P3 c, P3 d, const UV* uvs) {
    if (faceNormal(a, b, c).y < 0.0) {
        std::swap(b, d);
        if (uvs) {
            const UV swapped[4] = {uvs[0], uvs[3], uvs[2], uvs[1]};
            addQuad(a, b, c, d, swapped);
            return;
        }
    }
    addQuad(a, b, c, d, uvs);
}

void Mesh::addBox(P3 center, P3 size, double yaw) {
    const double hx = size.x * 0.5, hy = size.y * 0.5, hz = size.z * 0.5;
    const double co = std::cos(yaw), si = std::sin(yaw);
    auto point = [&](double x, double y, double z) {
        return P3{center.x + x * co + z * si, center.y + y, center.z - x * si + z * co};
    };
    const P3 p[8] = {point(-hx, -hy, -hz), point(hx, -hy, -hz), point(hx, -hy, hz), point(-hx, -hy, hz),
                     point(-hx, hy, -hz),  point(hx, hy, -hz),  point(hx, hy, hz),  point(-hx, hy, hz)};
    addQuad(p[0], p[1], p[2], p[3]);
    addQuad(p[4], p[7], p[6], p[5]);
    addQuad(p[0], p[4], p[5], p[1]);
    addQuad(p[1], p[5], p[6], p[2]);
    addQuad(p[2], p[6], p[7], p[3]);
    addQuad(p[3], p[7], p[4], p[0]);
}

Mesh smoothSurface(const Mesh& mesh) {
    struct H {
        size_t operator()(const P3& p) const {
            uint64_t h = 1469598103934665603ull;
            for (double d : {p.x + 0.0, p.y + 0.0, p.z + 0.0}) {
                uint64_t bits; std::memcpy(&bits, &d, 8);
                h = (h ^ bits) * 1099511628211ull;
            }
            return size_t(h);
        }
    };
    std::unordered_map<P3, P3, H> sums;
    sums.reserve(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i) {
        P3& total = sums[mesh.positions[i]];
        total.x += mesh.normals[i].x; total.y += mesh.normals[i].y; total.z += mesh.normals[i].z;
    }
    for (auto& [position, total] : sums) {
        double size = std::sqrt(total.x * total.x + total.y * total.y + total.z * total.z);
        if (size == 0.0) size = 1.0;
        total = {total.x / size, total.y / size, total.z / size};
    }
    Mesh out(mesh.uvMode);
    std::vector<uint32_t> remap(mesh.positions.size());
    for (size_t i = 0; i < mesh.positions.size(); ++i)
        remap[i] = out.vertex(mesh.positions[i], sums[mesh.positions[i]], mesh.texcoords[i]);
    out.indices.reserve(mesh.indices.size());
    for (uint32_t i : mesh.indices) out.indices.push_back(remap[i]);
    return out;
}

std::vector<std::array<double, 4>> tangents(const Mesh& mesh, double k) {
    const size_t count = mesh.positions.size();
    std::vector<P3> tan(count), bit(count);
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        const P3 p0 = mesh.positions[i0], p1 = mesh.positions[i1], p2 = mesh.positions[i2];
        const UV t0 = mesh.texcoords[i0], t1 = mesh.texcoords[i1], t2 = mesh.texcoords[i2];
        const double u0 = t0.u * k, v0 = t0.v * k, u1 = t1.u * k, v1 = t1.v * k, u2 = t2.u * k, v2 = t2.v * k;
        const P3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z}, e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        const double du1 = u1 - u0, dv1 = v1 - v0, du2 = u2 - u0, dv2 = v2 - v0;
        const double det = du1 * dv2 - du2 * dv1;
        if (std::abs(det) < 1e-12) continue;
        const double r = 1.0 / det;
        const P3 t{(e1.x * dv2 - e2.x * dv1) * r, (e1.y * dv2 - e2.y * dv1) * r, (e1.z * dv2 - e2.z * dv1) * r};
        const P3 b{(e2.x * du1 - e1.x * du2) * r, (e2.y * du1 - e1.y * du2) * r, (e2.z * du1 - e1.z * du2) * r};
        for (uint32_t j : {i0, i1, i2}) {
            tan[j].x += t.x; tan[j].y += t.y; tan[j].z += t.z;
            bit[j].x += b.x; bit[j].y += b.y; bit[j].z += b.z;
        }
    }
    std::vector<std::array<double, 4>> out(count);
    for (size_t j = 0; j < count; ++j) {
        const P3 n = mesh.normals[j];
        P3 t = tan[j];
        const double d = n.x * t.x + n.y * t.y + n.z * t.z;
        t = {t.x - n.x * d, t.y - n.y * d, t.z - n.z * d};
        double length = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
        if (length < 1e-9) {
            t = std::abs(n.z) < 0.9 ? P3{n.y, -n.x, 0.0} : P3{0.0, n.z, -n.y};
            length = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
            if (length == 0.0) length = 1.0;
        }
        t = {t.x / length, t.y / length, t.z / length};
        const P3 c{n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x};
        const P3 b = bit[j];
        const double w = (c.x * b.x + c.y * b.y + c.z * b.z) > 0.0 ? -1.0 : 1.0;
        out[j] = {t.x, t.y, t.z, w};
    }
    return out;
}

namespace {
void pad4(std::vector<uint8_t>& data, uint8_t byte) { while (data.size() % 4) data.push_back(byte); }
template <class T> void put(std::vector<uint8_t>& out, T value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}
}  // namespace

std::vector<uint8_t> writeGlb(const std::vector<MeshPart>& parts) {
    using nlohmann::json;
    std::vector<uint8_t> binary;
    json views = json::array(), accessors = json::array(), meshes = json::array(), nodes = json::array(),
         materials = json::array(), images = json::array(), textures = json::array();
    std::map<std::string, int> textureIndex;
    auto texture = [&](const std::string& path) {
        auto it = textureIndex.find(path);
        if (it != textureIndex.end()) return it->second;
        // Relative to cache/world/<key>/, where a tile's GLB used to live.
        images.push_back({{"uri", "../../../" + path}});
        textures.push_back({{"source", int(images.size()) - 1}});
        return textureIndex[path] = int(textures.size()) - 1;
    };
    auto view = [&](const std::vector<uint8_t>& payload, int target) {
        pad4(binary, 0);
        views.push_back({{"buffer", 0}, {"byteOffset", binary.size()}, {"byteLength", payload.size()}, {"target", target}});
        binary.insert(binary.end(), payload.begin(), payload.end());
        return int(views.size()) - 1;
    };
    auto accessor = [&](int v, int component, size_t count, const char* type) {
        accessors.push_back({{"bufferView", v}, {"componentType", component}, {"count", count}, {"type", type}});
        return int(accessors.size()) - 1;
    };
    int meshIndex = 0;
    for (const MeshPart& part : parts) {
        if (part.mesh.empty()) continue;
        const Mesh& m = part.mesh;
        const double k = part.material.uvScale;
        std::vector<uint8_t> pos, nrm, uv, idx;
        P3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
        for (size_t i = 0; i < m.positions.size(); ++i) {
            const P3 p = m.positions[i], n = m.normals[i];
            put(pos, float(p.x)); put(pos, float(p.y)); put(pos, float(p.z));
            put(nrm, float(n.x)); put(nrm, float(n.y)); put(nrm, float(n.z));
            put(uv, float(m.texcoords[i].u * k)); put(uv, float(m.texcoords[i].v * k));
            lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
            hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
        }
        for (uint32_t i : m.indices) put(idx, i);
        json attributes;
        attributes["POSITION"] = accessor(view(pos, 34962), 5126, m.positions.size(), "VEC3");
        accessors.back()["min"] = {float(lo.x), float(lo.y), float(lo.z)};
        accessors.back()["max"] = {float(hi.x), float(hi.y), float(hi.z)};
        attributes["NORMAL"] = accessor(view(nrm, 34962), 5126, m.positions.size(), "VEC3");
        attributes["TEXCOORD_0"] = accessor(view(uv, 34962), 5126, m.positions.size(), "VEC2");
        const int indexAccessor = accessor(view(idx, 34963), 5125, m.indices.size(), "SCALAR");
        if (!part.material.normalTexture.empty()) {
            std::vector<uint8_t> tan;
            for (const auto& t : tangents(m, k)) for (double c : t) put(tan, float(c));
            attributes["TANGENT"] = accessor(view(tan, 34962), 5126, m.positions.size(), "VEC4");
        }
        const Material& mat = part.material;
        json pbr = {{"baseColorFactor", mat.color}, {"metallicFactor", mat.metallic}, {"roughnessFactor", mat.roughness}};
        if (!mat.baseColorTexture.empty()) pbr["baseColorTexture"] = {{"index", texture(mat.baseColorTexture)}};
        if (!mat.metallicRoughnessTexture.empty())
            pbr["metallicRoughnessTexture"] = {{"index", texture(mat.metallicRoughnessTexture)}};
        json material = {{"name", mat.name}, {"doubleSided", mat.doubleSided}, {"pbrMetallicRoughness", pbr}};
        if (!mat.normalTexture.empty()) material["normalTexture"] = {{"index", texture(mat.normalTexture)}};
        materials.push_back(material);
        meshes.push_back({{"name", part.name},
                          {"primitives", {{{"attributes", attributes}, {"indices", indexAccessor},
                                           {"material", int(materials.size()) - 1}, {"mode", 4}}}}});
        nodes.push_back({{"name", part.name}, {"mesh", meshIndex++}});
    }
    json scene = json::array();
    for (int i = 0; i < meshIndex; ++i) scene.push_back(i);
    json doc = {{"asset", {{"version", "2.0"}, {"generator", "R1World generator v2 (C++)"}}},
                {"scene", 0}, {"scenes", {{{"nodes", scene}}}}, {"nodes", nodes}, {"meshes", meshes},
                {"materials", materials}, {"accessors", accessors}, {"bufferViews", views},
                {"buffers", {{{"byteLength", binary.size()}}}}};
    if (!textures.empty()) { doc["images"] = images; doc["textures"] = textures; }
    std::string text = doc.dump();
    std::vector<uint8_t> jsonChunk(text.begin(), text.end());
    pad4(jsonChunk, ' ');
    pad4(binary, 0);
    std::vector<uint8_t> glb;
    put(glb, uint32_t(0x46546C67)); put(glb, uint32_t(2));
    put(glb, uint32_t(12 + 8 + jsonChunk.size() + 8 + binary.size()));
    put(glb, uint32_t(jsonChunk.size())); put(glb, uint32_t(0x4E4F534A));
    glb.insert(glb.end(), jsonChunk.begin(), jsonChunk.end());
    put(glb, uint32_t(binary.size())); put(glb, uint32_t(0x004E4942));
    glb.insert(glb.end(), binary.begin(), binary.end());
    return glb;
}

}  // namespace r1
