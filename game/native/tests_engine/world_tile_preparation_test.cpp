// Pure CPU mesh preparation: no Engine, scene, device or window is created.
#include "world/tile_preparation.hpp"
#include <cassert>
#include <iostream>

namespace w = r1::world;

int main() {
    r1::MeshPart part;
    part.name = "ground";
    part.material.uvScale = 2.;
    part.mesh.uvMode = r1::UvMode::Planar;
    part.mesh.positions = {{0, 0, 0}, {0, 0, 1}, {1, 0, 0}};
    part.mesh.normals = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
    part.mesh.texcoords = {{0, 0}, {0, -1}, {1, 0}};
    part.mesh.indices = {0, 1, 2};
    auto upload = w::uploadOf(part, 7);
    assert(upload.name == part.name && upload.material == 7);
    assert(upload.indices == part.mesh.indices && upload.vertices.size() == 3);
    assert(upload.vertices[0].color == glm::vec3(1.f));
    assert(upload.vertices[1].texCoord == glm::vec2(0.f, -2.f));
    assert(upload.vertices[2].texCoord == glm::vec2(2.f, 0.f));
    assert(upload.vertices[0].normal == glm::vec3(0.f, 1.f, 0.f));
    assert(std::abs(glm::length(glm::vec3(upload.vertices[0].tangent)) - 1.f) < 1e-6f);
    part.mesh.colors = {{.1, .2, .3}, {.2, .3, .4}, {.3, .4, .5}};
    assert(w::uploadOf(part, 0).vertices[0].color == glm::vec3(.1f, .2f, .3f));

    r1::CookedTile cooked;
    cooked.parts = {part};
    cooked.buildingCollisions = {part};
    r1::BuildingVisual building;
    building.id = 42;
    for (auto& level : building.levels) level = {part};
    building.reducedNear = {part};
    cooked.buildings = {building};
    cooked.manifest = {{"footprints", {{{0., 0.}, {1., 0.}, {0., 1.}}}}, {"footprintTops", {12.}}};
    const auto manifest = cooked.manifest;
    auto prepared = w::prepareTile(cooked);
    assert(prepared.parts.size() == 1 && prepared.buildings.size() == 1);
    assert(prepared.parts[0].prepared->geometry.vertices.size() == 3);
    assert(prepared.parts[0].prepared->collision->indices == part.mesh.indices);
    assert(prepared.buildingCollisions[0]->positions[2] == glm::vec3(1.f, 0.f, 0.f));
    assert(prepared.footprints[0].top == 12.);
    assert(prepared.footprints[0].low == glm::dvec2(0.));
    assert(prepared.footprints[0].high == glm::dvec2(1.));
    for (const auto& level : prepared.buildings[0].levels)
        assert(level[0].prepared->geometry.indices == part.mesh.indices);
    assert(prepared.buildings[0].reducedNear[0].prepared->geometry.indices == part.mesh.indices);

    cooked.manifest.erase("footprintTops");
    assert(w::prepareTile(cooked).footprints[0].top == 1e30);
    cooked.manifest = manifest;

    w::releaseCookedGeometry(cooked);
    assert(cooked.parts[0].mesh.empty() && cooked.buildingCollisions[0].mesh.empty());
    for (const auto& level : cooked.buildings[0].levels) assert(level[0].mesh.empty());
    assert(cooked.buildings[0].reducedNear[0].mesh.empty());
    assert(cooked.parts[0].mesh.uvMode == r1::UvMode::Planar);
    assert(cooked.manifest == manifest && cooked.buildings[0].id == 42);
    assert(cooked.parts[0].name == part.name && cooked.parts[0].material == part.material);
    // Prepared data must outlive the generator's released geometry.
    const auto& geometry = prepared.parts[0].prepared->geometry;
    assert(geometry.vertices[2].pos == glm::vec3(1.f, 0.f, 0.f));
    const auto diagnostic = w::generatorPart("diagnostic", geometry.vertices, geometry.indices);
    assert(diagnostic.name == "diagnostic" && diagnostic.mesh.indices == part.mesh.indices);
    assert(diagnostic.mesh.positions == part.mesh.positions);
    assert(diagnostic.mesh.texcoords[2].u == 2.);
    std::cout << "PASS tile preparation: vertices, LODs, collision, metadata and released source lifetime\n";
}
