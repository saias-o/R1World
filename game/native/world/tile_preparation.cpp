#include "tile_preparation.hpp"
#include <nlohmann/json.hpp>

namespace r1::world {
using json = nlohmann::json;
// A generator part in the engine's vertex format.
PartUpload uploadOf(const r1::MeshPart& part,size_t index) {
    const r1::Mesh& m=part.mesh;
    const double k=part.material.uvScale;
    const auto tangents=r1::tangents(m,k);
    PartUpload up{part.name,{},m.indices,index};
    up.vertices.resize(m.positions.size());
    for(size_t v=0;v<m.positions.size();++v) {
        saida::Vertex& x=up.vertices[v];
        x.pos=glm::vec3(m.positions[v].x,m.positions[v].y,m.positions[v].z);
        x.normal=glm::vec3(m.normals[v].x,m.normals[v].y,m.normals[v].z);
        x.color=m.colors.empty()?glm::vec3(1.f):glm::vec3(m.colors[v][0],m.colors[v][1],m.colors[v][2]);
        x.texCoord=glm::vec2(m.texcoords[v].u*k,m.texcoords[v].v*k);
        x.tangent=glm::vec4(tangents[v][0],tangents[v][1],tangents[v][2],tangents[v][3]);
    }
    return up;
}

// Once a tile is prepared, the generator's own copy of its geometry -- double
// precision, with the weld keys of the ground and the streets -- is read for
// nothing: parts keep their names and materials, buildings their ids and
// bounds, and everything drawn or collided with is the prepared copy. It was
// some 68 of a Paris tile's 140 MB, held as long as the service kept the tile
// (measured 16 October 2026: 9 tiles mounted, 2.9 GB of heap).
void releaseCookedGeometry(r1::CookedTile& tile) {
    auto release=[](std::vector<r1::MeshPart>& parts){for(auto& part:parts)part.mesh=r1::Mesh(part.mesh.uvMode);};
    release(tile.parts);release(tile.buildingCollisions);
    for(auto& building:tile.buildings){for(auto& level:building.levels)release(level);release(building.reducedNear);}
}
// A prepared part back in the generator's form, for a GLB written when a
// smoke fails: the generator's own copy is gone by then.
r1::MeshPart generatorPart(const std::string& name,const std::vector<saida::Vertex>& vertices,const std::vector<uint32_t>& indices) {
    r1::MeshPart part;part.name=name;
    for(const auto& v:vertices) {
        part.mesh.positions.push_back({v.pos.x,v.pos.y,v.pos.z});
        part.mesh.normals.push_back({v.normal.x,v.normal.y,v.normal.z});
        part.mesh.texcoords.push_back({v.texCoord.x,v.texCoord.y});
    }
    part.mesh.indices=indices;
    return part;
}

PreparedTile prepareTile(const r1::CookedTile& tile) {
    PreparedTile prepared;
    auto& out=prepared.parts;
    out.reserve(tile.parts.size());
    for(size_t i=0;i<tile.parts.size();++i) {
        auto part=uploadOf(tile.parts[i],i);
        part.prepared=saida::prepareMesh({std::move(part.vertices),std::move(part.indices)});
        out.push_back(std::move(part));
    }
    auto prepareParts=[](const std::vector<r1::MeshPart>& parts) {
        std::vector<PartUpload> result;
        for(size_t i=0;i<parts.size();++i) {
            auto part=uploadOf(parts[i],i);
            part.prepared=saida::prepareMesh({std::move(part.vertices),std::move(part.indices)});
            result.push_back(std::move(part));
        }
        return result;
    };
    for(const auto& building:tile.buildings) {
        PreparedTile::Building b;
        for(size_t level=0;level<3;++level)b.levels[level]=prepareParts(building.levels[level]);
        b.reducedNear=prepareParts(building.reducedNear);
        prepared.buildings.push_back(std::move(b));
    }
    for(const auto& part:tile.buildingCollisions) {
        auto data=std::make_shared<saida::MeshCollisionData>();
        data->indices=part.mesh.indices;
        for(const auto& p:part.mesh.positions)data->positions.push_back({float(p.x),float(p.y),float(p.z)});
        prepared.buildingCollisions.push_back(std::move(data));
    }
    const json& tops=tile.manifest.contains("footprintTops")?tile.manifest.at("footprintTops"):json::array();
    for(const auto& polygon:tile.manifest.at("footprints")) {
        Footprint shape;
        if(prepared.footprints.size()<tops.size())shape.top=tops[prepared.footprints.size()].get<double>();
        for(const auto& point:polygon) {
            const glm::dvec2 p{point[0].get<double>(),point[1].get<double>()};
            shape.points.push_back(p);shape.low=glm::min(shape.low,p);shape.high=glm::max(shape.high,p);
        }
        prepared.footprints.push_back(std::move(shape));
    }
    return prepared;
}

} // namespace r1::world
