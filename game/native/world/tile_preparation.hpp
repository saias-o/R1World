#pragma once
#include "gen/cook.hpp"
#include "graphics/Mesh.hpp"
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace r1::world {
struct Footprint {
    std::vector<glm::dvec2> points;
    glm::dvec2 low{1e30},high{-1e30};
    double top=1e30;  // the highest point over it, in its tile's frame: what an aircraft clears
    int interior=-1;
};
// A cooked part as the GPU takes it, made on the worker that cooked it
// (WorldService's `prepare`), so the frame only uploads.
struct PartUpload {
    std::string name;
    std::vector<saida::Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t material=0;  // index into the cooked tile's parts
    std::shared_ptr<const saida::PreparedMesh> prepared;
};
// Everything the frame needs of a cooked tile, made on the worker that cooked
// it: the parts as the GPU takes them, and building metadata for inspection.
struct PreparedTile {
    std::vector<PartUpload> parts;
    std::vector<Footprint> footprints;
    struct Building {
        std::array<std::vector<PartUpload>,3> levels;
        std::vector<PartUpload> reducedNear;
    };
    std::vector<Building> buildings;
    std::vector<std::shared_ptr<const saida::MeshCollisionData>> buildingCollisions;
};

PartUpload uploadOf(const r1::MeshPart& part, size_t index);
void releaseCookedGeometry(r1::CookedTile& tile);
r1::MeshPart generatorPart(const std::string& name, const std::vector<saida::Vertex>& vertices,
                          const std::vector<uint32_t>& indices);
PreparedTile prepareTile(const r1::CookedTile& tile);

} // namespace r1::world
