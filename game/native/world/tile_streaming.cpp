#include "session.hpp"

namespace r1::world {
saida::AssetID World::Impl::texture(const std::string& path,bool srgb) {
    if(path.empty())return saida::kAssetInvalid;
    auto [it,inserted]=textures.try_emplace(path+(srgb?"#srgb":"#linear"),saida::kAssetInvalid);
    if(inserted)it->second=engine.resources().getOrRegister((game/path).string(),saida::AssetType::Texture,srgb);
    return it->second;
}

saida::Material* World::Impl::material(const r1::Material& m) {
    saida::MaterialDesc d;
    d.baseColor=glm::vec4(m.color[0],m.color[1],m.color[2],m.color[3]);
    d.metallic=float(m.metallic);d.roughness=float(m.roughness);d.doubleSided=m.doubleSided;
    d.variation={float(m.variation[0]),float(m.variation[1]),float(m.variation[2]),float(m.variation[3])};
    d.normalStrength=float(m.normalStrength);
    d.heightId=texture(m.heightTexture,false);d.parallaxDepth=float(m.parallaxDepth);
    d.environmentReflection=float(m.environmentReflection);
    // Only outdoor hard surfaces collect a film of water. Interior floors
    // and facade concrete remain independent even when they share textures.
    if(m.name=="Carriageway"||m.name=="Cobbled carriageway"||m.name=="Sidewalks"||
       m.name=="Kerbs"||m.name=="Surveyed zebra crossings"||m.name=="Bridge concrete"||
       m.name=="Made ground"||m.name=="Parking asphalt"||m.name=="Parking markings")d.rain.reception=1.f;
    d.albedoId=texture(m.baseColorTexture,true);
    d.normalId=texture(m.normalTexture,false);
    d.metallicRoughnessId=texture(m.metallicRoughnessTexture,false);
    return engine.resources().getMaterial(d);
}

std::unique_ptr<saida::Node> World::Impl::buildTile(const std::string& key,const r1::ServedTile& served) {
    auto root=std::make_unique<saida::Node>(key);
    const r1::CookedTile& cooked=served.cooked;
    if(!cooked.ocean.is_null()) {
        // Nothing is drawn under the open ocean: water that lets the
        // background through would show the clear colour, not a sea bed.
        auto ocean=cooked.ocean;ocean["transparency"]=0.0;
        auto sea=saida::SceneSerializer::nodeFromJson(ocean.dump(),engine.resources());
        if(!sea)throw std::runtime_error("sea node refused by the scene loader");
        sea->setEnabled(debugWater);
        root->addChild(std::move(sea));
    } else root->createChild<saida::Node>("Geography");
    if(!cooked.grass.empty()) {
        auto grass=grassNode(cooked);grass->setEnabled(debugGrass);root->addChild(std::move(grass));
    }
    return root;
}

void World::Impl::bendGrass() {
    SAIDA_PROFILE_SCOPE("World/Grass");
    if(!debugGrass)return;
    const auto at=[](saida::Node* n){return glm::vec3(n->worldTransform()[3]);};
    const bool walking=player&&player->isActiveInHierarchy();
    const bool driving=car&&car->isActiveInHierarchy();
    for(auto& [key,t]:loaded) {
        if(!t.grass)continue;
        const glm::mat4 toLocal=glm::inverse(t.grass->worldTransform());
        auto bender=[&](saida::Node* n,float radius){
            return glm::vec4(glm::vec3(toLocal*glm::vec4(at(n),1.f)),radius);};
        t.grass->benders[0]=walking?bender(player,kWalkerBend):glm::vec4(0.f);
        t.grass->benders[1]=driving?bender(car,kCarBend):glm::vec4(0.f);
    }
}

std::unique_ptr<saida::GrassNode> World::Impl::grassNode(const r1::CookedTile& cooked) {
    const r1::GrassCover& g=cooked.grass;
    auto node=std::make_unique<saida::GrassNode>();
    saida::GrassNode::Field f;
    f.groundSamples=g.groundSamples;f.heights=g.heights;
    for(const auto& p:g.southBoundary)f.southBoundary.push_back({p[0],p[1]});
    for(const auto& p:g.northBoundary)f.northBoundary.push_back({p[0],p[1]});
    const auto& m=g.uvFromEngine;
    f.uvFromLocal=glm::mat3(glm::vec3(float(m[0]),float(m[3]),0.f),glm::vec3(float(m[1]),float(m[4]),0.f),
                            glm::vec3(float(m[2]),float(m[5]),1.f));
    f.coverSize=g.coverSize;f.cover=g.cover;
    // The ground's texture coordinates are (x, -z) in repeats (gen/terrain):
    // the blades carry its light and dark patches past where they end.
    const float repeat=float(cooked.grassUvScale);
    f.materialUvFromLocal=glm::mat3(glm::vec3(repeat,0.f,0.f),glm::vec3(0.f,-repeat,0.f),glm::vec3(0.f,0.f,1.f));
    const auto& v=cooked.grassVariation;
    f.variation=glm::vec4(float(v[0]),float(v[1]),float(v[2]),float(v[3]));
    if(!node->setField(std::move(f)))throw std::runtime_error("grass field refused by the engine");
    return node;
}

void World::Impl::failTileUpload(const std::string& key, Loaded& tile, const std::string& reason) {
    if(!tile.uploadError.empty())return;
    tile.uploadError=reason;
    saida::Log::error("[World streaming] upload failed for ",key,": ",reason);
    text("stream-status","Terrain unavailable: "+reason);
    if(smoke) {
        saida::Log::error("[World E2E] FAIL terrain upload: ",reason);
        testFailed=true;engine.sceneTree().quit();
    } else if(pending) {
        pending=false;warming=false;showMap(true);
        text("status","Terrain could not be loaded: "+reason);
    }
}

void World::Impl::uploadParts() {
    for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles)
        for(auto* mesh:t.partMeshes)if(mesh->uploadState()==saida::Mesh::UploadState::Failed)
            failTileUpload(key,t,mesh->uploadError());
    const auto start=std::chrono::steady_clock::now();
    const auto anchor=streamAnchor();
    const auto priority=orderedNearby(anchor.x,anchor.y);
    // A tile's replacement uploads right after it: the old one is shown
    // until then, so it is the one standing in for it on screen.
    std::vector<std::pair<const std::string*,Loaded*>> order;
    for(const auto tile:priority)for(auto* tiles:{&loaded,&incoming}) {
        auto found=tiles->find(tile.key());
        if(found!=tiles->end())order.push_back({&found->first,&found->second});
    }
    for(auto [name,tile]:order) {
        const auto& key=*name;auto& t=*tile;
        if(!t.geography||!t.uploadError.empty())continue;
        const auto& parts=std::any_cast<const PreparedTile&>(t.served->prepared).parts;
        while(t.nextPart<parts.size()) {
            const PartUpload& part=parts[t.nextPart++];
            auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(part.prepared));
            if(!mesh) {
                // Said, never silent: a tile missing a part looks like OSM missing it.
                failTileUpload(key,t,"geometry arena refused "+part.name);
                break;
            }
            t.partMeshes.push_back(mesh);
            meshCollider(*t.geography,std::make_unique<saida::MeshNode>(part.name,mesh,
                material(t.served->cooked.parts[part.material].material)));
            if(msSince(start)>=kPartUploadMs)return;
        }
    }
}

const std::vector<PartUpload>& World::Impl::buildingParts(const Loaded& t,size_t index,int level) {
    const auto& source=std::any_cast<const PreparedTile&>(t.served->prepared).buildings[index];
    return level==0&&t.reducedDensity?source.reducedNear:source.levels[size_t(level)];
}

const std::vector<r1::MeshPart>& World::Impl::buildingMaterials(const Loaded& t,size_t index,int level) {
    const auto& source=t.served->cooked.buildings[index];
    return level==0&&t.reducedDensity?source.reducedNear:source.levels[size_t(level)];
}

size_t World::Impl::residentVertices(const json& data) { return data.value("residentVertices",data.at("vertices").get<size_t>()); }

size_t World::Impl::residentIndices(const json& data) { return data.value("residentIndices",data.value("indices",size_t(0))); }

std::pair<size_t,size_t> World::Impl::buildingExtraGeometry(const Loaded& t) {
    size_t vertices=0,indices=0;
    for(size_t i=0;i<t.buildings.size();++i)for(int level=0;level<2;++level)
        if(t.buildings[i].levels[size_t(level)].started)for(const auto& part:buildingParts(t,i,level)) {
            vertices+=part.prepared->geometry.vertices.size();indices+=part.prepared->geometry.indices.size();
        }
    return {vertices,indices};
}

bool World::Impl::buildingsReady(const Loaded& t) {
    for(size_t i=0;i<t.buildings.size();++i) {
        const auto& state=t.buildings[i].levels[2];
        if(!state.started||state.nextPart<buildingParts(t,i,2).size())return false;
        for(auto* mesh:state.meshes)if(!mesh->loaded())return false;
    }
    return true;
}

bool World::Impl::nearBuildingReady(const Loaded& t,size_t footprint) {
    if(footprint>=t.visualForFootprint.size()||t.visualForFootprint[footprint]<0)return false;
    const auto& b=t.buildings[size_t(t.visualForFootprint[footprint])];
    return b.lod->activeLevel()==0&&b.levels[0].started&&
        std::all_of(b.levels[0].meshes.begin(),b.levels[0].meshes.end(),[](auto* mesh){return mesh->loaded();});
}

void World::Impl::updateBuildingLods() {
    SAIDA_PROFILE_SCOPE("World/BuildingLods");
    // An abandoned upload is protected by the resource cache until its
    // job finishes. Sweep once more then, so it cannot retain arena space.
    if(buildingTrimPending&&engine.resources().assetLoadsSettled()) {trim();buildingTrimPending=false;}
    struct Candidate {Loaded* tile;size_t index;double distance;};
    std::vector<Candidate> candidates;
    size_t reservedV=farVertices()+farPack.vertices+nextFarPack.vertices,reservedI=0;
    size_t pendingV=0,pendingI=0;
    for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles) {
        reservedV+=residentVertices(t.data);reservedI+=residentIndices(t.data);
        const auto eye=cameraTileLocal(t);
        for(size_t i=0;i<t.buildings.size();++i) {
            auto& b=t.buildings[i];const auto& source=t.served->cooked.buildings[i];
            const double dx=eye.x-std::clamp(eye.x,source.low.x,source.high.x);
            const double dz=eye.z-std::clamp(eye.z,source.low.z,source.high.z);
            const double d=std::hypot(dx,dz);
            b.close=d<(b.close?kBuildingNearRelease:kBuildingNearLoad);
            b.lod->setFinestLevel(playing&&!pending&&!menu&&t.node->isVisibleInHierarchy()?(b.close?0:1):2);
            candidates.push_back({&t,i,d});
            for(int level=0;level<3;++level) {
                auto& state=b.levels[size_t(level)];const auto& parts=buildingParts(t,i,level);
                bool failedUpload=false;
                bool ready=state.started&&state.nextPart==parts.size();
                for(size_t n=0;n<state.meshes.size();++n) {
                    auto* mesh=state.meshes[n];ready=ready&&mesh->loaded();
                    failedUpload|=mesh->uploadState()==saida::Mesh::UploadState::Failed;
                    if(!mesh->loaded()) {pendingV+=parts[n].prepared->geometry.vertices.size();pendingI+=parts[n].prepared->geometry.indices.size();}
                }
                b.lod->setLevelReady(level,ready);
                if(state.started&&state.nextPart==parts.size()&&!ready&&!state.failed&&
                   (failedUpload||engine.resources().assetLoadsSettled())) {
                    state.failed=true;
                    saida::Log::error("[World building LOD] ",source.id," level=",level,
                        " GPU upload failed; retaining available representation");
                    if(level==2)failTileUpload(key,t,"building Far geometry upload failed");
                    if(smoke){testFailed=true;engine.sceneTree().quit();return;}
                }
                if(level<2&&state.started)for(const auto& part:parts) {
                    reservedV+=part.prepared->geometry.vertices.size();
                    reservedI+=part.prepared->geometry.indices.size();
                }
            }
        }
    }
    bool released=false;
    for(const auto& c:candidates) {
        auto& t=*c.tile;auto& b=t.buildings[c.index];
        const int desired=playing&&!pending&&!menu&&t.node->isVisibleInHierarchy()
            ?std::max(b.lod->desiredLevel(),b.lod->finestLevel()):2;
        for(int level=0;level<2;++level) {
            auto& state=b.levels[size_t(level)];
            if(!state.started||level==desired||level==b.lod->activeLevel()||level==b.lod->fadingLevel())continue;
            b.lod->setLevelReady(level,false);
            for(const auto& part:buildingParts(t,c.index,level)) {
                reservedV-=part.prepared->geometry.vertices.size();reservedI-=part.prepared->geometry.indices.size();
            }
            while(!state.root->children().empty())state.root->removeChild(state.root->children().front().get());
            state.meshes.clear();state.nextPart=0;state.started=false;state.refused=false;state.failed=false;released=true;
        }
    }
    if(released) {trim();buildingTrimPending=!engine.resources().assetLoadsSettled();}
    // Ready representations need no upload priority. Keep the same stable
    // distance order among pending requests without sorting the whole city.
    candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](const Candidate& c){
        const auto& t=*c.tile;const auto& b=t.buildings[c.index];
        int level=playing&&!pending&&!menu&&t.node->isVisibleInHierarchy()&&b.levels[2].started&&b.levels[2].nextPart==buildingParts(t,c.index,2).size()
            ?std::max(b.lod->desiredLevel(),b.lod->finestLevel()):2;
        level=std::clamp(level,0,2);
        const auto& state=b.levels[size_t(level)];
        return state.failed||(state.started&&state.nextPart>=buildingParts(t,c.index,level).size());
    }),candidates.end());
    std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.distance<b.distance;});
    const auto started=std::chrono::steady_clock::now();
    for(const auto& c:candidates) {
        auto& t=*c.tile;auto& b=t.buildings[c.index];
        // Always initialize Far, including hidden replacement tiles whose
        // renderer has not selected a view yet.
        int level=playing&&!pending&&!menu&&t.node->isVisibleInHierarchy()&&b.levels[2].started&&b.levels[2].nextPart==buildingParts(t,c.index,2).size()
            ?std::max(b.lod->desiredLevel(),b.lod->finestLevel()):2;
        level=std::clamp(level,0,2);
        auto& state=b.levels[size_t(level)];const auto& parts=buildingParts(t,c.index,level);
        if(state.failed)continue;
        if(!state.started) {
            size_t v=0,idx=0;for(const auto& part:parts){v+=part.prepared->geometry.vertices.size();idx+=part.prepared->geometry.indices.size();}
            if(level<2&&(reservedV+v>residentVertexBudget||reservedI+idx>residentIndexBudget)) {
                if(!state.refused)saida::Log::info("[World building LOD] ",t.served->cooked.buildings[c.index].id,
                    " requested ",level==0?"Near":"Mid"," exceeds resident arena share; keeping ready fallback");
                state.refused=true;continue;
            }
            state.started=true;state.refused=false;
            if(level<2){reservedV+=v;reservedI+=idx;}
        }
        const auto& materials=buildingMaterials(t,c.index,level);
        while(state.nextPart<parts.size()) {
            const auto& part=parts[state.nextPart];
            const auto used=engine.resources().geometryUsage();const auto capacity=engine.resources().geometryCapacity();
            const size_t vertices=part.prepared->geometry.vertices.size(),indices=part.prepared->geometry.indices.size();
            // Shared assets and impact-deformed cars occupy the same arena.
            // Queued building uploads count too; keep the ready LOD while
            // the required actor/streaming reserve is unavailable.
            if(level<2&&(used.vertices+pendingV+vertices+r1::police::kGeometryVertices>capacity.vertices||
                         used.indices+pendingI+indices+r1::police::kGeometryIndices>capacity.indices))break;
            ++state.nextPart;pendingV+=vertices;pendingI+=indices;
            auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(part.prepared));
            if(!mesh)throw std::runtime_error("Building visual mesh registration refused");
            auto node=std::make_unique<saida::MeshNode>(part.name,mesh,material(materials[part.material].material));
            node->castShadows()=level!=2;
            state.root->addChild(std::move(node));state.meshes.push_back(mesh);
            if(msSince(started)>=kPartUploadMs)return;
        }
    }
}

void World::Impl::placeTiles() {
    for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles) {
        // Keep the precise destination: composing a translation from the
        // old million-metre float pose loses centimetres at arrival.
        const auto position=glm::vec3(origin.local(t.frame.origin));
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*t.frame.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*t.node,position,rotation);
    }
    for(auto& f:farLandmarks)if(f.node)placeFar(f);
    placeFarRelief();
    placeFarPack();
}

void World::Impl::trim() {
    engine.sceneTree().applyDeferred();
    engine.resources().trimUnused(engine.sceneTree().world().resourceUsage());
}

} // namespace r1::world
