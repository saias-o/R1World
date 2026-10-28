#include "session.hpp"

namespace r1::world {
std::unique_ptr<saida::Node> World::Impl::clonePlant(saida::Node& source) {
    std::unique_ptr<saida::Node> out;
    if(source.mesh()){
        auto mesh=std::make_unique<saida::MeshNode>(source.name(),source.mesh(),source.material());
        if(auto* original=dynamic_cast<saida::MeshNode*>(&source))mesh->castShadows()=original->castShadows();
        out=std::move(mesh);
    }
    else out=std::make_unique<saida::Node>(source.name());
    out->transform()=source.transform();
    for(auto& child:source.children())out->addChild(clonePlant(*child));
    return out;
}

bool World::Impl::uniformScale(const glm::vec3& s) {
    return std::abs(s.x-s.y)<1e-6f&&std::abs(s.y-s.z)<1e-6f;
}

bool World::Impl::flattenable(saida::Node& node) const {
    if(!uniformScale(node.transform().scale))return false;
    for(auto& child:node.children())if(!flattenable(*child))return false;
    return true;
}

void World::Impl::collectMeshes(saida::Node& source,const saida::Transform& above,
                       std::vector<std::unique_ptr<saida::Node>>& out) {
    saida::Transform here;
    here.rotation=above.rotation*source.transform().rotation;
    here.scale=above.scale*source.transform().scale;
    here.position=above.position+above.rotation*(above.scale*source.transform().position);
    if(source.mesh()) {
        auto mesh=std::make_unique<saida::MeshNode>(source.name(),source.mesh(),source.material());
        if(auto* original=dynamic_cast<saida::MeshNode*>(&source))mesh->castShadows()=original->castShadows();
        mesh->transform()=here;
        out.push_back(std::move(mesh));
    }
    for(auto& child:source.children())collectMeshes(*child,here,out);
}

saida::Node* World::Impl::prototype(const std::string& path) {
    auto& found=naturePrototypes[path];
    if(!found) {
        const auto retry=budgetRetries.find(path);
        if(retry!=budgetRetries.end()&&std::chrono::steady_clock::now()<retry->second.at)return nullptr;
        auto& handle=prototypeLoads[path];
        if(!handle)handle=saida::GLTFLoader::request((game/path).string(),engine.resources());
        if(handle&&handle.failed()&&handle.error().find("budget exceeded")!=std::string::npos) {
            auto& again=budgetRetries[path];
            if(again.tries<kBudgetRetries) {
                if(again.tries==0)saida::Log::info("[World streaming] ",path," refused by the decode budget for now; asked again each second");
                ++again.tries;again.at=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                prototypeLoads.erase(path);
                return nullptr;
            }
        }
        if(!handle||handle.failed())throw std::runtime_error("Model load failed: "+path+" "+handle.error());
        if(!handle.ready())return nullptr;
        auto imported=std::make_unique<saida::Node>(fs::path(path).stem().string());
        saida::GLTFLoadOptions importOptions;
        importOptions.retainDeformationSource=path.rfind("assets/models/vehicles/",0)==0;
        if(!saida::GLTFLoader::instantiate(handle,*imported,engine.resources(),importOptions))
            throw std::runtime_error("Model instantiation failed: "+path);
        if(path=="assets/models/external/nature_selected/urban_tree.glb"
           ||path=="assets/models/external/nature_selected/urban_tree_mid.glb") {
            // Keep the scanned leaf colours and gloss map, but give dry
            // foliage a softer lobe, including its bright grazing angles.
            // Applied once to shared prototypes, never per tree or frame.
            constexpr float kDryLeafSpecularStrength=.25f;
            imported->traverse([&](saida::Node& n,const glm::mat4&){
                auto* mesh=dynamic_cast<saida::MeshNode*>(&n);
                if(!mesh||!mesh->material())return;
                auto desc=mesh->material()->desc();
                if(desc.alphaCutoff<=0.f)return; // Bark keeps its authored material.
                desc.specularStrength*=kDryLeafSpecularStrength;
                mesh->setMaterial(engine.resources().getMaterial(desc));
            });
        }
        if(path=="assets/models/external/nature_selected/urban_tree_far.glb"
           ||path=="assets/models/external/nature_selected/pine_tall_far.glb") {
            // A filtered atlas normal cannot represent every leaf direction
            // in a canopy pixel. Keep half the engine's volume response so
            // backlit crowns retain scattered light, while atlas normals
            // still supply contrast. The source albedo and atlas stay intact.
            imported->traverse([&](saida::Node& n,const glm::mat4&){
                auto* mesh=dynamic_cast<saida::MeshNode*>(&n);
                if(!mesh||!mesh->material())return;
                auto desc=mesh->material()->desc();
                if(!desc.billboardColumns)return;
                desc.normalStrength=.5f;
                mesh->setMaterial(engine.resources().getMaterial(desc));
            });
        }
        found=prototypes->addChild(std::move(imported));
        prototypeLoads.erase(path);
    }
    return found;
}

bool World::Impl::modelsReady(const json& doc) {
    bool ready=true;
    const auto path=doc.value("importedFrom",std::string());
    if(!path.empty()&&!prototype(path))ready=false;
    for(const auto& child:doc.value("children",json::array()))if(!modelsReady(child))ready=false;
    return ready;
}

saida::Node* World::Impl::prototypeForStartup(const std::string& path) {
    // Before the frame loop only: CPU decoding still uses the worker,
    // while GPU publication waits for the normal resource pump.
    for(;;) {
        if(auto* node=prototype(path))return node;
        engine.resources().assetLoader().pump();
        std::this_thread::yield();
    }
}

void World::Impl::warm() {
    // During arrival, streamProps prepares each mounted tile immediately
    // instead of warming a second copy of the selected tile's models.
    if(pending||drop.phase!=Drop::None||service->status().memoryPaused)return;
    const auto at=tileAt(pickLon,pickLat);
    const auto served=service->find(at.gen());
    if(!served)return;
    if(warmTile!=at.key()||warmSerial!=served->serial) {
        warmTile=at.key();warmSerial=served->serial;
        std::set<std::string> models;
        auto collect=[&](const auto& self,const json& node)->void {
            const auto path=node.value("importedFrom",std::string());
            if(!path.empty())models.insert(path);
            if(node.contains("children"))for(const auto& child:node.at("children"))self(self,child);
        };
        for(const auto& prop:served->cooked.props)collect(collect,prop);
        warmList.assign(models.begin(),models.end());
        saida::Log::info("[World prefetch] ",warmTile,": ",warmList.size()," local models to warm");
    }
    if(warmList.empty())return;
    const std::string path=warmList.back();
    if(prototype(path))warmList.pop_back();
}

std::unique_ptr<saida::Node> World::Impl::plantNode(json doc) {
    auto path=doc.value("importedFrom",std::string());
    auto children=doc.value("children",json::array());
    doc.erase("importedFrom");doc.erase("children");
    auto node=saida::SceneSerializer::nodeFromJson(doc.dump(),engine.resources());
    if(!path.empty()){
        saida::Node* prototype=this->prototype(path);
        if(!prototype)throw std::runtime_error("Nature prototype failed: "+path);
        if(flattenable(*prototype)) {
            std::vector<std::unique_ptr<saida::Node>> meshes;
            collectMeshes(*prototype,saida::Transform{},meshes);
            for(auto& mesh:meshes)node->addChild(std::move(mesh));
        } else node->addChild(clonePlant(*prototype));
    }
    for(auto& child:children)node->addChild(plantNode(child));
    // A holder whose whole content is one mesh becomes that mesh. The
    // tile authors a plant as root -> {Near, Far} -> model, and after
    // flattening each holder has exactly one child and nothing else of its
    // own: five nodes to say "this tree has two levels of detail". The
    // holder's name is what the LOD switch looks for, so the mesh takes it.
    //
    // The top-level plant keeps its own node: it carries the groups the
    // rest of the file finds it by, and `blocked` reads its position.
    if(node->children().size()==1&&node->behaviours().empty()
       &&!node->isInGroup("vegetation")&&node->children()[0]->mesh()
       &&node->children()[0]->children().empty()
       &&uniformScale(node->transform().scale)
       &&uniformScale(node->children()[0]->transform().scale)) {
        auto only=node->detachChild(node->children()[0].get());
        const saida::Transform above=node->transform(),below=only->transform();
        saida::Transform merged;
        merged.rotation=above.rotation*below.rotation;
        merged.scale=above.scale*below.scale;
        merged.position=above.position+above.rotation*(above.scale*below.position);
        only->transform()=merged;
        only->setName(node->name());
        only->setEnabled(node->enabled());
        return only;
    }
    if(node->isInGroup("vegetation") && node->findByPath("Near") && node->findByPath("Far")) {
        auto* lod = node->addBehaviour<saida::LODGroupBehaviour>();
        // Coverage is the bounding sphere's projected diameter, squared
        // (MeshLod.cpp). Mid ends at 9%: roughly 80 m for a nine-metre
        // tree at 60 degrees. Beyond it the Mid-derived view atlas retains
        // its silhouette with one camera-facing quad. Full geometry is for
        // within kTreeNearRadius (`updateNature`), which cover far more
        // than 10% even when small.
        if(node->isInGroup("understory"))lod->setLevels({{"Near", .003f}, {"Far", 0.f}});
        else if(node->findByPath("Mid"))lod->setLevels({{"Near", .1f}, {"Mid", .09f}, {"Far", 0.f}});
        else lod->setLevels({{"Near", .06f}, {"Far", 0.f}});
        // A level dissolves into the next over half a second instead of
        // swapping in one frame (Saida's screen-door cross-fade).
        lod->setCrossFade(.5f);
    }
    return node;
}

void World::Impl::updateNature() {
    SAIDA_PROFILE_SCOPE("World/Nature");
    for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles){
        const auto pos=t.frame.local(ecef(lon,lat,alt));
        for(Plant& plant:t.vegetation){
            if(plant.tree&&!debugTrees)continue;
            const auto p=plant.node->transform().position;
            double d=std::hypot(p.x-pos.x,p.z-pos.z);
            const double maxDist=plant.understory?(toasterMode?110.:170.):
                plant.grass?(toasterMode?35.:65.):(toasterMode?280.:550.);
            plant.node->setVisible(d<maxDist);
            if(plant.lod) {
                // Entered inside the radius, left a tenth beyond it, so
                // a tree at the edge does not flicker between two models.
                const double radius=plant.understory?22.:kTreeNearRadius;
                plant.close=d<radius*(plant.close?1.1:1.);
                // The low vegetation's fine mesh already has only 176
                // triangles. Both quality modes keep it within arm's reach.
                plant.lod->setFinestLevel(plant.understory?(plant.close?0:1):
                    (toasterMode?1:(plant.close?0:1)));
            }
        }
    }
}

} // namespace r1::world
