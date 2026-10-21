#include "menu_globe.hpp"
#include "menu_intro.hpp"
#include "Engine.hpp"
#include "core/Window.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/WebCanvasNode.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <cstdio>

// Real game shaders + real game RmlUi document, without loading the world.
// This capture harness makes visual iteration reproducible and inexpensive.
int main(int argc,char** argv) {
    if(argc<3)return 2;
    const auto project=std::filesystem::absolute(argv[1]);
    const bool toaster=argc>8&&std::string(argv[8])=="cache-test-toaster";
    const bool cacheTest=toaster||(argc>8&&std::string(argv[8])=="cache-test");
    auto globe=r1::registerMenuGlobe();
    globe->toaster=toaster;
    saida::Engine engine(nullptr,(project/"R1World.saidaproj").string());
    auto* canvas=engine.scene().createChild<saida::WebCanvasNode>();
    canvas->deserialize({{"type","WebCanvasNode"},{"name","Menu"},{"url","ui/world.html"},
        {"width",1440},{"height",900},{"referenceWidth",1440},{"referenceHeight",900},{"scaleMode",1},{"hotReload",false}},engine.resources());
    if(argc>3&&std::string(argv[3])!="main") {
        canvas->executeJS("document.getElementById('menu').style.display='none';document.getElementById('options-screen').style.display='block';showSettingsTab('"+std::string(argv[3])+"');");
    }
    if(argc>4)globe->view.zoom=std::stod(argv[4]);
    if(argc>6){globe->view.lon=std::stod(argv[5]);globe->view.lat=std::stod(argv[6]);}
    // Optional eighth argument freezes a subtitle time for visual inspection.
    if(argc>7) {
        const std::string subtitle(r1::MenuIntro::subtitleAt(std::stod(argv[7])));
        canvas->executeJS("document.getElementById('startup-subtitle').textContent="+nlohmann::json(subtitle).dump()+
            ";document.getElementById('startup-subtitle').style.display='"+(subtitle.empty()?"none":"block")+"';");
    }
    // Detail layers from whatever the game already cached for this view.
    {
        const int level=globe->view.tileLevel(900);
        auto cached=[&](const char* name,int z,int coarsest,const char* ext,r1::GlobeLayer& layer){
            layer.z=z;layer.coarsest=coarsest;layer.visible=globe->view.tiles(1440,900,z);
            for(int level=coarsest;level<=z;++level)
                for(const auto& t:globe->view.tiles(1440,900,level)) {
                    const auto p=project/"cache/map-tiles"/name/std::to_string(t.z)/std::to_string(t.x)/(std::to_string(t.y)+ext);
                    if(std::filesystem::exists(p))layer.cached.push_back({t,p});
                }
        };
        if(level>=r1::kImageryFirstLevel)cached("s2cloudless-2016",std::min(level,r1::kImageryLastLevel),r1::kImageryFirstLevel,".jpg",globe->imagery);
        // The arrival exposes imagery at street zoom, where the menu normally
        // covers it with OSM. Keep that view reproducible without world loading.
        const bool imageryOnly=cacheTest||(argc>8&&std::string(argv[8])=="imagery");
        if(globe->view.zoom>=r1::kOsmZoom&&!imageryOnly){cached("osm",level,std::max(0,level-6),".png",globe->map);globe->map.opacity=1;}
        std::printf("preview level %d: %zu imagery, %zu map tiles\n",level,globe->imagery.cached.size(),globe->map.cached.size());
    }
    std::vector<r1::GlobeTile> cached;
    saida::AssetID ids[2]{};
    saida::Texture* original[2]{};
    bool failed=false;
    int frame=0;
    if(cacheTest) {
        // Only local files: this executable has no tile downloader. Pick two
        // distinct cached images, one shown and one prefetched but not shown.
        cached=globe->imagery.cached;
        if(cached.size()<2){std::fprintf(stderr,"cache-test needs two cached imagery files\n");return 2;}
        cached={cached.front(),cached.back()};
        for(int i=0;i<2;++i)ids[i]=engine.resources().getOrRegister(cached[i].path.generic_string(),
            saida::AssetType::Texture,true,saida::rhi::AddressMode::ClampToEdge);
        globe->imagery={};globe->imagery.cached=cached;globe->map={};
    }
    engine.setOnFrame([&](float){
        int w=1,h=1;engine.window().size(w,h);
        auto origin=canvas->screenPosition(),size=canvas->screenSize();
        globe->viewport={origin.x/w,origin.y/h,size.x/w,size.y/h};
        if(!cacheTest)return;
        ++frame;
        auto check=[&](bool ok,const char* message){
            if(!ok){failed=true;std::fprintf(stderr,"[menu-cache] FAIL %s\n",message);engine.sceneTree().quit();}
            return ok;
        };
        if(frame==240) {
            // First texture lookup: if hidden prefetch did not upload, this
            // returns null instead of silently allowing the test to load it.
            for(int i=0;i<2;++i) {
                original[i]=engine.resources().getTexture(ids[i]);
                if(!check(original[i]&&original[i]!=engine.resources().missingTexture(),"hidden layer did not prefetch to GPU"))return;
            }
            globe->imagery.z=cached[0].tile.z;
            globe->imagery.visible={cached[0].tile};globe->imagery.cached={cached[0]};
        }
        if(frame==242) {
            if(!check(globe->imageryCovers,"cached view was not sharp on its first rendered frame"))return;
            globe->visible=false;
        }
        if(frame==248) {
            engine.resources().trimUnused({});
            for(int i=0;i<2;++i)
                if(!check(engine.resources().getTexture(ids[i])==original[i],"gameplay trim discarded recent imagery"))return;
            globe->visible=true;globe->imagery.z=cached[1].tile.z;
            globe->imagery.visible={cached[1].tile};globe->imagery.cached={cached[1]};
        }
        if(frame==250) {
            if(!check(globe->imageryCovers,"reopened view waited for a reload or a fade"))return;
            std::printf("[menu-cache] PASS hidden GPU prefetch, gameplay trim, immediate reopening; same GPU textures, local files only\n");
            saida::CaptureRequest shot;shot.pngPath=argv[2];shot.frame=2;shot.fixedStep=1.f/60.f;
            engine.captureFrameThenExit(shot);
        }
    });
    if(!cacheTest) {
        saida::CaptureRequest shot;shot.pngPath=argv[2];shot.frame=argc>6?600:90;shot.fixedStep=1.f/60.f;shot.settleTimeoutFrames=900;
        engine.captureFrameThenExit(shot);
    }
    engine.run();return failed||engine.captureFailed()?1:0;
}
