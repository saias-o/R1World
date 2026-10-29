#include "session.hpp"

namespace r1::world {
void World::Impl::resetPerformanceSample() {
    performanceSeconds=0;performanceFrames=0;
    if(performanceDebug)performanceText("debug-fps","FPS: measuring…");
}

void World::Impl::performanceText(const std::string& id,const std::string& value) {
    if(!performanceUi)return;
    auto& previous=written["debug:"+id];
    if(previous!=value&&performanceUi->setElementText(id,value))previous=value;
}

void World::Impl::placePerformanceCanvas() {
    if(!performanceUi||!performanceDebug||menu)return;
    const glm::vec2 scale=ui->screenSize()/glm::vec2(ui->width(),ui->height());
    const glm::vec2 at=ui->screenPosition()+glm::vec2((1440.f-360.f-32.f)*scale.x,130.f*scale.y);
    performanceUi->transform().position={at.x,at.y,0};
    performanceUi->transform().scale={scale.x,scale.y,1};
}

void World::Impl::ensurePerformanceCanvas() {
    if(performanceUi)return;
    // A small independent texture avoids re-rendering the 1440x900 map/HUD
    // twice a second. It is not even allocated until debug is first opened.
    json doc={{"type","WebCanvasNode"},{"name","Performance debug"},{"width",360},{"height",218},
        {"mode",0},{"url",(game/"ui/performance.html").string()},{"hotReload",false},
        {"interactive",false},{"renderOrder",1002},{"enabled",false},
        {"transform",{{"position",{32,130,0}},{"scale",{1,1,1}},{"rotation",{1,0,0,0}}}}};
    auto canvas=saida::SceneSerializer::nodeFromJson(doc.dump(),engine.resources());
    performanceUi=dynamic_cast<saida::WebCanvasNode*>(canvas.get());
    if(!performanceUi||!performanceUi->lastLoadOk())throw std::runtime_error("Performance HUD failed to load");
    engine.sceneTree().world().addChild(std::move(canvas));
}

void World::Impl::performanceStatus() {
    performanceText("debug-traffic",std::string("V · Road traffic: ")+(debugTraffic?"ON":"OFF"));
    performanceText("debug-grass",std::string("Y · Volumetric grass: ")+(debugGrass?"ON":"OFF"));
    performanceText("debug-water",std::string("M · Water (WaterNode): ")+(debugWater?"ON":"OFF"));
    performanceText("debug-trees",std::string("T · Trees: ")+(debugTrees?"ON":"OFF"));
}

void World::Impl::applyPerformanceTile(Loaded& tile) {
    if(tile.grass)tile.grass->setEnabled(debugGrass);
    for(auto* water:tile.waters)water->setEnabled(debugWater);
    for(auto& plant:tile.vegetation)if(plant.tree)plant.node->setEnabled(debugTrees);
    if(tile.trunks)tile.trunks->setEnabled(debugTrees);
    for(size_t i=0;i<tile.cars.size();++i)
        if(tile.cars[i])tile.cars[i]->setEnabled(trafficSlotLive(tile,i));
}

void World::Impl::handlePerformanceKeys(const bool (&keys)[5]) {
    bool pressed[5];
    for(size_t i=0;i<5;++i){pressed[i]=keys[i]&&!performanceKeys[i];performanceKeys[i]=keys[i];}
    // Latch even in menus so typing, holding a key, or resuming cannot toggle.
    if(!playing||menu||playerMenu||photoMode||photoResult)return;
    bool changed=pressed[0];
    if(pressed[0]) {
        performanceDebug=!performanceDebug;
        if(performanceDebug)ensurePerformanceCanvas();
        if(performanceUi)performanceUi->setEnabled(performanceDebug);
        if(!performanceDebug)debugTraffic=debugGrass=debugWater=debugTrees=true;
    }
    if(performanceDebug) {
        if(pressed[1]){debugTraffic=!debugTraffic;changed=true;}
        if(pressed[2]){debugGrass=!debugGrass;changed=true;}
        if(pressed[3]){debugWater=!debugWater;changed=true;}
        if(pressed[4]){debugTrees=!debugTrees;changed=true;}
    }
    if(!changed)return;
    for(auto* tiles:{&loaded,&incoming})for(auto& [key,tile]:*tiles)applyPerformanceTile(tile);
    performanceStatus();resetPerformanceSample();
    saida::Log::info("[World debug] enabled=",performanceDebug," traffic=",debugTraffic,
                     " grass=",debugGrass," water=",debugWater," trees=",debugTrees);
}

void World::Impl::samplePerformance(double seconds) {
    if(!performanceDebug||!playing||menu){performanceSeconds=0;performanceFrames=0;return;}
    if(seconds<=0||!std::isfinite(seconds))return;
    performanceSeconds+=seconds;++performanceFrames;
    if(performanceSeconds<kPerformanceRefreshSeconds)return;
    // Whole-frame wall time includes GPU waits and pacing. Do not enable the
    // full profiler or copy its scopes/history just to display two numbers.
    performanceText("debug-fps",number(double(performanceFrames)/performanceSeconds,1)+" FPS · "+
                     number(performanceSeconds*1000./double(performanceFrames),2)+" ms/frame");
    performanceSeconds=0;performanceFrames=0;
}

void World::Impl::pressPerformanceKey(size_t key) {
    bool keys[5]{};handlePerformanceKeys(keys);
    keys[key]=true;handlePerformanceKeys(keys);handlePerformanceKeys(keys);
    keys[key]=false;handlePerformanceKeys(keys);
}

void World::Impl::runPerformanceSmoke(double dt) {
    if(captureQueued)return;
    performanceSmokeWait+=dt;
    auto fail=[&](const char* reason){
        saida::Log::error("[World debug E2E] FAIL ",reason);testFailed=true;engine.sceneTree().quit();
    };
    if(performanceSmokePhase==0) {
        if(performanceSmokeWait<2.)return;
        size_t trees=0,grass=0,water=0;
        for(const auto& [key,t]:loaded) {
            grass+=t.grass!=nullptr;water+=t.waters.size();
            for(const auto& plant:t.vegetation)trees+=plant.tree;
        }
        if((!trees||!grass||!trafficLive())&&performanceSmokeWait<15.)return;
        if(!trees||!grass||!trafficLive()){fail("fixture needs grass, trees and road traffic");return;}
        saida::Log::info("[World debug E2E] fixture trees=",trees," grass=",grass," water=",water);
        pressPerformanceKey(1);
        if(!debugTraffic||performanceDebug){fail("V acted outside debug");return;}
        pressPerformanceKey(0); // Held R is delivered twice by the helper.
        if(!performanceDebug){fail("R did not toggle once while held");return;}
        resetPerformanceSample();samplePerformance(.01);samplePerformance(.49);
        if(written["debug:debug-fps"]!="4.0 FPS · 250.00 ms/image") {
            fail("FPS did not count frames over elapsed wall time");return;
        }
        for(size_t key=1;key<5;++key)pressPerformanceKey(key);
        performanceSmokePhase=1;performanceSmokeWait=0;return;
    }
    if(performanceSmokePhase==1) {
        if(performanceSmokeWait<1.)return; // New props stream while switches are OFF.
        for(auto* tiles:{&loaded,&incoming})for(const auto& [key,t]:*tiles) {
            if(t.grass&&t.grass->enabled()){fail("grass stayed enabled");return;}
            for(auto* water:t.waters)if(water->enabled()){fail("water stayed enabled");return;}
            for(const auto& plant:t.vegetation)if(plant.tree&&plant.node->enabled()){fail("tree stayed enabled");return;}
            if(t.trunks&&(t.trunks->enabled()||t.trunks->physicsWorld())){fail("tree collisions stayed enabled");return;}
            for(auto* car:t.cars)if(car&&car->enabled()){fail("traffic car stayed enabled");return;}
            const auto before=t.flow.agents();
            updateTraffic(.05f);
            const auto& after=t.flow.agents();
            for(size_t i=0;i<before.size();++i)
                if(before[i].lane!=after[i].lane||before[i].s!=after[i].s||before[i].speed!=after[i].speed||
                   before[i].seed!=after[i].seed||before[i].alive!=after[i].alive) {
                    fail("traffic simulation continued while OFF");return;
                }
        }
        if(!engine.sceneTree().world().grassFields().empty()||!engine.sceneTree().world().waterNodes().empty()) {
            fail("disabled grass/water still published to renderer");return;
        }
        pressPerformanceKey(2);
        if(!debugGrass||debugTraffic||debugWater||debugTrees){fail("Y changed another switch");return;}
        pressPerformanceKey(0);
        if(performanceDebug||!debugGrass||!debugTraffic||!debugWater||!debugTrees||performanceUi->enabled()) {
            fail("leaving debug did not restore normal play");return;
        }
        performanceSmokePhase=2;performanceSmokeWait=0;return;
    }
    if(performanceSmokePhase==2) {
        if(performanceSmokeWait<1.)return;
        if(!trafficLive()||engine.sceneTree().world().grassFields().empty()){fail("features did not resume");return;}
        pressPerformanceKey(0);placePerformanceCanvas();
        performanceSmokePhase=3;performanceSmokeWait=0;return;
    }
    if(performanceSmokeWait<1.)return;
    if(!performanceUi->lastLoadOk()||written["debug:debug-fps"].find("ms/image")==std::string::npos) {
        fail("FPS HUD did not render its sample");return;
    }
    saida::Log::info("[World debug E2E] PASS held keys, independent toggles, streamed nodes, physics removal, traffic pause/resume, FPS averaging and HUD");
    if(const char* png=std::getenv("R1WORLD_DEBUG_SHOT")) {
        if(!captureQueued) {
            saida::CaptureRequest shot;shot.pngPath=png;shot.frame=15;
            captureQueued=true;engine.captureFrameThenExit(shot);
        }
    } else engine.sceneTree().quit();
}

} // namespace r1::world
