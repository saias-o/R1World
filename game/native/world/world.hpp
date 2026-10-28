#pragma once

#include <array>
#include <filesystem>
#include <memory>

namespace saida {
class Engine;
struct CaptureRequest;
namespace runtime { struct CaptureViewpoint; }
namespace xrgesture { struct RuntimeInputState; }
}
namespace r1 { struct MenuGlobeState; }

namespace r1::world {

// Application-facing session. RmlUi, scene actors and streaming state remain
// private; the session must be destroyed before the Engine it uses.
class World {
public:
    World(saida::Engine& engine, std::filesystem::path game, bool smoke,
          saida::CaptureRequest capture, double startLon, double startLat,
          bool hop, double hopLon, double hopLat, saida::runtime::CaptureViewpoint view,
          bool sail=false, bool fly=false, bool headset=false);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;

    void update(float delta);
    bool needsAfterSimulation() const;
    void afterSimulation(float delta);
    bool failed() const;
    void setMenuGlobe(std::shared_ptr<r1::MenuGlobeState> model);
#ifdef SAIDA_ENABLE_XR
    void setXrGestureInput(const saida::xrgesture::RuntimeInputState& state);
#endif
    void diveSmoke();
    void inspectInstant(double unixSeconds);
    void inspectConditions(std::array<double,3> conditions);
    void inspectHeat(double temperature);
    void captureFromAltitude(double metres);
    void captureAtGeography(std::array<double,3> camera);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r1::world
