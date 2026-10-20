#include "menu_globe.hpp"
#include "render/RenderFeatureRegistry.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/Texture.hpp"
#include "core/Time.hpp"
#include "core/Paths.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <tuple>

namespace r1 {
namespace {
constexpr double pi=3.14159265358979323846;
constexpr double radians=pi/180.;
struct Push {
    glm::vec4 east,north,outward;
    glm::vec4 viewport;
    glm::vec4 lens; // physical aspect, altitude, time, tile opacity
    glm::vec4 tile; // zoom level, x, y, 0 base / 1 tile
    glm::vec4 sun;  // Earth-fixed unit direction towards the Sun
};
class MenuGlobeFeature final:public saida::ScenePassFeature {
    std::shared_ptr<MenuGlobeState> state;
    saida::ResourceManager* resources=nullptr;
    std::unique_ptr<saida::rhi::Pipeline> pipeline;
    std::unique_ptr<saida::rhi::BindGroupLayout> layout;
    std::array<saida::AssetID,4> images{};
    saida::AssetID preview=0;
    bool dayRetained=false;
    struct Slot {std::vector<std::unique_ptr<saida::rhi::BindGroup>> sets;};
    std::vector<Slot> slots;
    struct ReadyTile {MapTile tile;saida::Texture* texture;float mode;float alpha;}; // mode 2 lit imagery, 1 map
    std::vector<ReadyTile> readyTiles;
    // Tiles stay retained (resident, loaded first) while recently used: a
    // zoom out and back in finds them still on the GPU. Least recent go first.
    static constexpr size_t kRetainedTiles=320;
    static constexpr double kFadeSeconds=.3;
    std::unordered_map<saida::AssetID,uint64_t> retainedTiles;
    std::unordered_map<std::string,saida::AssetID> tileIds;
    // What each layer shows. A level is shown only once every tile the view
    // needs at it is on the GPU, then replaces the previous level in one fade:
    // no patchwork, no blurry tile sharpening on its own.
    struct Shown {MapTile tile;saida::AssetID id;};
    struct LayerState {
        int z=-1,previousZ=-1;
        std::vector<Shown> shown,previous;
        double swapped=-1e9;
    };
    LayerState layerStates[2]; // imagery, map
    uint64_t tileFrame=0;
    std::array<saida::Texture*,4> textures{};
    bool ready=false;
public:
    MenuGlobeFeature(std::shared_ptr<MenuGlobeState> model):state(std::move(model)){}
    void createPipelines(const saida::RenderContext& ctx) override {
        resources=&ctx.resources;
        std::vector<saida::rhi::BindGroupLayoutEntry> entries;
        for(uint32_t i=0;i<5;++i)entries.push_back({i,saida::rhi::BindingType::CombinedImageSampler,saida::rhi::ShaderStages::Fragment});
        layout=std::make_unique<saida::rhi::BindGroupLayout>(ctx.device,entries);
        saida::rhi::Pipeline::Desc desc;
        const auto shaders=std::filesystem::path(saida::executableDirectory())/"menu-shaders";
        desc.vertPath=(shaders/"menu_globe.vert.spv").string();
        desc.fragPath=(shaders/"menu_globe.frag.spv").string();
        desc.colorFormats={ctx.colorFormat};desc.depthFormat=ctx.depthFormat;desc.samples=ctx.samples;
        // The menu covers the gameplay scene. A near depth also prevents that
        // scene's fog, AO and sun flare from leaking into its presentation.
        desc.vertexInput=false;desc.depthTest=true;desc.depthWrite=true;desc.depthCompare=saida::rhi::CompareOp::Always;
        desc.cullMode=saida::rhi::CullMode::None;desc.blendMode=saida::rhi::BlendMode::Alpha;
        desc.bindGroupLayouts={layout.get()};desc.pushConstantSize=sizeof(Push);desc.viewMask=ctx.viewMask;
        pipeline=std::make_unique<saida::rhi::Pipeline>(ctx.device,desc);
        slots.resize(ctx.framesInFlight);
        auto* registry=resources->getRegistry();
        const std::array<const char*,4> names={"earth-day.jpg","earth-clouds.jpg","earth-night.jpg","milky-way.jpg"};
        // No scene node samples these, so scene trims and the GPU budget would
        // evict them mid-menu and leave the globe black while they reload.
        // Retaining also loads them ahead of the world streaming behind.
        for(auto id:images)if(id&&(id!=images[0]||dayRetained))resources->releaseTexture(id);
        if(preview)resources->releaseTexture(preview);
        for(size_t i=0;i<images.size();++i)
            images[i]=registry->registerAsset("assets/menu/"+std::string(names[i]),saida::AssetType::Texture);
        // The 8K day map decodes for seconds and prepares alone, blocking every
        // texture queued after it. The same map at 1024x512 lights the globe
        // first; the 8K map is requested only once the globe is on screen.
        preview=registry->registerAsset("assets/menu/earth-day-preview.jpg",saida::AssetType::Texture);
        resources->retainTexture(preview);
        for(size_t i=1;i<images.size();++i)resources->retainTexture(images[i],i!=1);
        dayRetained=false;
    }
    void recordPrePass(const saida::PrePassContext& pc) override {
        if(!state->visible) {
            // Back in the world: free the prefetch and the stand-ins, but keep
            // what the globe showed resident, so Escape brings the menu back
            // exactly as it was instead of the blurry globe sharpening again.
            std::unordered_set<saida::AssetID> keep;
            for(auto& st:layerStates){st.previous.clear();for(const auto& t:st.shown)keep.insert(t.id);}
            for(auto it=retainedTiles.begin();it!=retainedTiles.end();)
                if(keep.count(it->first))++it;else{resources->releaseTexture(it->first);it=retainedTiles.erase(it);}
            return;
        }
        ready=true;
        for(size_t i=0;i<images.size();++i) {
            textures[i]=i||dayRetained?resources->getTexture(images[i],i!=1):nullptr;
            if(i==0&&!textures[i])textures[i]=resources->getTexture(preview);
            ready=ready&&textures[i];
        }
        if(ready&&!dayRetained){resources->retainTexture(images[0]);dayRetained=true;}
        readyTiles.clear();
        if(!ready)return;
        ++tileFrame;
        const double now=saida::Time::unscaledElapsed();
        gatherLayer(state->imagery,layerStates[0],2,now);
        gatherLayer(state->map,layerStates[1],1,now);
        if(retainedTiles.size()>kRetainedTiles) {
            std::vector<std::pair<uint64_t,saida::AssetID>> idle;
            for(const auto& [id,used]:retainedTiles)if(used<tileFrame)idle.push_back({used,id});
            std::sort(idle.begin(),idle.end());
            for(const auto& [used,id]:idle) {
                if(retainedTiles.size()<=kRetainedTiles)break;
                resources->releaseTexture(id);retainedTiles.erase(id);
            }
        }
    }
    // Every cached tile is retained -- so it loads ahead of the world streaming
    // behind the menu -- and so is whatever is on screen, whatever its level.
    void retain(saida::AssetID id) {
        auto [it,fresh]=retainedTiles.try_emplace(id,tileFrame);
        if(fresh)resources->retainTexture(id);else it->second=tileFrame;
    }
    void gatherLayer(const GlobeLayer& layer,LayerState& st,float mode,double now) {
        if(layer.z<0){st={};return;}
        std::map<std::tuple<int,int,int>,saida::AssetID> cached;
        for(const auto& [tile,path]:layer.cached) {
            const auto key=path.generic_string();
            auto found=tileIds.find(key);
            // Clamped: a repeating sampler blends each edge with the opposite
            // one and draws a seam between neighbours.
            if(found==tileIds.end())found=tileIds.emplace(key,resources->getOrRegister(key,saida::AssetType::Texture,true,saida::rhi::AddressMode::ClampToEdge)).first;
            retain(found->second);
            cached[{tile.z,tile.x,tile.y}]=found->second;
        }
        for(const auto& t:st.shown)retain(t.id);
        for(const auto& t:st.previous)retain(t.id);
        // The level the view wants, complete on the GPU?
        std::vector<Shown> wanted;bool complete=!layer.visible.empty();
        for(const auto& t:layer.visible) {
            const auto it=cached.find({t.z,t.x,t.y});
            if(it==cached.end()||!resources->getTexture(it->second)){complete=false;continue;}
            wanted.push_back({t,it->second});
        }
        if(complete&&layer.z!=st.z) {
            // Fade from what is on screen; a swap during a fade keeps the
            // older, fully drawn level underneath.
            if(now-st.swapped>=kFadeSeconds||st.previous.empty()){st.previous=std::move(st.shown);st.previousZ=st.z;}
            st.shown=std::move(wanted);st.z=layer.z;st.swapped=now;
        } else if(layer.z==st.z) {
            // Same level, the view moved: tiles of equal detail join as they
            // arrive (nothing sharpens, the edge just fills in).
            std::set<std::tuple<int,int,int>> have;
            for(const auto& t:st.shown)have.insert({t.tile.z,t.tile.x,t.tile.y});
            for(const auto& t:wanted)if(have.insert({t.tile.z,t.tile.x,t.tile.y}).second)st.shown.push_back(t);
            if(st.shown.size()>4*layer.visible.size()+16)st.shown=std::move(wanted); // drop what panned far away
        }
        const float alpha=float(std::clamp((now-st.swapped)/kFadeSeconds,0.,1.));
        auto draw=[&](const std::vector<Shown>& tiles,float a){
            for(const auto& t:tiles)if(auto* tex=resources->getTexture(t.id))readyTiles.push_back({t.tile,tex,mode,a});
        };
        if(alpha<1){
            // Under a finer level coming in, the coarser one; under a coarser
            // one (zooming out), the finer one stays on top where it covers.
            if(st.previousZ>st.z){draw(st.shown,1);draw(st.previous,1-alpha);}
            else {draw(st.previous,1);draw(st.shown,alpha);}
        } else {st.previous.clear();draw(st.shown,1);}
    }
    void record(saida::FrameContext& fc) override {
        if(!state->visible||fc.stereo)return;
        for(auto*& texture:textures)if(!texture)texture=resources->defaultWhiteTexture();
        auto& sets=slots.at(fc.frameIndex).sets;sets.clear();
        auto bind=[&](saida::Texture* tile){
            std::vector<saida::rhi::BindGroupEntry> entries;
            for(uint32_t i=0;i<5;++i) {
                auto* texture=i<4?textures[i]:tile;
                saida::rhi::BindGroupEntry entry;entry.binding=i;entry.view=texture->imageView();entry.sampler=texture->sampler();
                entries.push_back(entry);
            }
            sets.push_back(std::make_unique<saida::rhi::BindGroup>(*layout,entries));
            fc.pass.setBindGroup(0,*sets.back());
        };
        const auto b=state->view.basis();
        Push push{glm::vec4(b[0],0),glm::vec4(b[1],0),glm::vec4(b[2],0),state->viewport,
            {float(fc.extent.width)/fc.extent.height,float(state->view.altitude()),float(saida::Time::unscaledElapsed()),
             state->map.opacity},{0,0,0,ready?0.f:-1.f},
            glm::vec4(glm::vec3(GlobeView::direction(state->subsolar.lon,state->subsolar.lat)),0)};
        fc.pass.setPipeline(*pipeline);bind(textures[0]);
        fc.pass.setPushConstants(&push,sizeof(push));fc.pass.draw(3);
        const auto rect=state->viewport;
        const double width=fc.extent.width*rect.z,height=fc.extent.height*rect.w;
        for(const auto& tile:readyTiles) {
            const double n=double(1<<tile.tile.z);
            double left=width,right=0,top=height,bottom=0;
            bool projected=false;
            bool hidden=false;
            for(int y=0;y<=4;++y)for(int x=0;x<=4;++x) {
                const double lon=(tile.tile.x+x*.25)/n*360.-180.;
                const double lat=std::atan(std::sinh(pi*(1.-2.*(tile.tile.y+y*.25)/n)))/radians;
                if(auto p=state->view.project(lon,lat,width,height)) {
                    left=std::min(left,p->x);right=std::max(right,p->x);top=std::min(top,p->y);bottom=std::max(bottom,p->y);projected=true;
                } else hidden=true;
            }
            if(!projected)continue;
            // A large tile crossing the horizon bulges past its visible
            // corners; the shader's own test clips it exactly.
            if(hidden){left=0;top=0;right=width;bottom=height;}
            const int x0=std::clamp(int(std::floor(left-2+rect.x*fc.extent.width)),0,int(fc.extent.width));
            const int x1=std::clamp(int(std::ceil(right+2+rect.x*fc.extent.width)),0,int(fc.extent.width));
            const int y0=std::clamp(int(std::floor(top-2+rect.y*fc.extent.height)),0,int(fc.extent.height));
            const int y1=std::clamp(int(std::ceil(bottom+2+rect.y*fc.extent.height)),0,int(fc.extent.height));
            if(x1<=x0||y1<=y0)continue;
            fc.pass.setScissor(x0,y0,x1-x0,y1-y0);
            bind(tile.texture);
            // View centre minus tile corner, in tiles, kept in double until
            // the difference is small (see tileCoord in the shader).
            const double cx=(state->view.lon+180.)/360.*n;
            const double cy=(1.-std::asinh(std::tan(std::clamp(state->view.lat,-MapView::mercatorLimit,MapView::mercatorLimit)*radians))/pi)*.5*n;
            const double dx=std::remainder(cx-tile.tile.x,n);
            push.tile={float(tile.tile.z),float(dx),float(cy-tile.tile.y),tile.mode};
            push.sun.w=tile.alpha;
            fc.pass.setPushConstants(&push,sizeof(push));fc.pass.draw(3);
        }
        fc.pass.setScissor(0,0,fc.extent.width,fc.extent.height);
    }
};
}
std::shared_ptr<MenuGlobeState> registerMenuGlobe() {
    auto state=std::make_shared<MenuGlobeState>();
    saida::RenderFeatureRegistry::instance().add(400,[state]{return std::make_unique<MenuGlobeFeature>(state);});
    return state;
}
} // namespace r1
