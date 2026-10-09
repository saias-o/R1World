// Welded, flat-shaded triangle meshes, bucketed by material.
//
// The engine's arena holds a fixed million vertices for the whole scene
// (CLAUDE.md §5), so vertices are welded on (position, normal, uv): the two
// triangles of a quad share an edge and cost four vertices, not six, and no
// crease that should stay sharp is ever smoothed. The weld is deterministic:
// the first occurrence wins and the order follows insertion (PLAN §3 I3).
#pragma once

#include "common.hpp"

#include <cstring>
#include <optional>
#include <unordered_map>

namespace r1 {

struct UV { double u = 0, v = 0; };

// How a triangle given no UVs gets them: none, the ground's (x, -z) metres,
// metres across and up its own plane (Slope), or a facade sheet's bays and
// storeys across and up it (Facade, `facadeRepeat` metres a repeat).
enum class UvMode { None, Planar, Slope, Facade };

struct Mesh {
    std::vector<P3> positions, normals;
    std::vector<UV> texcoords;
    std::vector<uint32_t> indices;
    // Per vertex, multiplied into the material's colour; empty: white. Only
    // meshes built vertex by vertex carry it (the weld does not keep it).
    std::vector<std::array<double, 3>> colors;
    UvMode uvMode = UvMode::None;
    UV facadeRepeat{1.0, 1.0};

    explicit Mesh(UvMode mode = UvMode::None) : uvMode(mode) {}
    bool empty() const { return indices.empty(); }
    size_t vertexCount() const { return positions.size(); }

    // Construction-only weld keys are larger than the finished vertex data.
    // Release them once no triangles will be appended (e.g. stored LODs).
    // Positions, normals, UVs and indices remain available for rendering.
    void releaseWeld() { lookup_.clear(); lookup_.rehash(0); }

    uint32_t vertex(const P3& position, const P3& normal, UV uv);
    void addTriangle(P3 a, P3 b, P3 c, const UV* uvs = nullptr);
    // Turned so its normal points up (y > 0).
    void addUpTriangle(P3 a, P3 b, P3 c, const UV* uvs = nullptr);
    void addQuad(P3 a, P3 b, P3 c, P3 d, const UV* uvs = nullptr);
    void addUpQuad(P3 a, P3 b, P3 c, P3 d, const UV* uvs = nullptr);
    void addBox(P3 center, P3 size, double yaw = 0.0);

private:
    struct Key {
        double v[8];
        bool operator==(const Key& o) const {
            for (int i = 0; i < 8; ++i) if (v[i] != o.v[i]) return false;
            return true;
        }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            uint64_t h = 1469598103934665603ull;
            for (double d : k.v) {
                uint64_t bits; d += 0.0; std::memcpy(&bits, &d, 8);
                h = (h ^ bits) * 1099511628211ull;
                h ^= h >> 29;
            }
            return size_t(h);
        }
    };
    std::unordered_map<Key, uint32_t, KeyHash> lookup_;
    friend Mesh smoothSurface(const Mesh&);
};

P3 faceNormal(P3 a, P3 b, P3 c);

// Share a surface's vertices across triangles, averaging their normals:
// continuous street tops and terrain, never kerbs (streets.smooth_surface).
Mesh smoothSurface(const Mesh& mesh);

// What a part is drawn with. Texture paths are relative to the game root.
struct Material {
    std::string name;
    std::array<double, 4> color{1, 1, 1, 1};
    double roughness = 0.9, metallic = 0.0;
    bool doubleSided = false;
    std::string baseColorTexture, normalTexture, metallicRoughnessTexture;
    // Mesh UVs are metres, or bays and storeys on walls; this makes them repeats.
    double uvScale = 1.0;
    // The engine's surface variation (saida::SurfaceVariation), in repeats:
    // warp, macro cells per repeat, macro albedo, macro slope. Zero: none.
    std::array<double, 4> variation{};
    double normalStrength = 1.0;
    // Relief for parallax occlusion mapping: the height map, and how deep its
    // black lies below its white, in repeats. Empty: drawn flat.
    std::string heightTexture;
    double parallaxDepth = 0.0;
    // How much of the sky the material reflects (saida::MaterialDesc::
    // environmentReflection): 1 on facade sheets, whose glazing shows it.
    double environmentReflection = 0.0;

    bool operator==(const Material& other) const {
        return name == other.name && color == other.color &&
               roughness == other.roughness && metallic == other.metallic &&
               doubleSided == other.doubleSided &&
               baseColorTexture == other.baseColorTexture &&
               normalTexture == other.normalTexture &&
               metallicRoughnessTexture == other.metallicRoughnessTexture &&
               uvScale == other.uvScale && variation == other.variation &&
               normalStrength == other.normalStrength && heightTexture == other.heightTexture &&
               parallaxDepth == other.parallaxDepth && environmentReflection == other.environmentReflection;
    }
};

struct MeshPart {
    std::string name;
    Mesh mesh;
    Material material;
};

// Per-vertex tangents in the convention the engine reads (dP/du, w signs the
// bitangent), for parts whose material carries a normal map.
std::vector<std::array<double, 4>> tangents(const Mesh& mesh, double uvScale);

// A deterministic GLB of the parts (mesh.write_glb). Only the headless tools
// write one now; the game uploads the parts directly.
std::vector<uint8_t> writeGlb(const std::vector<MeshPart>& parts);

}  // namespace r1
