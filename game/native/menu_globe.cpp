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
#include <unordered_map>
#include <set>
#include <tuple>

namespace r1 {
namespace {
constexpr double pi=3.14159265358979323846;
constexpr double radians=pi/180.;
struct Push {
    glm::vec4 east,north,outward; // east.w: the globe's opacity, north.w: its vignette
    // outward.w: bit mask of the eight same-level tile neighbours bound below
    glm::vec4 viewport;
    glm::vec4 lens; // physical aspect, altitude, time, tile opacity
    glm::vec4 tile; // zoom, view centre minus tile corner, mode (0 base / 1 map / 2 imagery)
    glm::vec4 sun;  // Earth-fixed unit direction towards the Sun
};
class MenuGlobeFeature final:public saida::ScenePassFeature {
    std::shared_ptr<MenuGlobeState> state;
    saida::ResourceManager* resources=nullptr;
    // The second leaves the depth alone: fading, the world behind keeps its
    // own depth, so its fog and shading do not change when the globe goes.
    std::unique_ptr<saida::rhi::Pipeline> pipeline,fadePipeline;
    std::unique_ptr<saida::rhi::BindGroupLayout> layout;
    std::array<saida::AssetID,4> images{};
    saida::AssetID preview=0;
    bool dayRetained=false;
    struct Slot {std::vector<std::unique_ptr<saida::rhi::BindGroup>> sets;};
    std::vector<Slot> slots;
    struct ReadyTile {MapTile tile;saida::Texture* texture;float mode;float alpha=1;}; // mode 2 lit imagery, 1 map
    std::vector<ReadyTile> readyTiles;
    // Tiles stay retained (resident, loaded first) while recently used: a
    // zoom out and back in finds them still on the GPU. Least recent go first.
    std::unordered_map<saida::AssetID,uint64_t> retainedTiles;
    std::unordered_map<std::string,saida::AssetID> tileIds;
    struct RetainedTile {MapTile tile;saida::AssetID id;float mode;};
    std::unordered_map<saida::AssetID,RetainedTile> tileInfo;
    uint64_t tileFrame=0;
    // Each change of satellite picture fades: a tile newly drawn comes in over
    // kImageryFadeSeconds, and the tiles it replaces stay drawn beneath it
    // until it has.
    static constexpr double kImageryFadeSeconds=.5;
    struct Shown {double since=0;uint64_t frame=0;};
    std::unordered_map<saida::AssetID,Shown> shown;
    std::vector<RetainedTile> drawnImagery;  // last frame's imagery, beneath first
    std::array<saida::Texture*,4> textures{};
    bool ready=false;
public:
    MenuGlobeFeature(std::shared_ptr<MenuGlobeState> model):state(std::move(model)){}
    void createPipelines(const saida::RenderContext& ctx) override {
        resources=&ctx.resources;
        std::vector<saida::rhi::BindGroupLayoutEntry> entries;
        for(uint32_t i=0;i<13;++i)entries.push_back({i,saida::rhi::BindingType::CombinedImageSampler,saida::rhi::ShaderStages::Fragment});
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
        desc.depthWrite=false;
        fadePipeline=std::make_unique<saida::rhi::Pipeline>(ctx.device,desc);
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
            state->imageryCovers=false;
            // Keep the bounded recent-view cache through gameplay. Releasing
            // stand-ins and the hidden map here forced Escape/M to decode and
            // upload them again despite the permanent disk cache.
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
        state->imageryCovers=false;
        // The base globe's large textures must not gate detail prefetch.
        ++tileFrame;
        state->imageryCovers=gatherLayer(state->imagery,2);
        gatherLayer(state->map,1);
        const size_t retainedLimit=globeTileBudget(state->toaster).retainedTiles;
        if(retainedTiles.size()>retainedLimit) {
            std::vector<std::pair<uint64_t,saida::AssetID>> idle;
            for(const auto& [id,used]:retainedTiles)if(used<tileFrame)idle.push_back({used,id});
            std::sort(idle.begin(),idle.end());
            for(const auto& [used,id]:idle) {
                if(retainedTiles.size()<=retainedLimit)break;
                resources->releaseTexture(id);retainedTiles.erase(id);tileInfo.erase(id);shown.erase(id);
            }
        }
    }
    // Every cached tile is retained -- so it loads ahead of the world streaming
    // behind the menu -- and so is whatever is on screen, whatever its level.
    void retain(saida::AssetID id) {
        auto [it,fresh]=retainedTiles.try_emplace(id,tileFrame);
        if(fresh)resources->retainTexture(id);else it->second=tileFrame;
    }
    bool gatherLayer(const GlobeLayer& layer,float mode) {
        // Prefetch also uploads while this layer is still hidden. Previously
        // the early z<0 return left downloaded arrival images sitting on disk.
        for(const auto& [tile,path]:layer.cached) {
            const auto key=path.generic_string();
            auto found=tileIds.find(key);
            // Clamped: a repeating sampler blends each edge with the opposite
            // one and draws a seam between neighbours.
            if(found==tileIds.end())found=tileIds.emplace(key,resources->getOrRegister(key,saida::AssetType::Texture,true,saida::rhi::AddressMode::ClampToEdge)).first;
            retain(found->second);
            tileInfo[found->second]={tile,found->second,mode};
        }
        if(layer.z<0)return false;
        std::vector<MapTile> resident;
        std::vector<RetainedTile> tiles;
        std::vector<saida::Texture*> gpu;
        std::set<std::tuple<int,int,int>> exact;
        // The map fades as a transparent overlay: overlapping parent/child
        // map tiles would apply that opacity twice. Imagery is opaque and can
        // safely reuse its entire resident pyramid underneath missing detail.
        for(const auto& [id,t]:tileInfo)if(t.mode==mode&&(mode!=1||t.tile.z==layer.z))
            if(auto* texture=resources->getTexture(id)) {
                resident.push_back(t.tile);tiles.push_back(t);gpu.push_back(texture);
                exact.insert({t.tile.z,t.tile.x,t.tile.y});
            }
        // Display each available tile now; one slow edge tile must not keep
        // the entire flight on the blurry base globe.
        const auto covering=globeTileCoverage(layer.visible,resident);
        if(mode==2)fadeImagery(covering,tiles,gpu);
        else for(auto i:covering) {
            retain(tiles[i].id);
            readyTiles.push_back({tiles[i].tile,gpu[i],mode});
        }
        return !layer.visible.empty()&&std::all_of(layer.visible.begin(),layer.visible.end(),
            [&](const auto& t){return exact.count({t.z,t.x,t.y})!=0;});
    }
    static bool overlaps(const MapTile& a,const MapTile& b) {
        const MapTile& fine=a.z>=b.z?a:b;const MapTile& coarse=a.z>=b.z?b:a;
        const int shift=fine.z-coarse.z;
        return (fine.x>>shift)==coarse.x&&(fine.y>>shift)==coarse.y;
    }
    void fadeImagery(const std::vector<size_t>& covering,const std::vector<RetainedTile>& tiles,
                     const std::vector<saida::Texture*>& gpu) {
        const double now=saida::Time::unscaledElapsed();
        // The arrival's fade already blends the globe into the world: a tile
        // fading there would show the world through it.
        const bool still=state->opacity>=1;
        std::vector<ReadyTile> current;std::vector<RetainedTile> drawn;
        std::set<saida::AssetID> present;
        for(auto i:covering) {
            retain(tiles[i].id);present.insert(tiles[i].id);
            auto& s=shown[tiles[i].id];
            if(!s.frame||s.frame+1<tileFrame)s.since=now;  // new, or back after a gap
            s.frame=tileFrame;
            const double u=std::clamp((now-s.since)/kImageryFadeSeconds,0.,1.);
            current.push_back({tiles[i].tile,gpu[i],2.f,still?float(u*u*(3-2*u)):1.f});
            drawn.push_back(tiles[i]);
        }
        // What was drawn and is being replaced stays beneath, opaque, while
        // a tile fading in over it has not finished.
        std::vector<ReadyTile> beneath;
        for(const auto& old:drawnImagery) {
            if(present.count(old.id))continue;
            const bool covered=std::any_of(current.begin(),current.end(),
                [&](const ReadyTile& t){return t.alpha<1&&overlaps(t.tile,old.tile);});
            if(!covered)continue;
            auto* texture=resources->getTexture(old.id);
            if(!texture)continue;
            retain(old.id);shown[old.id].frame=tileFrame;
            beneath.push_back({old.tile,texture,2.f,1.f});drawn.push_back(old);
        }
        std::sort(beneath.begin(),beneath.end(),[](const ReadyTile& a,const ReadyTile& b){return a.tile.z<b.tile.z;});
        readyTiles.insert(readyTiles.end(),beneath.begin(),beneath.end());
        readyTiles.insert(readyTiles.end(),current.begin(),current.end());
        drawnImagery=std::move(drawn);
    }
    void record(saida::FrameContext& fc) override {
        if(!state->visible||fc.stereo)return;
        for(auto*& texture:textures)if(!texture)texture=resources->defaultWhiteTexture();
        auto& sets=slots.at(fc.frameIndex).sets;sets.clear();
        auto bind=[&](saida::Texture* tile,const std::array<saida::Texture*,8>& neighbours=std::array<saida::Texture*,8>{}){
            std::vector<saida::rhi::BindGroupEntry> entries;
            for(uint32_t i=0;i<13;++i) {
                auto* texture=i<4?textures[i]:(i==4?tile:(neighbours[i-5]?neighbours[i-5]:tile));
                saida::rhi::BindGroupEntry entry;entry.binding=i;entry.view=texture->imageView();entry.sampler=texture->sampler();
                entries.push_back(entry);
            }
            sets.push_back(std::make_unique<saida::rhi::BindGroup>(*layout,entries));
            fc.pass.setBindGroup(0,*sets.back());
        };
        const float opacity=std::clamp(state->opacity,0.f,1.f);
        if(opacity<=0)return;
        const bool fading=opacity<1;
        const auto b=state->view.basis();
        Push push{glm::vec4(b[0],opacity),glm::vec4(b[1],std::clamp(state->vignette,0.f,1.f)),glm::vec4(b[2],0),state->viewport,
            {float(fc.extent.width)/fc.extent.height,float(state->view.altitude()),float(saida::Time::unscaledElapsed()),
             state->map.opacity},{0,0,0,ready?0.f:-1.f},
            glm::vec4(glm::vec3(GlobeView::direction(state->subsolar.lon,state->subsolar.lat)),0)};
        fc.pass.setPipeline(fading?*fadePipeline:*pipeline);
        if(!fading||!state->imageryCovers){bind(textures[0]);fc.pass.setPushConstants(&push,sizeof(push));fc.pass.draw(3);}
        const auto rect=state->viewport;
        const double width=fc.extent.width*rect.z,height=fc.extent.height*rect.w;
        std::map<std::tuple<float,int,int,int>,saida::Texture*> tileTextures;
        for(const auto& t:readyTiles)tileTextures[{t.mode,t.tile.z,t.tile.x,t.tile.y}]=t.texture;
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
            // Bilinear magnification straddles the tile edge by half a texel.
            // ClampToEdge alone repeats each tile's own last pixel there;
            // the two limits differ even for perfectly aligned source images.
            static constexpr int offsets[8][2]={{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{1,-1},{-1,1},{1,1}};
            std::array<saida::Texture*,8> neighbours{};
            unsigned mask=0;
            for(unsigned i=0;i<neighbours.size();++i) {
                const int tx=(tile.tile.x+offsets[i][0]+int(n))%int(n),ty=tile.tile.y+offsets[i][1];
                const auto it=tileTextures.find({tile.mode,tile.tile.z,tx,ty});
                if(it!=tileTextures.end()&&it->second->width()==tile.texture->width()&&it->second->height()==tile.texture->height()) {
                    neighbours[i]=it->second;mask|=1u<<i;
                }
            }
            push.outward.w=float(mask);
            bind(tile.texture,neighbours);
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
