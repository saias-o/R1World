#pragma once
#include "geography.hpp"
#include "gen/crowd.hpp"
#include "police.hpp"
#include <cstdint>

namespace r1::world {
// Keep room for shared models, UI and the player within the configured arena.
constexpr double kTileGeometryShare = .85;
// The title keeps human rigs, vehicles and scanned vegetation alongside nine
// tiles. The engine's 256 MiB default decoder budget rejected existing models
// while the game used only 1.4-3 GiB of the reference machine's 7.4 GiB RAM.
constexpr uint64_t kDecodedAssetBudget=512ull*1024ull*1024ull;

// ── driving ─────────────────────────────────────────────────────────────────
//
// VehicleRuntime configures physical handling; these are interaction limits.
constexpr double kCarSteerAngle=34.;    // degrees of visible lock on the front wheels
constexpr double kCarReach=4.5;         // m -- how close you stand to open the door
constexpr double kSmokeBrakeSpeed=2.;  // The smoke driver also exercises a normal stop.
constexpr double kCarSinkRate=1.2;      // m/s after a car leaves the road for water
constexpr double kCarSinkDepth=4.5;     // keep the car below the surface until a teleport
constexpr double kCarKerb=.3;           // m above the ground a street surface is still driven onto
// The player's height is read from assets/models/humans/humans.json, drawn
// at its `scale` times kPeopleVehicleResize; the head stays this fraction
// of it clear of the water.
constexpr double kSwimHeadAbove=.33;
// He swims once this fraction of his height is under the water; below it he
// wades, walking on the bed.
constexpr double kSwimDepth=.5;
// Reduce people and road vehicles by 15% relative to their previous 0.8 scale.
// Keep the authored assets and clip timings intact; runtime dimensions follow
// the same resize so doors, cameras and collisions agree with the meshes.
constexpr double kPeopleVehicleResize=.85;
constexpr double kVehicleScale=.8*kPeopleVehicleResize;
// What a walker or a car climbs without noticing: a kerb, a ramp's next
// metre. A surface higher than this above him is a wall or a ceiling.
constexpr double kStepUp=1.2;
constexpr double kStreamAheadSeconds=45.; // prepare the road before the car reaches it
constexpr double kStreamAheadMinSpeed=4.2; // m/s; enable forward prefetch for moving vehicles
// The floating origin follows the player (PLAN §3 I1).
constexpr double kRebaseDistance=350.;
// Soft per-frame budgets, in milliseconds: soft because one mesh upload or one
// model import cannot be split.
constexpr double kPartUploadMs=3.,kPropImportMs=2.,kMountTickMs=4.;
constexpr double kBuildingNearLoad=75.,kBuildingNearRelease=85.;
constexpr size_t kStreamForecastTiles=3;
constexpr size_t kStreamRequestLimit=kPolarTiles+kStreamForecastTiles;
constexpr double kOnFootFollow=3.8,kDrivingFollow=8.5;
// Aiming, the camera closes in over the right shoulder: how far behind, and
// how far to the side.
constexpr double kAimFollow=1.9,kAimShoulder=.42;
// One message for the whole wait after Go: every change of the status line
// re-renders the interface, a quarter of a second on the map screen.
constexpr const char* kPreparing="Preparing the starting area… You can still change destination.";
// ── boats ───────────────────────────────────────────────────────────────────
// A boat is sailed the way the car is driven: a longitude, a latitude and a
// heading moved in the tangent plane, with one authority on position for the
// streamer, the origin and the Sun. Its handling -- top speed, acceleration,
// turn rate -- comes with it from the tile manifest (gen/harbours.cpp), because
// a container ship and a speedboat are not one boat with two paint jobs.
constexpr double kBoatReach=3.5;        // m from the hull's side to take the helm
// Ships at sea (gen/sea.cpp): how many the neighbourhood shows, how far
// away a new one may appear (never in plain view), and how far one sails on
// before it is dropped.
constexpr size_t kSeaShips=40;
constexpr double kSeaAppear=1200.;
constexpr double kSeaKeep=6500.;
constexpr double kHopSpeed=2.;          // m/s between two hulls to step across
constexpr double kBoatExitSpeed=1.5;    // m/s -- above it, leaving the hull is refused
constexpr size_t kLeftBoats=4;
// ── aircraft ────────────────────────────────────────────────────────────────
// Parked where the generator put them (gen/airports.cpp) and flown the way the
// car is driven: a longitude, a latitude, an altitude and three angles moved in
// the tangent plane. Each class handles as itself, from
// assets/models/aircraft/fleet.json; all of it is arcade and says so.
constexpr double kAircraftReach=4.;     // m from the fuselage's side to climb aboard
constexpr double kAircraftExitSpeed=2.; // m/s -- stopped: the smoke steps down, not out
constexpr size_t kLeftAircraft=4;
constexpr double kGravity=9.81;
// A body falling from a height, the player or an aircraft left in the air,
// falls no faster than a skydiver.
constexpr double kFallTerminal=55.;     // m/s
// How many traffic cars the whole neighbourhood may show at once. Not a memory
// budget -- every one of them is the same shared mesh (§5) -- but a draw-call
// one: a car is five primitives, so forty cars is two hundred draws, which is
// what the reference machine can spare beside a city.
constexpr size_t kTrafficCars=80;
// How many people the neighbourhood may show at once. Each is one skinned
// draw per material (two or three) and one animator: sixty is what the
// reference machine spends on a street without it showing in the frame time,
// and the animators far away pose at a lower rate (World::syncPeople).
constexpr size_t kCrowdPeople=60;
// How many cars the player may leave standing around before the oldest is
// cleared. They cost a node each and nothing else -- the mesh is shared -- but
// a city paved with the player's abandoned cars is its own kind of wrong.
constexpr size_t kAbandonedCars=6;
// Interiors (PLAN §8): built within kInteriorLoad metres of the door, released
// beyond kInteriorRelease, kInteriorRooms at most, the player's first. An
// entrance whose room is not built is a closed door within kClosedDoorNear,
// cleared beyond kClosedDoorFar.
constexpr double kInteriorLoad=65.,kInteriorRelease=85.,kClosedDoorNear=90.,kClosedDoorFar=95.;
// A tree shows its full model within this distance of the player, its middle
// level beyond. Forest trees stand about 12 m apart (gen/scatter.cpp: the
// inferred forest's spacing, the 11.6 m canopy cell), so a spacing and a
// quarter holds the nearest few: pi * 15^2 / 12^2 is about five.
constexpr double kTreeNearRadius=15.;
constexpr size_t kInteriorRooms=2;


// ── physics bodies ──────────────────────────────────────────────────────────
//
// The physics world reserves its body table once, when it is built
// (saida::PhysicsCapacity), like the geometry arena (§5): its capacity is the
// sum of what the world can hold at once, not a number picked, and a body past
// it is refused out loud (World::checkPhysics). A compound is one body however
// many shapes it holds, which is why a tile's trunks are one body.
//
// A tile's static bodies: one per uploaded mesh part, one per landmark, one
// for all its trunks, and the doors of the unloaded interiors near the player.
// The densest tile measured, by the Hôtel de Ville (v25_27771_23995,
// 2026-10-01), holds 835; World::sayBodies logs the densest tile against this
// budget. The nine tiles round it hold 5 626 trunks, which used to be a body
// each.
constexpr uint32_t kTileStaticBodies=1536;
// Its moving ones: a traffic slot keeps its car's box once used, a crowd slot
// its person's capsule, each at most the whole neighbourhood's share.
// The animals a tile pools (World::readFauna), a box each: two pigeon flocks
// of up to 16 and the birds and beasts around them.
constexpr size_t kFaunaSlots=48;
constexpr uint32_t kTileMovingBodies=uint32_t(kTrafficCars+kCrowdPeople+r1::Crowd::kVehicleEscapeSlots+kFaunaSlots);
// The ring near a pole, plus the tile kept under the player through a teleport.
constexpr uint32_t kResidentTiles=uint32_t(kPolarTiles)+1;
// Two open interiors of at most 128 fixtures each (gen/interior_uses.cpp) with
// their walls, door leaves, tills and forecourt cars.
constexpr uint32_t kInteriorBodies=2*192;
// Feet, car, abandoned cars, flown aircraft and police air support. Boats
// still carry no collider; rockets use queries rather than solver bodies.
constexpr uint32_t kPlayerBodies=4+uint32_t(kAbandonedCars+kLeftAircraft);
// Bodies thrown down at once (World::knockDown): past this, the oldest still
// tumbling is frozen where it lies -- never refused. Eleven bodies each
// (r1::rocketboxRagdoll), and the player's own.
constexpr size_t kLiveRagdolls=8;
constexpr uint32_t kCombatBodies=uint32_t(kLiveRagdolls+1)*11;
constexpr size_t kMountedAnimalRemains=32, kSimulatingAnimalRemains=8;
constexpr size_t kAnimalRagdollParts=64;
constexpr double kAnimalRemainsReach=2000.;
constexpr uint32_t kAnimalRemainsBodies=uint32_t(kMountedAnimalRemains+kSimulatingAnimalRemains*kAnimalRagdollParts);
constexpr uint32_t kPhysicsBodies=kAnimalRemainsBodies+kResidentTiles*(kTileStaticBodies+kTileMovingBodies)+kInteriorBodies+kPlayerBodies+kCombatBodies+uint32_t(r1::police::kCars+r1::police::kOfficers);

} // namespace r1::world
