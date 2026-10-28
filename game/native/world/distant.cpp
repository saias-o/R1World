#include "session.hpp"

namespace r1::world {
void World::Impl::readFarLandmarks() {
    for(const auto& l:r1::landmarks()) {
        FarLandmark f;
        f.slug=l.slug;f.tile=r1::tileAt(l.lon,l.lat).key();
        f.lon=l.lon;f.lat=l.lat;f.alt=l.groundAlt;
        for(size_t i=1;i<l.levels.size();++i)f.levels.push_back({l.levels[i].path,l.levels[i].until,l.levels[i].vertices});
        farLandmarks.push_back(std::move(f));
    }
    saida::Log::info("[World landmarks] ",farLandmarks.size()," landmarks visible from afar, to ",
                     r1::landmarkFarRange()," m");
}

bool World::Impl::nearModel(FarLandmark& f) {
    auto it=loaded.find(f.tile);
    if(it==loaded.end())return false;
    auto& t=it->second;
    if(f.seenTile!=t.node) {
        f.seenTile=t.node;f.nearIndex=-1;
        const std::string name="landmark "+f.slug;
        for(size_t i=0;i<t.props.size();++i)
            if(t.props[i].value("name",std::string())==name){f.nearIndex=long(i);break;}
    }
    // A tile cooked before its landmark carries none: keep the far model.
    return f.nearIndex>=0&&long(t.nextProp)>f.nearIndex;
}

void World::Impl::placeFar(FarLandmark& f) {
    const auto position=glm::vec3(origin.local(ecef(f.lon,f.lat,f.alt)));
    const Frame own(f.lon,f.lat,f.alt);
    const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
    engine.sceneTree().world().rebaseSubtreeTo(*f.node,position,rotation);
}

size_t World::Impl::farVertices() const {
    size_t n=0;
    for(const auto& f:farLandmarks)if(f.node&&f.shown>=0)n+=f.levels[size_t(f.shown)].vertices;
    return n;
}

void World::Impl::updateFar() {
    if(toasterMode) {
        for(auto& f:farLandmarks) {
            if(f.node){f.node->queueFree();f.node=nullptr;}
            f.shown=-1;f.requested=-1;f.loading.reset();
        }
        return;
    }
    for(auto& f:farLandmarks) {
        const glm::dvec3 here=origin.local(ecef(f.lon,f.lat,f.alt));
        const double distance=std::hypot(here.x,here.z);
        int want=-1;
        if(playing&&!nearModel(f))
            for(size_t i=0;i<f.levels.size();++i)if(distance<=f.levels[i].until){want=int(i);break;}
        if(want==f.shown)continue;
        if(want<0) {
            if(f.node){f.node->queueFree();f.node=nullptr;}
            f.shown=-1;f.requested=-1;f.loading.reset();continue;
        }
        const auto& level=f.levels[size_t(want)];
        if(f.requested!=want) {
            f.loading=saida::GLTFLoader::request((game/level.path).string(),engine.resources());
            f.requested=want;
        }
        if(f.loading&&!f.loading.failed()&&!f.loading.ready())continue;
        auto n=std::make_unique<saida::Node>("far "+f.slug);
        if(!f.loading||f.loading.failed()||!saida::GLTFLoader::instantiate(f.loading,*n,engine.resources())) {
            // Keeps `shown`, so a model that will not load is reported
            // once per distance band rather than once a frame.
            saida::Log::warn("[World landmarks] ",f.slug," level ",want+1," failed to load: ",level.path," ",f.loading.error());
            if(f.node){f.node->queueFree();f.node=nullptr;}
            f.shown=want;f.loading.reset();
            if(smoke){testFailed=true;engine.sceneTree().quit();}
            continue;
        }
        if(f.node)f.node->queueFree();
        f.shown=want;
        f.node=engine.sceneTree().world().addChild(std::move(n));
        placeFar(f);
        saida::Log::info("[World landmarks] ",f.slug," level ",want+1," at ",int(distance)," m");
    }
}

void World::Impl::placeFarPack() {
  for(auto* pack:{&farPack,&nextFarPack}) {
    if(!pack->node)continue;
    const auto position=glm::vec3(origin.local(ecef(pack->lon,pack->lat,0.)));
    const Frame own(pack->lon,pack->lat,0.);
    const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
    engine.sceneTree().world().rebaseSubtreeTo(*pack->node,position,rotation);
  }
}

void World::Impl::updateFarPack() {
    Loaded* here=playing&&!pending?tile(lon,lat):nullptr;
    const auto ice=here&&here->seaIce?here->served->cooked.seaIce:nullptr;
    if(!ice) {
        if(nextFarPack.node){nextFarPack.node->queueFree();nextFarPack=FarPack{};}
        if(farPack.node){farPack.node->queueFree();farPack=FarPack{};saida::Log::info("[World ice] far pack taken down");}
        return;
    }
    if(nextFarPack.node) {
        if(nextFarPack.ice!=ice||metresBetween(lon,lat,nextFarPack.lon,nextFarPack.lat)>=2000.) {
            nextFarPack.node->queueFree();nextFarPack=FarPack{};
        } else {
            if(nextFarPack.refused)return;
            bool ready=true;
            nextFarPack.node->traverse([&](saida::Node& node,const glm::mat4&) {
                if(auto* mesh=node.mesh())ready=ready&&mesh->loaded();
            });
            if(!ready) {
                if(engine.resources().assetLoadsSettled()) {
                    nextFarPack.refused=true;
                    saida::Log::error("[World ice] far pack geometry upload failed");
                    text("stream-status","Distant sea ice unavailable: geometry upload refused.");
                    if(smoke){testFailed=true;engine.sceneTree().quit();}
                }
                return;
            }
            if(farPack.node)farPack.node->queueFree();
            farPack=nextFarPack;nextFarPack=FarPack{};farPack.node->setVisible(true);
        }
    }
    if(farPack.node&&farPack.ice==ice&&metresBetween(lon,lat,farPack.lon,farPack.lat)<2000.)return;
    const auto started=std::chrono::steady_clock::now();
    const auto parts=r1::buildFarPack(*ice,lon,lat);
    auto root=std::make_unique<saida::Node>("far pack");
    size_t vertices=0;
    for(size_t i=0;i<parts.size();++i) {
        PartUpload up=uploadOf(parts[i],i);
        const auto count=up.vertices.size();
        auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(
            saida::prepareMesh({std::move(up.vertices),std::move(up.indices)})));
        if(!mesh){saida::Log::warn("[World ice] the geometry arena refused ",parts[i].name);continue;}
        root->addChild(std::make_unique<saida::MeshNode>(parts[i].name,mesh,material(parts[i].material)));
        vertices+=count;
    }
    // The old pack remains visible and owned until all replacement meshes
    // are draw-ready; a hidden branch pins the new queued resources.
    root->setVisible(false);
    nextFarPack=FarPack{engine.sceneTree().world().addChild(std::move(root)),lon,lat,vertices,ice};
    placeFarPack();
    saida::Log::info("[World ice] far pack to ",r1::kFarPackRadius/1000.," km around ",lon,", ",lat,
                     ": ",vertices," vertices, ",ice->measured?"measured ":"inferred ",ice->date,
                     " build_ms=",msSince(started));
}

void World::Impl::placeFarRelief() {
    if(!farRelief.node)return;
    const auto position=glm::vec3(origin.local(ecef(farRelief.lon,farRelief.lat,0.)));
    const Frame own(farRelief.lon,farRelief.lat,0.);
    const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
    engine.sceneTree().world().rebaseSubtreeTo(*farRelief.node,position,rotation);
}

void World::Impl::updateFarRelief() {
    SAIDA_PROFILE_SCOPE("World/FarRelief");
    // Where it cannot be drawn: before a spawn, over the pack (whose own
    // far field is the horizon there), and past the Mercator images' 85 degrees.
    const bool wanted=!toasterMode&&playing&&!pending&&!farPack.node&&std::abs(lat)<84.5;
    if(!wanted) {
        if(farRelief.node&&farRelief.node->visible())farRelief.node->setVisible(false);
        return;
    }
    if(!farRelief.node) {
        auto n=std::make_unique<saida::TerrainRingsNode>();
        n->setName("far relief");
        n->levels=kFarLevels;n->baseSpacing=float(kFarBaseSpacing);
        farRelief.node=static_cast<saida::TerrainRingsNode*>(engine.sceneTree().world().addChild(std::move(n)));
    }
    // Sampled behind the arrival's opaque globe, drawn once it fades.
    if(farRelief.node->visible()==behindGlobe())farRelief.node->setVisible(!behindGlobe());
    auto& f=farRelief;
    // A new tangent frame where the player is, when the old one is far.
    glm::dvec2 focus{0.};
    if(f.job) {const r1::P3 p=f.anchor.toEngine(lon,lat,0.);focus={p.x,p.z};}
    if(!f.job||glm::length(focus)>kFarReanchor) {
        f.lon=lon;f.lat=lat;f.anchor=r1::Anchor::at(lon,lat,0.);
        f.layers=std::make_shared<r1::FarLayers>(r1::climateAt(r1::profileFor(lon,lat).climate,lat));
        f.layersSeen=0;
        f.node->clearLevels();f.asked={};f.retryAt={};f.reach=0;++f.generation;
        f.job=std::make_shared<FarJob>();
        focus={0.,0.};
        placeFarRelief();
        saida::Log::info("[World far] rings anchored at ",lon,", ",lat);
    }
    f.node->focus=focus;
    // What came back from the thread.
    std::vector<std::pair<int,r1::FarLevel>> done;std::vector<std::string> said;
    {std::lock_guard<std::mutex> g(f.job->lock);done.swap(f.job->done);said.swap(f.job->said);}
    for(const auto& line:said)saida::Log::info("[World far] ",line);
    for(auto& [k,level]:done) {
        f.retryAt[size_t(k)]=level.installedFallback?std::chrono::steady_clock::now()+kFarRetry:
            std::chrono::steady_clock::time_point{};
        saida::TerrainRingsNode::Level l;
        l.origin={level.originX,level.originZ};l.spacing=level.spacing;
        l.heights=std::move(level.heights);l.layers=std::move(level.layers);
        if(f.node->setLevel(k,std::move(l)))
            f.reach=std::max(f.reach,0.5*saida::TerrainRingsNode::kResolution*level.spacing);
    }
    if(f.layers->revision()!=f.layersSeen) {
        f.layersSeen=f.layers->revision();
        const auto swatches=f.layers->swatches();
        const auto surfaces=f.layers->materials();
        // Stand-scale variation remains visible beyond the physical
        // texture's detail; its mean stays the Atlas's measured albedo.
        constexpr float kGroundMacroMetres=128.0f, kForestMacroMetres=12.0f;
        constexpr float kGroundVariation=0.35f, kForestVariation=0.65f, kSnowVariation=0.06f;
        constexpr float kCanopyNormalStrength=0.45f;
        for(size_t i=0;i<size_t(saida::TerrainRingsNode::kMaxLayers);++i) {
            if(i>=swatches.size()) {f.node->setLayer(int(i),{});continue;}
            saida::TerrainRingsNode::Layer layer;
            layer.albedo={float(swatches[i].color[0]),float(swatches[i].color[1]),float(swatches[i].color[2])};
            layer.roughness=float(swatches[i].roughness);
            if(i==r1::FarLayers::kWater) {
                layer.water=true;
                layer.albedo={.01f,.07f,.11f};
                layer.roughness=.05f;
                layer.waveAmplitude=.12f;
                layer.wavelength=9.0f;
            }
            if(!surfaces[i].baseColorTexture.empty())layer.material=material(surfaces[i]);
            layer.textureScale=float(surfaces[i].uvScale);
            layer.macroSize=i==r1::FarLayers::kForest?kForestMacroMetres:kGroundMacroMetres;
            layer.macroVariation=i==r1::FarLayers::kWater?0.0f:
                i==r1::FarLayers::kSnow?kSnowVariation:
                i==r1::FarLayers::kForest?kForestVariation:kGroundVariation;
            layer.macroNormalStrength=i==r1::FarLayers::kForest?kCanopyNormalStrength:0.0f;
            f.node->setLayer(int(i),layer);
        }
    }
    // The resident tiles draw their own ground: holes in the rings.
    std::vector<saida::TerrainRingsNode::Hole> holes;
    for(const auto& [key,t]:loaded) {
        if(!t.geography||t.ocean)continue;
        auto at=[&](double x,double y){const r1::P3 p=f.anchor.toEngine(x,y,0.);return glm::dvec2(p.x,p.z);};
        holes.push_back({at(t.west,t.south),at(t.east,t.south),at(t.east,t.north),at(t.west,t.north)});
    }
    f.node->setHoles(std::move(holes));
    // The rings whose samples no longer sit around the player, finest first.
    {std::lock_guard<std::mutex> g(f.job->lock);if(f.job->busy)return;}
    std::vector<std::pair<int,glm::dvec2>> asks;
    for(int k=0;k<kFarLevels;++k) {
        const glm::dvec2 o=f.node->levelOrigin(k,focus);
        const auto retry=f.retryAt[size_t(k)];
        const bool due=retry!=std::chrono::steady_clock::time_point{}&&std::chrono::steady_clock::now()>=retry;
        if(f.asked[size_t(k)]&&o==f.origins[size_t(k)]&&!due)continue;
        f.origins[size_t(k)]=o;f.asked[size_t(k)]=true;asks.push_back({k,o});
        f.retryAt[size_t(k)]=std::chrono::steady_clock::now()+kFarRetry;
    }
    if(asks.empty())return;
    f.job->busy=true;
    std::thread([job=f.job,layers=f.layers,anchor=f.anchor,asks,root=game.string()] {
        const r1::ObservationStore store(root);
        for(const auto& [k,o]:asks) {
            const double spacing=kFarBaseSpacing*std::ldexp(1.,k);
            const auto started=std::chrono::steady_clock::now();
            try {
                auto level=r1::sampleWorldFarLevel(store,anchor,o.x,o.y,spacing,
                                                  saida::TerrainRingsNode::kResolution,*layers);
                std::ostringstream line;
                line<<"ring "<<k<<": "<<spacing<<" m, "
                    <<(spacing>=r1::kInstalledReliefSpacing?"local relief":"Terrain Tiles z"+std::to_string(level.zoom))
                    <<(level.installedFallback?" (installed fallback; retry pending)":"")
                    <<", sea "<<level.seaCells<<" snow "<<level.snowCells<<" rock "<<level.rockCells<<" forest "<<level.forestCells<<" scrub "<<level.scrubCells
                    <<" cells inferred, build_ms="<<msSince(started);
                std::lock_guard<std::mutex> g(job->lock);
                job->said.push_back(line.str());
                job->done.push_back({k,std::move(level)});
            } catch(const std::exception& e) {
                std::lock_guard<std::mutex> g(job->lock);
                job->said.push_back("ring "+std::to_string(k)+" refused: no relief at "+std::to_string(spacing)+" m: "+e.what());
            }
        }
        std::lock_guard<std::mutex> g(job->lock);job->busy=false;
    }).detach();
}

} // namespace r1::world
