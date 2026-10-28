#include "session.hpp"

namespace r1::world {

World::World(saida::Engine& engine, std::filesystem::path game, bool smoke,
             saida::CaptureRequest capture, double startLon, double startLat,
             bool hop, double hopLon, double hopLat, saida::runtime::CaptureViewpoint view,
             bool sail, bool fly, bool headset)
    : impl_(std::make_unique<Impl>(engine, std::move(game), smoke, std::move(capture),
          startLon, startLat, hop, hopLon, hopLat, view, sail, fly, headset)) {}

World::~World() = default;
void World::update(float delta) { impl_->update(delta); }
bool World::needsAfterSimulation() const { return impl_->needsAfterSimulation(); }
void World::afterSimulation(float delta) { impl_->afterSimulation(delta); }
bool World::failed() const { return impl_->failed(); }
void World::setMenuGlobe(std::shared_ptr<r1::MenuGlobeState> model) { impl_->setMenuGlobe(std::move(model)); }
#ifdef SAIDA_ENABLE_XR
void World::setXrGestureInput(const saida::xrgesture::RuntimeInputState& state) { impl_->setXrGestureInput(state); }
#endif
void World::diveSmoke() { impl_->diveSmoke(); }
void World::inspectInstant(double unixSeconds) { impl_->inspectInstant(unixSeconds); }
void World::inspectConditions(std::array<double,3> conditions) { impl_->inspectConditions(conditions); }
void World::inspectHeat(double temperature) { impl_->inspectHeat(temperature); }
void World::captureFromAltitude(double metres) { impl_->captureFromAltitude(metres); }
void World::captureAtGeography(std::array<double,3> camera) { impl_->captureAtGeography(camera); }

} // namespace r1::world
