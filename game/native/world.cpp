// Game-side integration with Saida's public C++ API.
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include "Engine.hpp"
#include "core/Paths.hpp"
#include "core/Time.hpp"
#include "core/Input.hpp"
#include "core/Window.hpp"
#include "core/Log.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneTree.hpp"
#include "scene/SceneSerializer.hpp"
#include "scene/GLTFLoader.hpp"
#include "core/Profiler.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipView.hpp"
#include "graphics/ResourceManager.hpp"
#include "graphics/MemoryProfiler.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "nodes/GrassNode.hpp"
#include "nodes/WaterNode.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "nodes/TerrainRingsNode.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "behaviours/BoneAttachmentBehaviour.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "nodes/WebCanvasNode.hpp"
#include "scripting/ScriptBehaviour.hpp"
#include "runtime/CaptureArgs.hpp"
#include "runtime/ProfileArgs.hpp"
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include "saida/traffic/Traffic.hpp"
#include "vehicle_runtime.hpp"
#include "motorcycle.hpp"
#include "vehicle_radio.hpp"
#include "traffic_signal.hpp"
#include "gen/traffic_format.hpp"
#include "gen/roadside.hpp"
#include "gen/crowd.hpp"
#include "gen/beach.hpp"
#include "gen/appearance.hpp"
#include "gen/landmarks.hpp"
#include "gen/palette.hpp"
#include "gen/places.hpp"
#include "gen/map_tiles.hpp"
#include "gen/interiors.hpp"
#include "gen/predict.hpp"
#include "gen/net.hpp"
#include "gen/sea.hpp"
#include "gen/far_relief.hpp"
#include "gen/sources.hpp"
#include "gen/service.hpp"
#include "gen/terrain.hpp"
#include "minimap.hpp"
#include "menu_globe.hpp"
#include <RmlUi/Core/Context.h>
#include "weather.hpp"
#include "forced_time.hpp"
#include "density_policy.hpp"
#include "combat.hpp"
#include "combat_kit.hpp"
#include "cheats.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <future>
#include <mutex>
#include <thread>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;
double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
constexpr double rad = 3.141592653589793 / 180.;
constexpr double kTwoPi = 6.283185307179586;
double wrap(double x) { return x - 360.*std::floor((x+180.)/360.); }
// The tile grid is the generator's (gen/common.hpp), never a copy of it: a
// second copy rounded a row boundary differently and put the player on a
// tile the generator had not cooked under him.
using r1::columns;
constexpr int kLastRow=r1::kRows-1;
// Metres in a degree of the equator: the spherical shortcut local estimates use.
constexpr double kMetresPerDegree=r1::kMetresPerDegree;
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
// §5: "on ne voit pas les poignées de porte à 130 km/h". 15 km/h is where
// the plan's own table stops calling it walking.
constexpr double kFastDetail=4.2;       // m/s
// What a walker or a car climbs without noticing: a kerb, a ramp's next
// metre. A surface higher than this above him is a wall or a ceiling.
constexpr double kStepUp=1.2;
constexpr double kStreamAheadSeconds=45.; // prepare the road before the car reaches it
// The floating origin follows the player (PLAN §3 I1).
constexpr double kRebaseDistance=350.;
// Soft per-frame budgets, in milliseconds: soft because one mesh upload or one
// model import cannot be split.
constexpr double kPartUploadMs=3.,kPropImportMs=2.,kMountTickMs=4.;
constexpr double kBuildingNearLoad=75.,kBuildingNearRelease=85.;
constexpr size_t kStreamRequestLimit=25; // the world service's bounded priority list
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

struct Tile {
    int r,c;
    // The generator's own key (gen/common.hpp), so the tile the game streams
    // and the tile the generator cooks cannot disagree about their version.
    std::string key() const { return r1::Tile{r,c}.key(); }
    r1::Tile gen() const { return {r,c}; }
    bool operator<(const Tile& b) const { return std::tie(r,c)<std::tie(b.r,b.c); }
    bool operator==(const Tile& b) const { return r==b.r&&c==b.c; }
};
Tile tileAt(double lon,double lat) {
    const r1::Tile t=r1::tileAt(lon,lat);
    return {t.row,t.col};
}
// Metres along the sphere between two points.
double metresBetween(double lon1,double lat1,double lon2,double lat2) {
    const double p1=lat1*rad,p2=lat2*rad,dp=p2-p1,dl=wrap(lon2-lon1)*rad;
    const double h=std::sin(dp/2)*std::sin(dp/2)+std::cos(p1)*std::cos(p2)*std::sin(dl/2)*std::sin(dl/2);
    return 2*r1::kRMean*std::asin(std::min(1.,std::sqrt(h)));
}
// Within some 18 km of a pole a ring is a few wedges, and the three rows
// around the player no longer hold what he sees: standing on the pole, the
// ground a hundred metres away across it is in a column no row offset
// reaches. There the neighbourhood is every tile whose nearest point is
// within reach, in metres, nearest first -- twelve at the pole itself.
constexpr int kPolarColumns=200;
constexpr double kPolarReach=650.;
constexpr size_t kPolarTiles=12;

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
constexpr uint32_t kTileMovingBodies=uint32_t(kTrafficCars+kCrowdPeople);
// The ring near a pole, plus the tile kept under the player through a teleport.
constexpr uint32_t kResidentTiles=uint32_t(kPolarTiles)+1;
// Two open interiors of at most 128 fixtures each (gen/interior_uses.cpp) with
// their walls, door leaves, tills and forecourt cars.
constexpr uint32_t kInteriorBodies=2*192;
// The player's feet and car, and the cars left standing (boats and aircraft
// carry no collider).
constexpr uint32_t kPlayerBodies=2+uint32_t(kAbandonedCars);
// Bodies thrown down at once (World::knockDown): past this, the oldest still
// tumbling is frozen where it lies -- never refused. Eleven bodies each
// (r1::rocketboxRagdoll), and the player's own.
constexpr size_t kLiveRagdolls=8;
// Seconds a body may tumble before it is frozen where it lies.
constexpr double kLongestTumble=6.;
constexpr uint32_t kCombatBodies=uint32_t(kLiveRagdolls+1)*11;
constexpr uint32_t kPhysicsBodies=kResidentTiles*(kTileStaticBodies+kTileMovingBodies)+kInteriorBodies+kPlayerBodies+kCombatBodies;
std::vector<Tile> polarNearby(double lon,double lat) {
    const Tile center=tileAt(lon,lat);
    std::vector<std::pair<double,Tile>> found;
    for(int r=std::max(0,center.r-2);r<=std::min(kLastRow,center.r+2);++r) {
        const int n=columns(r);
        for(int c=0;c<n;++c) {
            const Tile t{r,c};
            const r1::Bounds b=t.gen().bounds();
            const double la=std::clamp(lat,b.south,b.north);
            double lo=wrap(lon);
            if(lo<b.west||lo>b.east)lo=std::abs(wrap(lo-b.west))<std::abs(wrap(lo-b.east))?b.west:b.east;
            const double d=t==center?-1.:metresBetween(lon,lat,lo,la);
            if(d<=kPolarReach)found.push_back({d,t});
        }
    }
    std::stable_sort(found.begin(),found.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<Tile> out;
    for(const auto& f:found)if(out.size()<kPolarTiles)out.push_back(f.second);
    return out;
}
std::vector<Tile> nearby(double lon,double lat) {
    Tile center=tileAt(lon,lat);
    if(columns(center.r)<kPolarColumns)return polarNearby(lon,lat);
    std::vector<Tile> out{center}; std::set<Tile> seen{center};
    for(int dr=-1;dr<=1;++dr) {
        int r=std::clamp(center.r+dr,0,kLastRow),n=columns(r);
        int c=int(std::floor((wrap(lon)+180.)/360.*n));
        for(int dc=-1;dc<=1;++dc) {
            Tile t{r,(c+dc+n)%n}; if(seen.insert(t).second)out.push_back(t);
        }
    }
    // Center first, then the closest terrain: the next boundary matters more
    // than the arbitrary row/column iteration order. Wrap at the date line.
    auto distance=[&](Tile t) {
        const r1::P2 middle=t.gen().center();
        return std::hypot(wrap(middle.x-lon)*std::cos(lat*rad),middle.y-lat);
    };
    std::stable_sort(out.begin()+1,out.end(),[&](Tile a,Tile b){return distance(a)<distance(b);});
    return out;
}
glm::dvec3 ecef(double lon,double lat,double alt=0) {
    const r1::P3 p=r1::geodeticToEcef(lon,lat,alt);
    return {p.x,p.y,p.z};
}
struct Frame {
    glm::dvec3 origin; glm::dmat3 basis;
    Frame(double lon=0,double lat=0,double alt=0):origin(ecef(lon,lat,alt)) {
        double l=lon*rad,p=lat*rad;
        basis=glm::dmat3(glm::dvec3(-sin(l),cos(l),0),
                        glm::dvec3(cos(p)*cos(l),cos(p)*sin(l),sin(p)),
                        glm::dvec3(sin(p)*cos(l),sin(p)*sin(l),-cos(p)));
    }
    glm::dvec3 local(glm::dvec3 p) const { return glm::transpose(basis)*(p-origin); }
};
// Great-circle step; longitude wraps across the date line. Written as a
// rotation of the unit vector rather than through asin: at the pole asin
// sees cos(d) round to exactly 1 for a step of a few centimetres, and the
// player could never walk off it. atan2 keeps every step, there as anywhere.
glm::dvec2 advance(double lon,double lat,double east,double north) {
    const double length=std::hypot(east,north),d=length/r1::kRMean;
    if(d==0)return {lon,lat};
    const double l=lon*rad,p=lat*rad;
    const glm::dvec3 here(std::cos(p)*std::cos(l),std::cos(p)*std::sin(l),std::sin(p));
    const glm::dvec3 e(-std::sin(l),std::cos(l),0.),n(-std::sin(p)*std::cos(l),-std::sin(p)*std::sin(l),std::cos(p));
    const glm::dvec3 q=here*std::cos(d)+(e*east+n*north)*(std::sin(d)/length);
    return {wrap(std::atan2(q.y,q.x)/rad),std::atan2(q.z,std::hypot(q.x,q.y))/rad};
}
struct Plant {
    saida::Node* node=nullptr;
    bool grass=false;
    bool tree=false;
    // A tree with a middle level: its full model is allowed only close by.
    saida::LODGroupBehaviour* lod=nullptr;
    bool close=false;
};
struct Footprint {
    std::vector<glm::dvec2> points;
    glm::dvec2 low{1e30},high{-1e30};
    double top=1e30;  // the highest point over it, in its tile's frame: what an aircraft clears
    int interior=-1;
};
struct LiveInterior {
    r1::InteriorPlan plan;
    r1::InteriorLayout layout;
    saida::Node* node=nullptr;
    saida::Node* leaves[2]{nullptr,nullptr};
    saida::Node* closedDoor=nullptr;
    r1::P2 low{1e30,1e30},high{-1e30,-1e30};
    double opening=0,hold=0;
    bool refused=false,ready=false;
    bool contains(r1::P2 at) const {
        return at.x>=low.x&&at.x<=high.x&&at.y>=low.y&&at.y<=high.y&&r1::pointInPolygon(at,plan.ring);
    }
};
struct StoreParking { std::string name;std::vector<r1::Ring> rings; };
// A walkable deck over the water -- a pier -- in its tile's frame.
struct Deck {
    std::vector<glm::dvec2> points;
    glm::dvec2 low{1e30},high{-1e30};
    double y=0;
};
// A piece of road off the ground (gen/bridges.hpp), in its tile's frame: the
// axis a -> b at the road surface, the half width it carries, and whether it
// is an embankment (solid down to the ground, its slopes walkable) or a deck
// (passed under).
struct RaisedPiece {
    glm::dvec2 a{0},b{0}; double ya=0,yb=0; bool solid=false;
    double half[2]{0,0}; bool rail[2]{false,false};  // right of a -> b, then left
    glm::dvec2 low{1e30},high{-1e30};
};
// A moored boat as the manifest lists it; `node` is found once it has streamed.
struct Mooring {
    std::string name,kind,model;
    glm::dvec3 local{0};
    double heading=0,length=8,beam=3,top=10,accel=2,turn=30;
    saida::Node* node=nullptr;
    bool taken=false;
};
// An aircraft parked by the generator, as the manifest lists it.
struct AircraftSpot {
    std::string name,type;
    glm::dvec3 local{0};
    double heading=0;
    saida::Node* node=nullptr;
    bool taken=false;
};
// A cooked part as the GPU takes it, made on the worker that cooked it
// (WorldService's `prepare`), so the frame only uploads.
struct PartUpload {
    std::string name;
    std::vector<saida::Vertex> vertices;
    std::vector<uint32_t> indices;
    size_t material=0;  // index into the cooked tile's parts
    std::shared_ptr<const saida::PreparedMesh> prepared;
};
// Everything the frame needs of a cooked tile, made on the worker that cooked
// it: the parts as the GPU takes them, and building metadata for inspection.
struct PreparedTile {
    std::vector<PartUpload> parts;
    std::vector<Footprint> footprints;
    struct Building {
        std::array<std::vector<PartUpload>,3> levels;
        std::vector<PartUpload> reducedNear;
    };
    std::vector<Building> buildings;
    std::vector<std::shared_ptr<const saida::MeshCollisionData>> buildingCollisions;
};

// A generator part in the engine's vertex format.
PartUpload uploadOf(const r1::MeshPart& part,size_t index) {
    const r1::Mesh& m=part.mesh;
    const double k=part.material.uvScale;
    const auto tangents=r1::tangents(m,k);
    PartUpload up{part.name,{},m.indices,index};
    up.vertices.resize(m.positions.size());
    for(size_t v=0;v<m.positions.size();++v) {
        saida::Vertex& x=up.vertices[v];
        x.pos=glm::vec3(m.positions[v].x,m.positions[v].y,m.positions[v].z);
        x.normal=glm::vec3(m.normals[v].x,m.normals[v].y,m.normals[v].z);
        x.color=m.colors.empty()?glm::vec3(1.f):glm::vec3(m.colors[v][0],m.colors[v][1],m.colors[v][2]);
        x.texCoord=glm::vec2(m.texcoords[v].u*k,m.texcoords[v].v*k);
        x.tangent=glm::vec4(tangents[v][0],tangents[v][1],tangents[v][2],tangents[v][3]);
    }
    return up;
}

// Once a tile is prepared, the generator's own copy of its geometry -- double
// precision, with the weld keys of the ground and the streets -- is read for
// nothing: parts keep their names and materials, buildings their ids and
// bounds, and everything drawn or collided with is the prepared copy. It was
// some 68 of a Paris tile's 140 MB, held as long as the service kept the tile
// (measured 16 October 2026: 9 tiles mounted, 2.9 GB of heap).
void releaseCookedGeometry(r1::CookedTile& tile) {
    auto release=[](std::vector<r1::MeshPart>& parts){for(auto& part:parts)part.mesh=r1::Mesh(part.mesh.uvMode);};
    release(tile.parts);release(tile.buildingCollisions);
    for(auto& building:tile.buildings){for(auto& level:building.levels)release(level);release(building.reducedNear);}
}
// A prepared part back in the generator's form, for a GLB written when a
// smoke fails: the generator's own copy is gone by then.
r1::MeshPart generatorPart(const std::string& name,const std::vector<saida::Vertex>& vertices,const std::vector<uint32_t>& indices) {
    r1::MeshPart part;part.name=name;
    for(const auto& v:vertices) {
        part.mesh.positions.push_back({v.pos.x,v.pos.y,v.pos.z});
        part.mesh.normals.push_back({v.normal.x,v.normal.y,v.normal.z});
        part.mesh.texcoords.push_back({v.texCoord.x,v.texCoord.y});
    }
    part.mesh.indices=indices;
    return part;
}

PreparedTile prepareTile(const r1::CookedTile& tile) {
    PreparedTile prepared;
    auto& out=prepared.parts;
    out.reserve(tile.parts.size());
    for(size_t i=0;i<tile.parts.size();++i) {
        auto part=uploadOf(tile.parts[i],i);
        part.prepared=saida::prepareMesh({std::move(part.vertices),std::move(part.indices)});
        out.push_back(std::move(part));
    }
    auto prepareParts=[](const std::vector<r1::MeshPart>& parts) {
        std::vector<PartUpload> result;
        for(size_t i=0;i<parts.size();++i) {
            auto part=uploadOf(parts[i],i);
            part.prepared=saida::prepareMesh({std::move(part.vertices),std::move(part.indices)});
            result.push_back(std::move(part));
        }
        return result;
    };
    for(const auto& building:tile.buildings) {
        PreparedTile::Building b;
        for(size_t level=0;level<3;++level)b.levels[level]=prepareParts(building.levels[level]);
        b.reducedNear=prepareParts(building.reducedNear);
        prepared.buildings.push_back(std::move(b));
    }
    for(const auto& part:tile.buildingCollisions) {
        auto data=std::make_shared<saida::MeshCollisionData>();
        data->indices=part.mesh.indices;
        for(const auto& p:part.mesh.positions)data->positions.push_back({float(p.x),float(p.y),float(p.z)});
        prepared.buildingCollisions.push_back(std::move(data));
    }
    const json& tops=tile.manifest.contains("footprintTops")?tile.manifest.at("footprintTops"):json::array();
    for(const auto& polygon:tile.manifest.at("footprints")) {
        Footprint shape;
        if(prepared.footprints.size()<tops.size())shape.top=tops[prepared.footprints.size()].get<double>();
        for(const auto& point:polygon) {
            const glm::dvec2 p{point[0].get<double>(),point[1].get<double>()};
            shape.points.push_back(p);shape.low=glm::min(shape.low,p);shape.high=glm::max(shape.high,p);
        }
        prepared.footprints.push_back(std::move(shape));
    }
    return prepared;
}

// One of the crowd, as drawn: a pooled node with its own animator, the
// shared meshes of one avatar under it (World::makePerson). Over the clip,
// the engine's two modifiers: the head turned toward the player, and the
// spine's stagger when he walks into them.
struct Person {
    saida::Node* node=nullptr; saida::Animator* animator=nullptr;
    saida::RigidBodyNode* body=nullptr;  // their capsule in the engine's physics
    saida::GazeModifier* gaze=nullptr; saida::ImpactModifier* impact=nullptr;
    const char* clip=""; float poseRate=-1.f;
    uint32_t bumps=0;            // the walker's bumps already staggered
    double yaw=0; bool shown=false;  // the heading drawn, eased toward the walker's
    size_t kind=0;
    // A fight (World::knockDown, World::personAttack): the engine's ragdoll
    // that takes the body when it falls, the arms that punch, and the clock
    // of the punch thrown at the player and of the time spent lying still.
    saida::RagdollBehaviour* ragdoll=nullptr;
    saida::TwoBoneIKModifier* arms[2]{nullptr,nullptr};  // left, right
    uint32_t attacks=0; double punch=-1; int punchHand=1; bool struck=false;
    double lying=0;
};

struct BeachVisitor {
    Person person;
    saida::Node* furniture=nullptr;
    int spot=-1;
    double age=0;
};
struct BeachSpot { r1::P3 towel,water; bool parasol=false; };

struct Loaded {
    saida::Node* node=nullptr;
    // The cooked tile this was mounted from. Its manifest and its prop list
    // are read where they are, never copied.
    std::shared_ptr<const r1::ServedTile> served;
    const json& data; Frame frame; const json& props; size_t nextProp=0,nextLettering=0;
    // The tile's own geometry goes up a few parts a frame (World::uploadParts).
    saida::Node* geography=nullptr; size_t nextPart=0;
    std::vector<saida::Mesh*> partMeshes;
    struct Building {
        saida::Node* node=nullptr;
        saida::LODGroupBehaviour* lod=nullptr;
        struct Level {
            saida::Node* root=nullptr;
            std::vector<saida::Mesh*> meshes;
            size_t nextPart=0;
            bool started=false,refused=false,failed=false;
        };
        std::array<Level,3> levels;
        bool close=false;
    };
    std::vector<Building> buildings;
    std::vector<int> visualForFootprint;
    saida::GrassNode* grass=nullptr;  // its blades, when its ground grows any
    std::vector<saida::WaterNode*> waters;
    bool reducedDensity=false;
    std::chrono::steady_clock::time_point builtAt=std::chrono::steady_clock::now();
    Loaded(saida::Node* n,std::shared_ptr<const r1::ServedTile> s)
        :node(n),served(std::move(s)),data(served->cooked.manifest),
         frame(data.at("lon").get<double>(),data.at("lat").get<double>()),props(served->cooked.props){}
    std::vector<Footprint> footprints; std::vector<Plant> vegetation;
    // Every trunk of the tile is one static body, a compound of one box per
    // tree: a body per trunk spent up to 640 of the world's bodies on one tile
    // of measured canopy, and nine Paris tiles ran it out. A trunk joins it
    // when its tree is mounted (World::streamProps), never before: a tree
    // still waiting to stream -- all of them while driving -- was a wall
    // nobody could see.
    saida::StaticBodyNode* trunks=nullptr;
    std::vector<LiveInterior> interiors;
    std::vector<StoreParking> parking;
    // This tile's road network and the cars on it. The graph must not move
    // once the flow points at it, which is why both live here rather than in
    // a side table: std::map never relocates a node it has already made.
    saida::traffic::Graph graph; saida::traffic::Flow flow;
    std::vector<float> groundUp;          // terrain height per graph node
    std::vector<saida::Node*> cars;       // keyed by agent index, pooled
    std::vector<size_t> carKinds;
    std::vector<std::vector<saida::Node*>> carWheels;
    std::vector<double> carSpin;
    std::vector<uint64_t> carIdentities;
    struct SignalLamp {
        saida::MeshNode* node=nullptr;
        uint32_t program=0,group=0,colour=0;
        bool lit=false;
    };
    std::vector<SignalLamp> signalLamps;
    std::vector<glm::vec2> signalPositions;
    size_t wanted=0;                      // traffic population this tile's density asks for
    double motorcycleChance=.05;
    // The people on its pavements (gen/crowd.hpp): the same shape as the
    // traffic, for the same reason -- the crowd points at the graph.
    r1::WalkGraph walks; r1::Crowd crowd;
    double crowdEyeX=1e9,crowdEyeZ=1e9,crowdLocalPopulation=0;
    std::vector<BeachSpot> beachSpots;
    std::vector<BeachVisitor> beachVisitors;
    std::vector<Person> people;           // keyed by walker slot, pooled
    std::vector<size_t> humanKinds;       // stable avatar per slot in this country
    // The manifest unpacked once at mount, because a frame cannot afford to
    // read JSON. See World::unpack.
    double west=0,east=0,south=0,north=0;
    int gridSize=0; std::vector<float> elevation;
    r1::ElevationGrid groundGrid;
    // 0 land, 1 inland water, 2 sea-level water (see r1/harbours.Cells).
    int waterRows=0,waterCols=0; std::vector<uint8_t> water;
    bool ocean=false,seaIce=false;
    std::vector<Deck> decks; glm::dvec2 deckLow{1e30},deckHigh{-1e30};
    // The streets through water cells (gen/cook.cpp dryStreets): rings, even-odd.
    std::vector<Deck> dryStreets; glm::dvec2 dryLow{1e30},dryHigh{-1e30};
    std::vector<RaisedPiece> raised; glm::dvec2 raisedLow{1e30},raisedHigh{-1e30};
    std::vector<Mooring> boats;
    std::vector<AircraftSpot> aircraft;
    // Snow lying on the ground and the roofs (World::updateSnowCover): which
    // parts wear it, and what each wore before.
    bool snowed=false; size_t snowSeen=0;
    std::vector<std::pair<saida::MeshNode*,saida::Material*>> bare;
};

// Interactive map tiles are requested only when their pixels are on screen.
// A completed PNG is cached indefinitely, satisfying the tile server's
// minimum seven-day cache rule without conditional re-downloads.
// One remote tile layer of the menu globe, cached on disk for good: a place
// already looked at never touches the network again.
class MenuTileCache {
public:
    struct Source {
        std::string name;      // cache folder, and what the log calls it
        std::string extension; // ".png" / ".jpg", checked by signature
        std::function<std::string(const r1::MapTile&)> url;
        size_t parallel=1;     // the provider's connection policy
    };
private:
    // `refused`: the server answered without an image for that tile, which no
    // retry will change; anything else is the network, retried after a pause.
    struct Result { bool ready=false,refused=false; std::string error; };
    Source source;
    fs::path root;
    std::vector<r1::MapTile> visibleTiles;
    std::vector<std::pair<std::string,std::future<Result>>> flights;
    std::unordered_set<std::string> present,absent,checkedAbsent;
    std::chrono::steady_clock::time_point retryAfter{};
    std::string lastError;
    static std::string key(const r1::MapTile& t){return std::to_string(t.z)+"/"+std::to_string(t.x)+"/"+std::to_string(t.y);}
    // Either format, whatever the extension: EOX answers a plain area (open
    // sea, no data) with a tiny PNG. The decoder reads the signature.
    static bool validImage(const std::string& body) {
        return (body.size()>8&&std::memcmp(body.data(),"\x89PNG\r\n\x1a\n",8)==0)||
               (body.size()>3&&std::memcmp(body.data(),"\xff\xd8\xff",3)==0);
    }
public:
    MenuTileCache(fs::path project,Source s):source(std::move(s)),root(std::move(project)/"cache/map-tiles"/source.name){}
    fs::path path(const r1::MapTile& tile) const {
        return root/std::to_string(tile.z)/std::to_string(tile.x)/(std::to_string(tile.y)+source.extension);
    }
    // Asked every frame for every visible tile and its ancestors: the disk is
    // consulted once per tile, then remembered.
    bool ready(const r1::MapTile& tile) {
        const auto k=key(tile);
        if(present.count(k))return true;
        std::error_code ec;const auto p=path(tile);
        if(!fs::is_regular_file(p,ec)||fs::file_size(p,ec)<=8)return false;
        present.insert(k);return true;
    }
    // A tile the server has no image for: left out of what a view waits for,
    // and remembered on disk (an empty "<y>.none" beside where the image would
    // be) so no later session asks again.
    fs::path marker(const r1::MapTile& tile) const {return path(tile).string()+".none";}
    bool missingForGood(const r1::MapTile& tile) {
        const auto k=key(tile);
        if(absent.count(k))return true;
        if(!checkedAbsent.insert(k).second)return false;
        std::error_code ec;
        if(!fs::exists(marker(tile),ec))return false;
        absent.insert(k);return true;
    }
    void setVisible(std::vector<r1::MapTile> tiles){visibleTiles=std::move(tiles);}
    size_t missing() {
        return size_t(std::count_if(visibleTiles.begin(),visibleTiles.end(),[&](const auto& tile){return !ready(tile)&&!missingForGood(tile);}));
    }
    bool unavailable() const {return !lastError.empty()&&std::chrono::steady_clock::now()<retryAfter;}
    bool pump() {
        const auto now=std::chrono::steady_clock::now();
        bool changed=false;
        for(auto it=flights.begin();it!=flights.end();) {
            if(it->second.wait_for(std::chrono::seconds(0))!=std::future_status::ready){++it;continue;}
            const Result result=it->second.get();
            if(result.ready){lastError.clear();present.insert(it->first);}
            else if(result.refused) {
                absent.insert(it->first);
                {std::error_code ec;fs::create_directories(root/it->first.substr(0,it->first.rfind('/')),ec);}
                std::ofstream(root/(it->first+source.extension+".none"));
                saida::Log::info("[World map] ",source.name," has no tile ",it->first,": ",result.error);
            } else {
                lastError=result.error;
                retryAfter=now+std::chrono::seconds(30);
                saida::Log::info("[World map] ",source.name," tiles unavailable: ",lastError);
            }
            changed=true;it=flights.erase(it);
        }
        if(now<retryAfter)return changed;
        for(const auto& tile:visibleTiles) {
            if(flights.size()>=source.parallel)break;
            const auto k=key(tile);
            if(ready(tile)||missingForGood(tile)||std::any_of(flights.begin(),flights.end(),[&](const auto& f){return f.first==k;}))continue;
            const fs::path target=path(tile);
            const std::string url=source.url(tile);
            flights.emplace_back(k,std::async(std::launch::async,[this,url,target]() -> Result {
                try {
                    const auto response=r1::net::request("GET",url,{}, {},10.);
                    if(response.status!=200||!validImage(response.body))
                        return {false,(response.status>=400&&response.status<500)||response.status==200,
                                "HTTP "+std::to_string(response.status)+", "+std::to_string(response.body.size())+" bytes, no image, from "+url};
                    fs::create_directories(target.parent_path());
                    const fs::path temporary=target.string()+".part";
                    {std::ofstream output(temporary,std::ios::binary);
                     output.write(response.body.data(),std::streamsize(response.body.size()));
                     if(!output)throw std::runtime_error("cannot cache map tile");}
                    fs::rename(temporary,target);
                    return {true,{}};
                } catch(const std::exception& e) {return {false,e.what()};}
            }));
        }
        return changed;
    }
    ~MenuTileCache(){for(auto& f:flights)if(f.second.valid())f.second.wait();}
};

class World : public Rml::EventListener {
    saida::Engine& engine; fs::path game;
    r1::DensityPolicy densityPolicy{game};
    r1::VehicleRadio radio{game};
    bool radioKeys[4]{};
    void updateRadio(double dt) {
        const bool keys[]={held(GLFW_KEY_B),held(GLFW_KEY_N),held(GLFW_KEY_PAGE_UP),held(GLFW_KEY_PAGE_DOWN)};
        bool pressed[4];
        for(int i=0;i<4;++i){pressed[i]=keys[i]&&!radioKeys[i]&&!menu&&!cheatOpen;radioKeys[i]=keys[i];}
        const bool aboard=playing&&driving&&!pending&&!carSinking;
        const bool previous=held(GLFW_KEY_LEFT_SHIFT)||held(GLFW_KEY_RIGHT_SHIFT);
        radio.update(dt,lon,lat,aboard,pressed[0],pressed[1]&&!previous,pressed[1]&&previous,int(pressed[2])-int(pressed[3]));
        style("vehicle-radio","display",aboard?"block":"none");
        if(aboard)text("vehicle-radio",radio.label(lon,lat));
    }
    saida::WebCanvasNode* ui; saida::WebCanvasNode* minimapUi; saida::CameraNode* camera;
    saida::Node* player=nullptr;
    // The player's car. A member of the entry scene beside him, not a prop of
    // the tile it happens to stand on: it is his, so a teleport takes it along
    // instead of evicting it with the neighbourhood he left.
    saida::Node* car=nullptr;
    // The same player body, rig, modifiers and weapon move onto the solver body.
    // Traffic ownership ends permanently when the player takes a bike.
    saida::Node* playerBikeRider=nullptr;
    std::vector<saida::Node*> frontWheels,rearWheels;
    // Every car the player has stepped out of, left standing where he left it.
    // GTA's rule, and the reason the car he is in is a *pointer* rather than
    // the one node the scene shipped: taking over a traffic car hands him that
    // car, not a copy of it moved under his feet.
    struct Parked {saida::Node* node; double lon,lat,alt,yaw;};
    std::vector<Parked> parked;
    bool driving=false,wasEnterKey=false,carParked=false,carSinking=false,wheelsReported=false;
    double carLon=0,carLat=0,carAlt=0,carYaw=0,carSpeed=0,wheelSpin=0,steerShown=0;
    double carWaterLevel=0,carSinkDepth=0;
    // Where the player is looking, measured from behind the car. The walk
    // steers the view and the view steers the walk, which is one angle; a
    // car steers itself, so looking out of the side window has to be a
    // second one. It eases back to zero when the mouse goes quiet.
    double lookYaw=0,lookIdle=0;
    // The boat the player is at the helm of, and the ones he has left moored
    // where he stepped ashore. Like the car, a boat he takes is the node that
    // was moored there, handed over, never a copy.
    struct Vessel {
        saida::Node* node=nullptr; std::string kind="motor";
        double length=8,beam=3,top=10,accel=2,turn=30;
        double lon=0,lat=0,alt=0,yaw=0,speed=0;
    };
    bool sailing=false,swimming=false; Vessel boat; std::vector<Vessel> leftBoats;
    double seaTime=0,swimTime=0,swimHeading=0,swimLean=0;
    struct SeaShip {std::string id; Vessel v; double phase=0;};
    std::vector<SeaShip> seaShips; std::set<std::string> seaTaken;
    std::map<std::string,saida::Node*> hullPrototypes;
    // The sea and the sky, on their own threads (gen/sea.hpp). A new answer
    // is a new pointer, so "has it changed" is one comparison.
    std::unique_ptr<r1::SeaService> sea;
    std::unique_ptr<r1::ConditionsService> sky;
    std::shared_ptr<const json> seaDoc,conditionsDoc;
    double seaAsk=5.,seaRead=0; bool seaFirst=true; std::string seaSaid;
    double conditionsRead=0;
    json conditions=json::object();
    bool smokeSeaWait=false; double captureSeaWait=0;
    bool smokeSail=false,smokeSailWait=false,smokeSailing=false,smokeSailBrake=false,smokeSwimming=false;
    // The aircraft the player is flying, and the ones he has left standing.
    // Like a boat, an aircraft he takes is the node that stood on its stand.
    struct Aircraft {
        saida::Node* node=nullptr; const r1::AircraftType* type=nullptr;
        double lon=0,lat=0,alt=0;              // the point under the wheels or the skids
        double yaw=0,pitch=0,roll=0;           // degrees; nose up and right wing down are positive
        double speed=0,climb=0;                // m/s along the heading, and upward
        double lever=0,thrust=0;               // a plane's throttle, and what its engines give yet
        double rotor=0,mainSpin=0,tailSpin=0;  // a helicopter's rotor speed (0..1) and blade angles
        bool airborne=false;
        std::vector<saida::Node*> gear,mainRotors,tailRotors;
        bool helicopter() const {return type&&type->klass=="helicopter";}
    };
    bool piloting=false; Aircraft plane; std::vector<Aircraft> leftAircraft;
    std::map<std::string,saida::Node*> aircraftPrototypes;
    // The last surface seen under the aircraft: what it keeps to over tiles
    // that have not streamed yet, rather than inventing a ground.
    double planeGround=0; bool planeStopped=false;
    bool smokeFly=false,smokeFlyWait=false; int smokeFlyPhase=0;
    double smokeFlyTime=0,smokeFlyAlt=0,smokeFlyTop=0,smokeFlyClear=0,smokeF=0,smokeR=0,smokeUp=0;
    glm::dvec3 smokeFlyStart{0}; std::set<std::string> smokeFlown;
    glm::dvec3 smokeSwimStart{0}; saida::Node* smokeSwimBoat=nullptr;
    double smokeSailTime=0,smokeSailTop=0,smokeSailClear=0; glm::dvec3 smokeSailStart{0};
    std::vector<saida::Animator*> animators;
    // The player among people (World::moveFeet): his feet in the
    // engine's physics, his body's stagger, the speed he means on foot
    // (east, north, m/s), and what is left of a broken stride.
    saida::CharacterBodyNode* feet=nullptr; int feetUnbuilt=0; bool feetRefused=false;
    // Bodies the physics world refused for capacity (CLAUDE.md rule 3): the
    // engine logs the first of each run, the game names the budget, the
    // player is told and the smoke fails.
    uint64_t physicsRefused=0;
    std::vector<saida::ImpactModifier*> playerImpacts;
    double footEast=0,footNorth=0,stagger=0; size_t playerBumps=0;
    double jumpOffset=0,jumpVelocity=0,followDistance=kOnFootFollow;
    bool wasJump=false;
    bool smokeRan=false,smokeJumped=false;
    bool smokeDriving=false,smokeDroveOnce=false,smokeApproach=false,smokeBrake=false;
    bool smokeTookOver=false;
    double smokeApproachTime=0;
    double smokeDriveTime=0,smokeTopSpeed=0,smokeClear=0; glm::dvec3 smokeDriveStart{0};
    size_t smokeTrafficSeen=0; bool smokeTrafficMoved=false;
    uint64_t generation=0; std::vector<Rml::Element*> listeners;
    std::map<std::string,Loaded> loaded;
    // A shown tile cooked again goes up here, hidden, while the old one is
    // still drawn; the two trade places in one frame (World::promoteIncoming).
    std::map<std::string,Loaded> incoming;
    std::map<std::string,std::string> zoneCountries,countryNames;
    uint64_t minimapRevision=0,minimapDrawnRevision=~uint64_t(0);
    double minimapLon=1e9,minimapLat=1e9;
    saida::Node* prototypes=nullptr;
    std::map<std::string,saida::Node*> naturePrototypes;
    struct VehicleModel {
        std::string name;
        double length=3.6,width=1.94,height=1.5,wheelbase=2.38,wheelRadius=.285;
        saida::Node* prototype=nullptr;
    };
    std::vector<VehicleModel> fleet;
    std::vector<glm::vec3> paints;
    std::vector<size_t> motorcycleRiderKinds,motorcycleFemaleRiderKinds;
    std::unique_ptr<saida::Mesh> motorcycleHelmetShell,motorcycleHelmetVisor;

    // ── people ──────────────────────────────────────────────────────────────
    //
    // assets/models/humans/humans.json (r1/humans.py): the player and the
    // crowd, Rocketbox scans with their clips retargeted onto them. A crowd
    // avatar is imported once into the shared prototypes; each person is a
    // node of its own with its own animator over the avatar's shared meshes,
    // rig and clips, so each avatar costs one upload however many walk.
    json humans;
    double humanScale=.8*kPeopleVehicleResize,playerHeight=1.46*kPeopleVehicleResize;
    struct HumanKind {
        std::string name,model;
        std::vector<std::pair<std::string,std::vector<std::unique_ptr<saida::MeshNode>>>> levels;
        saida::Rig* rig=nullptr;
        std::vector<std::pair<std::string,const saida::AnimationClip*>> clips;
        r1::CrowdPace pace;
        double seatDrop=0;  // origin below a seat's top when seated
        bool ready=false;
    };
    std::vector<HumanKind> crowdKinds;
    std::vector<r1::CrowdAppearance> crowdAppearances;
    r1::CountryCrowd countryCrowd;
    void loadHumans() {
        std::ifstream input(game/"assets/models/humans/humans.json");
        if(!input)throw std::runtime_error("Missing assets/models/humans/humans.json (python -m r1.humans)");
        input>>humans;
        humanScale=humans.at("scale").get<double>()*kPeopleVehicleResize;
        playerHeight=humans.at("player").at("height").get<double>()*humanScale;
        for(const auto& entry:humans.at("crowd")) {
            HumanKind kind;
            kind.name=entry.at("name");kind.model=entry.at("model");
            crowdAppearances.push_back({entry.at("sex").get<std::string>()[0],
                                        entry.value("skinTone",std::string("light"))});
            const auto& clips=entry.at("clips");
            for(const char* clip:{"idle","walk","wait","phone","talk","sit","run"})
                if(!clips.contains(clip))throw std::runtime_error("Crowd avatar "+kind.name+" has no clip "+clip);
            // Drawn at `scale`, a clip covers ground at its captured speed
            // times the scale, and a walker moving any faster would skate.
            kind.pace.walk=clips.at("walk").at("speed").get<double>()*humanScale;
            kind.pace.run=clips.at("run").at("speed").get<double>()*humanScale;
            // A seated pelvis sits about 14 cm above the seat it was captured
            // on (a 45 cm chair under a 59 cm pelvis), at the avatar's scale.
            kind.seatDrop=(entry.at("seat").at("pelvisHeight").get<double>()-.14)*humanScale;
            warmList.push_back(kind.model);
            crowdKinds.push_back(std::move(kind));
        }
        if(crowdKinds.empty())throw std::runtime_error("humans.json lists no crowd");
        std::ifstream countries(game/"assets/world/countries.geojson");
        if(!countries)throw std::runtime_error("Missing assets/world/countries.geojson");
        json boundaries;countries>>boundaries;
        countryCrowd.load(boundaries);
        saida::Log::info("[World crowd] ",crowdKinds.size()," avatars, player ",playerHeight,
                         " m, shared vertices=",humans.value("sharedVertices",0));
    }
    // Shorter strides need a faster cadence at the player's existing speed.
    // Clip views retime playback without rebuilding the imported animation.
    void playPlayerClip(const char* clip) {
        if(std::strcmp(clip,"run")!=0&&std::strcmp(clip,"sprint")!=0) {
            for(auto* a:animators)a->play(clip);
            return;
        }
        saida::ClipView view;
        view.name=clip;view.source=clip;
        view.speed=float(1./kPeopleVehicleResize);
        for(auto* a:animators)a->playView(view);
    }
    // A crowd avatar's shared parts, taken from its prototype on first use:
    // its meshes by level of detail, its rig and its clips.
    HumanKind* humanKind(size_t index) {
        HumanKind& kind=crowdKinds[index%crowdKinds.size()];
        if(kind.ready)return kind.rig?&kind:nullptr;
        saida::Node* source=prototype(kind.model);
        if(!source)return nullptr;
        kind.ready=true;
        std::vector<saida::Animator*> animators;
        source->findBehavioursInChildren(animators);
        if(animators.empty()||!animators.front()->rig()) {
            saida::Log::error("[World crowd] avatar has no skeleton: ",kind.model);return nullptr;
        }
        kind.rig=const_cast<saida::Rig*>(animators.front()->rig());
        for(const auto& [name,clip]:animators.front()->clips())kind.clips.push_back({name,clip});
        std::function<void(saida::Node&,const saida::Transform&,const std::string&)> walk=
            [&](saida::Node& n,const saida::Transform& above,const std::string& level) {
                saida::Transform here;
                here.rotation=above.rotation*n.transform().rotation;
                here.scale=above.scale*n.transform().scale;
                here.position=above.position+above.rotation*(above.scale*n.transform().position);
                const std::string mine=n.name()=="Near"||n.name()=="Far"?n.name():level;
                if(n.mesh()&&!mine.empty()) {
                    auto mesh=std::make_unique<saida::MeshNode>(n.name(),n.mesh(),n.material());
                    mesh->transform()=here;
                    auto found=std::find_if(kind.levels.begin(),kind.levels.end(),[&](const auto& l){return l.first==mine;});
                    if(found==kind.levels.end()){kind.levels.emplace_back(mine,std::vector<std::unique_ptr<saida::MeshNode>>{});found=kind.levels.end()-1;}
                    found->second.push_back(std::move(mesh));
                }
                for(auto& c:n.children())walk(*c,here,mine);
            };
        walk(*source,saida::Transform{},"");
        if(kind.levels.size()!=2) {
            saida::Log::error("[World crowd] ",kind.model," has ",kind.levels.size()," levels of detail, not Near and Far");
            kind.rig=nullptr;return nullptr;
        }
        return &kind;
    }
    // The Rocketbox biped's chains (r1/humans.py keeps its eyes for this).
    // A person's gaze, not a turret's: the eyes go first and furthest, the
    // head most of the way, the spine barely; past 83 degrees either side
    // the head holds at its limit rather than turn round.
    static saida::GazeModifier::Settings gazeSettings() {
        saida::GazeModifier::Settings s;
        s.chain={{"Bip01 Spine2",.15f,.25f},{"Bip01 Neck",.35f,.45f},{"Bip01 Head",1.f,.8f},
                 {"Bip01 LEye",1.f,.35f},{"Bip01 REye",1.f,.35f}};
        s.maxYaw=1.45f;s.maxPitchUp=.4f;s.maxPitchDown=.6f;s.response=5.f;
        return s;
    }
    // A blow bends the back from the waist up, the head last and loosest,
    // and the body swings back upright in under a second.
    static saida::ImpactModifier::Settings impactSettings() {
        saida::ImpactModifier::Settings s;
        s.chain={{"Bip01 Spine",.3f},{"Bip01 Spine1",.25f},{"Bip01 Spine2",.2f},{"Bip01 Neck",.1f},{"Bip01 Head",.15f}};
        s.frequency=1.4f;s.damping=.4f;s.maxAngle=.55f;
        return s;
    }
    // People whose skeleton lacks a bone the gaze or the stagger needs:
    // said once each (CLAUDE.md rule 3), and a failed smoke.
    size_t bodiesRefused=0;
    bool makePerson(Loaded& tile,size_t slot,Person& person) {
        const size_t selected=tile.humanKinds[slot%tile.humanKinds.size()];
        HumanKind* kind=humanKind(selected);
        if(!kind)return false;
        person.kind=selected;
        auto root=std::make_unique<saida::Node>("person-"+kind->name);
        auto* animator=root->addBehaviour<saida::Animator>();
        animator->setRig(kind->rig);
        for(const auto& [name,clip]:kind->clips)animator->addClip(name,clip);
        person.gaze=animator->addModifier<saida::GazeModifier>(*kind->rig,gazeSettings());
        person.impact=animator->addModifier<saida::ImpactModifier>(*kind->rig,impactSettings());
        if(!person.gaze->valid()||!person.impact->valid()) {
            if(bodiesRefused++==0)
                saida::Log::error("[World crowd] ",kind->model," lacks the spine, neck, head or eye bones the gaze and the "
                                  "stagger turn (python -m r1.humans keeps them): its people will neither look nor stagger");
        }
        // Their arms for a fight, and their fall: built when first needed
        // (RagdollBehaviour), nothing simulated until someone is knocked down.
        person.arms[0]=animator->addModifier<saida::TwoBoneIKModifier>(*kind->rig,r1::rocketboxArm(false));
        person.arms[1]=animator->addModifier<saida::TwoBoneIKModifier>(*kind->rig,r1::rocketboxArm(true));
        person.ragdoll=root->addBehaviour<saida::RagdollBehaviour>(ragdollProfile);
        for(const auto& [level,meshes]:kind->levels) {
            auto holder=std::make_unique<saida::Node>(level);
            for(const auto& m:meshes) {
                auto mesh=std::make_unique<saida::MeshNode>(m->name(),m->mesh(),m->material());
                mesh->transform()=m->transform();
                holder->addChild(std::move(mesh));
            }
            root->addChild(std::move(holder));
        }
        // Near while a person stands taller than about 3.5% of the screen,
        // some 35 m away; the far model's 500 triangles beyond.
        root->addBehaviour<saida::LODGroupBehaviour>()->setLevels({{"Near",.035f},{"Far",0.f}});
        // Their body in the engine's physics: a kinematic capsule that follows
        // the node wherever the crowd moves it, and that the player's feet
        // meet (World::moveFeet). In the node's units, drawn at scale.
        auto body=std::make_unique<saida::RigidBodyNode>();
        body->kinematic=true;
        body->transform().position=glm::vec3(0.f,float(playerHeight*.5/humanScale),0.f);
        auto capsule=std::make_unique<saida::CollisionShapeNode>();
        capsule->shapeType=saida::CollisionShapeType::Capsule;
        capsule->radius=float(r1::Crowd::kBodyRadius/humanScale);
        capsule->height=float(playerHeight/humanScale);
        capsule->axis=1;
        body->addChild(std::move(capsule));
        person.body=static_cast<saida::RigidBodyNode*>(root->addChild(std::move(body)));
        root->transform().scale=glm::vec3(float(humanScale));
        person.node=tile.node->addChild(std::move(root));
        person.animator=animator;
        return true;
    }
#include "beach_runtime.hpp"

    void readCrowd(Loaded& tile) {
        tile.walks=r1::WalkGraph::from(tile.data.value("crowd",json()));
        const uint32_t seed=uint32_t(std::hash<std::string>{}(tile.data.at("key").get<std::string>()))|1u;
        const r1::Country country=countryCrowd.at(tile.data.at("lon").get<double>(),tile.data.at("lat").get<double>());
        tile.humanKinds.clear();
        std::vector<r1::CrowdPace> paces;
        for(size_t slot=0;slot<kCrowdPeople;++slot) {
            const size_t selected=countryCrowd.choose(country,crowdAppearances,seed,slot);
            tile.humanKinds.push_back(selected);
            paces.push_back(crowdKinds[selected].pace);
        }
        tile.crowd.reset(&tile.walks,seed,std::move(paces));
        for(const auto& spot:tile.data.value("beachSpots",json::array())) {
            const auto& a=spot.at("towel");const auto& b=spot.at("water");
            tile.beachSpots.push_back({{a[0],a[1],a[2]},{b[0],b[1],b[2]},spot.value("parasol",false)});
        }
        if(!tile.beachSpots.empty())tile.beachVisitors.resize(12);
    }
    // The hour where the player stands, by the Sun: 12 when it is highest.
    double localSolarHour() {
        double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
        return std::fmod(std::fmod(unixSeconds/3600.+lon/15.,24.)+24.,24.);
    }
    double crowdFactor() {
        double factor=r1::crowdHourFactor(localSolarHour());
        // Rain and falling snow send people indoors, about half of them.
        const auto weather=effectiveWeather();
        if(weather.known&&(weather.rain>.5||weather.snowfall>.3))factor*=.5;
        return factor;
    }
    void updateCrowd(float delta) {
        SAIDA_PROFILE_SCOPE("World/Crowd");
        if(crowdKinds.empty())return;
        size_t remaining=kCrowdPeople-beachLive;
        const double factor=crowdFactor();
        for(auto tile:ring) {
            auto found=loaded.find(tile.key());
            if(found==loaded.end())continue;
            Loaded& l=found->second;
            if(l.walks.links.empty())continue;
            const glm::dvec3 eye=cameraTileLocal(l);
            if(std::hypot(eye.x-l.crowdEyeX,eye.z-l.crowdEyeZ)>=8.) {
                l.crowdEyeX=eye.x;l.crowdEyeZ=eye.z;
                l.crowdLocalPopulation=l.walks.populationNear(eye.x,eye.z,r1::Crowd::kSpawnFar);
            }
            const size_t normal=std::min<size_t>(size_t(std::lround(l.crowdLocalPopulation*factor)),remaining);
            const size_t share=l.reducedDensity?normal/4:normal;
            remaining-=share;
            l.crowd.setPopulation(int(share));
            r1::Crowd::Scene scene;
            scene.crossingAllowed=[this,&l](size_t link) {
                const auto& crossing=l.walks.links[link];
                const auto& start=l.walks.nodes[crossing.a];const auto& end=l.walks.nodes[crossing.b];
                const glm::vec2 middle(float((start.x+end.x)*.5),float((start.z+end.z)*.5));
                constexpr float kControlledCrossingDistance=35.f;
                float nearest=kControlledCrossingDistance;int signal=-1;
                for(size_t i=0;i<l.signalPositions.size();++i) {
                    const float distance=glm::distance(middle,l.signalPositions[i]);
                    if(distance<nearest){nearest=distance;signal=int(i);}
                }
                if(signal>=0&&!l.graph.signals[size_t(signal)].pedestriansMayStart(trafficTime_))return false;
                for(const auto& car:l.flow.agents())if(car.alive) {
                    const auto pose=l.flow.pose(car);
                    const float gap=glm::distance(middle,glm::vec2(pose.position.x,pose.position.y));
                    constexpr float kCrossingSafetyTime=3.f;
                    if(gap<car.length+pose.speed*kCrossingSafetyTime)return false;
                }
                return true;
            };
            scene.eyeX=eye.x;scene.eyeZ=eye.z;
            const glm::vec3 ahead=camera->transform().rotation*glm::vec3(0,0,-1);
            const glm::dvec3 look=glm::transpose(l.frame.basis)*origin.basis*glm::dvec3(ahead.x,ahead.y,ahead.z);
            const double flat=std::hypot(look.x,look.z);
            if(flat>1e-6){scene.faceX=look.x/flat;scene.faceZ=look.z/flat;}
            if(driving) {
                const glm::dvec3 p=l.frame.local(ecef(carLon,carLat,carAlt));
                scene.carX=p.x;scene.carZ=p.z;scene.carSpeed=std::abs(carSpeed);
            }
            // The player on foot: where he is, the speed he means (a tile's x
            // is east and its z south, near enough across one tile), and his
            // eyes, which is what people look at.
            glm::dvec3 head(0);
            const bool onFoot=!driving&&!sailing&&!piloting&&!swimming;
            if(onFoot) {
                const glm::dvec3 p=l.frame.local(ecef(lon,lat,alt));
                scene.playerX=p.x;scene.playerZ=p.z;
                scene.playerVX=footEast;scene.playerVZ=-footNorth;
                head=l.frame.local(ecef(lon,lat,alt+jumpOffset+playerHeight*.93));
            }
            l.crowd.update(std::min(.05,double(delta)),scene);
            syncPeople(l,eye,std::min(.05,double(delta)),onFoot?&head:nullptr);
        }
    }
    // A step of the player's among people, `east` and `north` metres: his
    // feet are the engine's character body, a standing capsule moved and slid
    // against the people's capsules (CharacterBodyNode::moveAndSlide), so he
    // stops at whoever is in the way and slides round them, and whoever walks
    // into him moves him. What his feet touched is then a bump each
    // (World::meetPeople). Buildings, furnishings and vehicles are streamed engine bodies too.
    glm::dvec3 moveFeet(double east,double north,double dt) {
        const glm::dvec2 intended=onward(lon,lat,east,north);
        if(dt<=0)return {intended,alt};
        // The scene's frame is the origin's: x east, y up, z south, near
        // enough within the 350 m it is rebased at.
        const glm::vec3 from(origin.local(ecef(lon,lat,alt+jumpOffset+.06))),to(origin.local(ecef(intended.x,intended.y,alt+jumpOffset+.06)));
        glm::vec3 velocity=(to-from)/float(dt);
        // The engine sticks supported feet to slopes and steps. A constant
        // downward velocity on supported ground projects into downhill motion.
        velocity.y=jumpOffset<=0&&!feet->isOnFloor()?-2.f:0.f;
        feet->transform().position=from;
        const glm::vec3 end=feet->moveAndSlide(velocity,float(dt));
        // Built by its first physics step; one that never is means he walks
        // through everyone, which is said once and fails the smoke.
        if(feet->physicsWorld())feetUnbuilt=0;
        else if(++feetUnbuilt==30&&!feetRefused) {
            saida::Log::error("[World crowd] the player's feet are no physics body: he walks through people");
            feetRefused=true;
        }
        meetPeople();
        const auto horizontal=onward(lon,lat,end.x-from.x,-(end.z-from.z));
        return {horizontal,alt+end.y-from.y};
    }
    // Every person the feet touched: the crowd answers the bump if it is one
    // (r1::Crowd::bump), and the player's own body feels it.
    void meetPeople() {
        for(const auto& contact:feet->contacts()) {
            if(!contact.node)continue;
            if(hitByVehicle(contact))return;
            for(auto tile:ring) {
                auto found=loaded.find(tile.key());
                if(found==loaded.end())continue;
                Loaded& l=found->second;
                for(size_t slot=0;slot<l.people.size();++slot) {
                    if(l.people[slot].body!=contact.node)continue;
                    // From him toward them (the normal points back at him),
                    // in the tile's frame, level.
                    glm::dvec3 toward=glm::transpose(l.frame.basis)*origin.basis*glm::dvec3(-glm::vec3(contact.normal));
                    toward.y=0;
                    const double length=glm::length(toward);
                    if(length<1e-6)break;
                    toward/=length;
                    // How fast they close: his speed toward them, and theirs toward him.
                    const double closing=footEast*toward.x-footNorth*toward.z+l.crowd.speedAlong(slot,-toward.x,-toward.z);
                    if(l.crowd.bump(slot,toward.x,toward.z,closing))staggerPlayer(l,toward,closing);
                    break;
                }
            }
        }
    }
    // Running into someone, the player's own body takes the blow -- thrown
    // back off whoever he hit -- and his stride is broken for a moment,
    // longer the harder he hit.
    void staggerPlayer(Loaded& tile,const glm::dvec3& toward,double speed) {
        const glm::dvec3 back=glm::transpose(origin.basis)*tile.frame.basis*(-toward);
        for(size_t i=0;i<animators.size()&&i<playerImpacts.size();++i) {
            if(!animators[i]->node())continue;
            glm::mat3 body(animators[i]->node()->worldTransform());
            for(int c=0;c<3;++c)body[c]=glm::normalize(body[c]);
            playerImpacts[i]->push(glm::transpose(body)*glm::vec3(back),float(std::min(5.,.6*speed)));
        }
        stagger=std::max(stagger,speed>=r1::Crowd::kShove?.45:.2);
        ++playerBumps;
    }
    void syncPeople(Loaded& tile,const glm::dvec3& eye,double dt,const glm::dvec3* head) {
        const auto& walkers=tile.crowd.walkers();
        if(tile.people.size()<walkers.size())tile.people.resize(walkers.size());
        for(size_t i=0;i<tile.people.size();++i) {
            Person& p=tile.people[i];
            const bool live=i<walkers.size()&&walkers[i].alive;
            if(!p.node) {
                if(!live)continue;
                if(!makePerson(tile,i,p))continue;
            }
            // Disabled rather than hidden: nobody animates a person who is not
            // there, and their capsule leaves the physics with them.
            if(!live){if(p.node->enabled())p.node->setEnabled(false);p.shown=false;p.punch=-1;continue;}
            if(!p.node->enabled())p.node->setEnabled(true);
            const r1::Walker& w=walkers[i];
            // On the ground, the ragdoll holds the body (World::knockDown).
            if(personDown(tile,i,p,w,dt))continue;
            if(p.body&&!p.body->enabled())p.body->setEnabled(true);
            // Off the paths -- going for the player, back to a path after a
            // fall -- they walk on the ground itself.
            double y=r1::Crowd::offGraph(w)?groundUnder(tile,w.x,w.z,w.y):w.y;
            if(w.activity==r1::Activity::Sit)y-=crowdKinds[p.kind].seatDrop;
            p.node->transform().position=glm::vec3(float(w.x),float(y),float(w.z));
            // A body turns rather than snaps: round a corner, or round to
            // face the player it is telling off. Someone new stands as placed.
            if(!p.shown){p.yaw=w.heading;p.shown=true;p.bumps=w.bumps;}
            else p.yaw+=std::remainder(w.heading-p.yaw,6.283185307179586)*(1-std::exp(-7.*dt));
            // Modelled facing +Z, like the player's body; this world's forward is -Z.
            const glm::quat facing=glm::angleAxis(float(-p.yaw),glm::vec3(0,1,0))*glm::quat(0,0,1,0);
            p.node->transform().rotation=facing;
            // Walked into: the stagger, along the blow, in the body's own axes.
            if(w.bumps!=p.bumps) {
                if(w.bumps>p.bumps&&p.impact)
                    p.impact->push(glm::inverse(facing)*glm::vec3(float(w.bumpX),0.f,float(w.bumpZ)),
                                   float(std::min(8.,1.1*w.bumpSpeed)));
                p.bumps=w.bumps;
            }
            // Looking at him: his eyes, in the body's own axes and size.
            if(p.gaze) {
                if(w.look>0&&head)
                    p.gaze->lookAt(glm::inverse(facing)*(glm::vec3(*head)-p.node->transform().position)/float(humanScale));
                else p.gaze->release();
            }
            const char* clip=w.activity==r1::Activity::Fight&&w.closing?"run":r1::clipOf(w.activity);
            if(std::strcmp(clip,p.clip)!=0){p.animator->play(clip);p.clip=clip;}
            // Animation LOD: a pose every frame close by; further out the pose
            // is resampled at 15, 8 and 4 Hz and held in between, which costs
            // nothing (Animator::PoseRateMode::Hold).
            const double d=std::hypot(w.x-eye.x,w.z-eye.z);
            const float rate=d<12.?0.f:d<30.?15.f:d<60.?8.f:4.f;
            if(rate!=p.poseRate) {
                p.animator->setPoseRate(rate,saida::Animator::PoseRateMode::Hold);
                p.poseRate=rate;
            }
            personAttack(tile,i,p,w,dt);
        }
    }
    // ── the fight ───────────────────────────────────────────────────────────
    //
    // The Fun update (PLAN §8): GTA III's street on the real one. Fists and a
    // pistol; passers-by who fall, run, or square up and fight back; a car
    // that throws whoever it hits over its bonnet; a player who can be hurt,
    // knocked down, and wake up at the nearest mapped hospital. The rules are
    // native/combat.hpp; the engine gives the falls (RagdollBehaviour), the
    // arms (TwoBoneIKModifier) and the pistol in the hand (BoneAttachment).
    r1::combat::Arsenal arsenal; r1::combat::Health health; r1::combat::Punch punch;
    r1::CombatEffects effects; r1::CombatSounds sounds;
    saida::RagdollProfile ragdollProfile=r1::rocketboxRagdoll();
    saida::Node* playerBody=nullptr; saida::Transform playerBodyRest;
    saida::RagdollBehaviour* playerRagdoll=nullptr;
    saida::TwoBoneIKModifier* playerArms[2]{nullptr,nullptr};
    saida::TwoBoneIKModifier* playerBikeLegs[2]{nullptr,nullptr};  // left, right
    saida::GazeModifier* playerBikeLean=nullptr;
    saida::GazeModifier* playerBikeLook=nullptr;
    r1::RocketboxRidingAim* playerBikeAim=nullptr;
    r1::RocketboxRidingHands* playerBikeHands=nullptr;
    saida::Node* pistol=nullptr; saida::Transform pistolGrip;
    float playerReach=.55f;  // shoulder to wrist, the rig's metres
    bool aiming=false,wasFire=false,wasReloadKey=false; double aimBlend=0;
    bool wasWeaponKey[2]{};
    double fightClock=0,arrivalClock=0;
    bool playerDown=false,wasted=false; double downClock=0;
    // The cheat line (World::updateCheats): open, the keys are its own.
    int outOfMemorySaid=0;
    bool cheatOpen=false,wasCheatKey=false,wasCheatEnter=false,wasCheatEscape=false; double cheatSaidAt=-1e9;
    std::string punchTileKey; size_t punchSlot=0; bool punchAimed=false;  // whom a punch is thrown at
    double hurtShown=-1e9,recoil=0;
    size_t ragdollRefused=0;
    std::string respawnSaid;
    bool smokeFought=false,smokeFighting=false; double smokeFightTime=0; int smokeFightPhase=0;
    std::string smokeFightTile; size_t smokeFightSlot=0;

    // The player's scan arrives as one MeshNode per material, each with an
    // Animator of its own (GLTFLoader): one pose evaluated three times, and
    // three skeletons a ragdoll or a held pistol would have to keep together.
    // Like a passer-by (World::makePerson), the body becomes one node with one
    // Animator over the scan's meshes.
    void unifyPlayerBody() {
        if(animators.empty()||!animators.front()->rig())return;
        saida::Animator* source=animators.front();
        saida::Node* model=nullptr;
        for(auto& c:player->children())
            for(saida::Node* n=source->node();n&&!model;n=n->parent())if(n==c.get())model=c.get();
        if(!model)return;
        auto body=std::make_unique<saida::Node>("player-body");
        body->transform()=model->transform();
        auto* animator=body->addBehaviour<saida::Animator>();
        animator->setRig(const_cast<saida::Rig*>(source->rig()));
        for(const auto& [name,clip]:source->clips())animator->addClip(name,clip);
        std::function<void(saida::Node&,const saida::Transform&)> walk=[&](saida::Node& n,const saida::Transform& above) {
            saida::Transform here;
            here.rotation=above.rotation*n.transform().rotation;
            here.scale=above.scale*n.transform().scale;
            here.position=above.position+above.rotation*(above.scale*n.transform().position);
            if(n.mesh()) {
                auto mesh=std::make_unique<saida::MeshNode>(n.name(),n.mesh(),n.material());
                mesh->transform()=here;
                if(auto* shown=dynamic_cast<saida::MeshNode*>(&n))mesh->castShadows()=shown->castShadows();
                body->addChild(std::move(mesh));
            }
            for(auto& c:n.children())walk(*c,here);
        };
        for(auto& c:model->children())walk(*c,saida::Transform{});
        model->setEnabled(false);
        const size_t before=animators.size();
        playerBody=player->addChild(std::move(body));
        playerBodyRest=playerBody->transform();
        animators={animator};
        saida::Log::info("[World player] one body: one Animator over ",playerBody->children().size(),
                         " meshes (the scan imported ",before,")");
    }
    void setupCombat() {
        if(!playerBody){playerBody=animators.front()->node();playerBodyRest=playerBody->transform();}
        auto* animator=animators.front();
        const saida::Rig& rig=*animator->rig();
        playerBikeLean=animator->addModifier<saida::GazeModifier>(rig,r1::rocketboxRidingLean());
        playerBikeAim=animator->addModifier<r1::RocketboxRidingAim>(rig);
        playerBikeLook=animator->addModifier<saida::GazeModifier>(rig,r1::rocketboxRidingLook());
        playerArms[0]=animator->addModifier<saida::TwoBoneIKModifier>(rig,r1::rocketboxArm(false));
        playerArms[1]=animator->addModifier<saida::TwoBoneIKModifier>(rig,r1::rocketboxArm(true));
        if(!playerArms[0]->valid()||!playerArms[1]->valid())
            throw std::runtime_error("Player skeleton lacks the arm chains a punch reaches with");
        for(int h=0;h<2;++h)playerBikeLegs[h]=animator->addModifier<saida::TwoBoneIKModifier>(rig,
            saida::TwoBoneIKModifier::Settings{h?"Bip01 R Thigh":"Bip01 L Thigh",h?"Bip01 R Calf":"Bip01 L Calf",h?"Bip01 R Foot":"Bip01 L Foot",22.f});
        playerBikeHands=animator->addModifier<r1::RocketboxRidingHands>(rig);
        playerReach=r1::armLength(rig,true);
        playerRagdoll=playerBody->addBehaviour<saida::RagdollBehaviour>(ragdollProfile);
        if(!r1::pistolGrip(rig,pistolGrip))throw std::runtime_error("Player skeleton lacks the hand and fingers a pistol is held in");
        if(auto built=r1::buildPistol(engine.resources())) {
            built->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 R Hand",pistolGrip);
            pistol=playerBody->addChild(std::move(built));
            pistol->setVisible(false);
        }
        effects.build(engine.sceneTree().world());
        sounds.load();
        saida::Log::info("[World combat] ready: ",ragdollProfile.parts.size()," bodies per ragdoll, ",kLiveRagdolls,
                         " tumbling at once; arm ",playerReach," m");
    }

    // A tile-frame point's ground: what a passer-by off the paths stands on.
    double groundUnder(const Loaded& tile,double x,double z,double fallback) {
        const glm::dvec3 local(x,fallback,z);
        const auto point=tile.frame.origin+tile.frame.basis*local;
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        if(!this->tile(geo.x,geo.y))return fallback;
        const double h=height(geo.x,geo.y,geo.z+.5);
        if(!std::isfinite(h))return fallback;
        return tile.frame.local(ecef(geo.x,geo.y,h)).y;
    }
    // Scene-frame point to geodetic, and the height of the ground there.
    glm::dvec3 sceneToGeo(const glm::vec3& p) const {
        const auto point=origin.origin+origin.basis*glm::dvec3(p);
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        return {geo.x,geo.y,geo.z};
    }
    float sceneGround(const glm::vec3& p) {
        const auto geo=sceneToGeo(p);
        if(!tile(geo.x,geo.y))return p.y;
        const double h=height(geo.x,geo.y,geo.z+.5);
        return std::isfinite(h)?float(origin.local(ecef(geo.x,geo.y,h)).y):p.y;
    }
    Loaded* tileByKey(const std::string& key) {
        auto found=loaded.find(key);
        return found==loaded.end()?nullptr:&found->second;
    }
    static glm::vec3 nodePosition(const saida::Node& n) { return glm::vec3(n.worldTransform()[3]); }
    // A scene direction to the tile frame (both are tangent frames a few km apart at most).
    glm::dvec3 toTile(const Loaded& tile,const glm::vec3& v) const {
        return glm::transpose(tile.frame.basis)*origin.basis*glm::dvec3(v);
    }

    // ── bodies on the ground ──

    // Off their feet: the crowd stops walking them, their capsule leaves the
    // physics, and the engine's ragdoll takes the body, moving at `velocity`
    // (scene frame), turning at `spin`. Past kLiveRagdolls tumbling at once,
    // the one down longest is frozen where it lies. A world too full for the
    // bodies is said (CLAUDE.md rule 3), and the person only staggers.
    bool knockDown(Loaded& tile,size_t slot,const glm::vec3& velocity,const glm::vec3& spin) {
        if(slot>=tile.people.size())return false;
        Person& p=tile.people[slot];
        if(!p.node||!p.ragdoll)return false;
        if(p.ragdoll->simulating())return true;
        limitRagdolls();
        if(p.body)p.body->setEnabled(false);
        if(!p.ragdoll->activate(velocity,spin)) {
            if(ragdollRefused++==0)
                saida::Log::error("[World combat] a knocked-down body was refused by the physics world (kCombatBodies ",
                                  kCombatBodies," of kPhysicsBodies ",kPhysicsBodies,"): the person only staggers");
            if(p.body)p.body->setEnabled(true);
            text("stream-status","Physics world full: the fall could not be simulated.");
            return false;
        }
        tile.crowd.knockDown(slot);
        p.lying=0;p.punch=-1;
        for(auto* arm:p.arms)if(arm)arm->release();
        if(p.gaze)p.gaze->release();
        return true;
    }
    void limitRagdolls() {
        size_t live=0;saida::RagdollBehaviour* oldest=nullptr;
        for(auto tile:ring) {
            auto* t=tileByKey(tile.key());if(!t)continue;
            for(auto& p:t->people) {
                if(!p.ragdoll||!p.ragdoll->simulating())continue;
                ++live;
                if(!oldest||p.ragdoll->simulatedSeconds()>oldest->simulatedSeconds())oldest=p.ragdoll;
            }
        }
        if(live>=kLiveRagdolls&&oldest)oldest->freeze();
        SAIDA_PROFILE_COUNTER("World/RagdollsTumbling",double(live));
    }
    // A body down: its node is the ragdoll's. Once still for a moment, the
    // living get up -- the pose blends back to the animation where the body
    // lies -- and go back to the paths, fists up or running.
    bool personDown(Loaded& tile,size_t slot,Person& p,const r1::Walker& w,double dt) {
        if(w.activity!=r1::Activity::Down) {
            // Back on their feet and blending out: the crowd walks them again.
            return false;
        }
        if(!p.ragdoll)return true;
        const auto state=p.ragdoll->state();
        if(state==saida::RagdollBehaviour::State::Animated) {
            // Down without a body to show (a ragdoll the world refused, or
            // released by a disable): stood up where they are.
            const glm::vec3 at=p.node->transform().position;
            tile.crowd.getUp(slot,at.x,at.y,at.z,p.yaw);
            return false;
        }
        if(state==saida::RagdollBehaviour::State::Simulated) {
            if(p.ragdoll->simulatedSeconds()>=kLongestTumble)p.ragdoll->freeze();
            return true;
        }
        if(w.dead)return true;
        if(state==saida::RagdollBehaviour::State::Frozen&&(p.lying+=dt)>=r1::combat::kLieSeconds) {
            const glm::vec3 at=nodePosition(*p.node);
            p.ragdoll->blendOut(float(r1::combat::kStandSeconds),sceneGround(at));
            const glm::vec3 local=p.node->transform().position;
            const glm::vec3 front=p.node->transform().rotation*glm::vec3(0,0,1);
            p.yaw=std::atan2(front.x,-front.z);p.shown=true;
            tile.crowd.getUp(slot,local.x,local.y,local.z,p.yaw);
            p.clip="";
            return false;
        }
        return true;
    }

    // ── a passer-by's punch ──

    // A fighter's punch (r1::Crowd counts them): the arm goes out at the
    // player's head and lands if he is still in reach.
    void personAttack(Loaded& tile,size_t slot,Person& p,const r1::Walker& w,double dt) {
        (void)slot;
        if(w.attacks!=p.attacks) {
            if(w.attacks>p.attacks&&p.punch<0){p.punch=0;p.punchHand^=1;p.struck=false;}
            p.attacks=w.attacks;
        }
        if(p.punch<0)return;
        p.punch+=dt;
        auto* arm=p.arms[p.punchHand];
        const double reach=r1::combat::Punch::extensionAt(p.punch);
        if(arm&&player->enabled()) {
            const glm::mat4 toObject=glm::inverse(p.node->worldTransform());
            const glm::vec3 head=player->transform().position+glm::vec3(0.f,float(playerHeight*.9),0.f);
            const glm::vec3 target=glm::vec3(toObject*glm::vec4(head,1.f));
            arm->reach(target,float(reach));arm->setWeightNow(float(reach));
            arm->setPole({p.punchHand?-.5f:.5f,.9f,-.3f});
        }
        if(!p.struck&&p.punch>=r1::combat::Punch::kStrike) {
            p.struck=true;
            const glm::vec3 from=nodePosition(*p.node),to=player->transform().position;
            const bool onFoot=!driving&&!sailing&&!piloting&&!swimming&&!playerDown;
            if(onFoot&&glm::length(glm::vec2(to.x-from.x,to.z-from.z))<r1::Crowd::kFightReach+.45) {
                sounds.play(r1::CombatSounds::Punch,glm::length(camera->transform().position-to));
                hurtPlayer(r1::combat::kFists.damage*.65,glm::normalize(glm::vec3(to.x-from.x,0.f,to.z-from.z)+glm::vec3(0,.001f,0)),"a passer-by");
            } else sounds.play(r1::CombatSounds::Swing,glm::length(camera->transform().position-from),.6f);
        }
        if(p.punch>=r1::combat::Punch::kRecover){p.punch=-1;if(arm)arm->release();}
        (void)tile;
    }

    // ── the player hurt ──

    void hurtPlayer(double damage,const glm::vec3& push,const char* by) {
        if(wasted||!player->enabled()||health.invincible())return;
        const bool killed=health.take(damage);
        for(size_t i=0;i<animators.size()&&i<playerImpacts.size();++i) {
            glm::mat3 body(animators[i]->node()->worldTransform());
            for(int c=0;c<3;++c)body[c]=glm::normalize(body[c]);
            playerImpacts[i]->push(glm::transpose(body)*push,float(std::min(9.,2.+damage*.25)));
        }
        hurtShown=fightClock;
        saida::Log::info("[World combat] player hurt by ",by,": ",damage," (health ",health.hp,")");
        if(killed)knockPlayerDown(push*3.f+glm::vec3(0,1.f,0),glm::cross(glm::vec3(0,1,0),push)*2.f,true);
    }
    // Off his feet: the ragdoll takes his body, his feet leave the physics.
    void knockPlayerDown(const glm::vec3& velocity,const glm::vec3& spin,bool killed) {
        if(playerDown){if(killed)wasted=true;return;}
        if(aiming)setAiming(false);
        for(auto* arm:playerArms)if(arm)arm->release();
        if(!playerRagdoll->activate(velocity,spin)) {
            if(ragdollRefused++==0)
                saida::Log::error("[World combat] the player's fall was refused by the physics world (kCombatBodies ",kCombatBodies,")");
            if(killed){wasted=true;playerDown=true;downClock=0;}
            return;
        }
        playerDown=true;wasted=killed;downClock=0;
        sounds.play(r1::CombatSounds::Fall,followDistance,.8f);
        if(killed)showWasted(true);
    }
    void landed(double speed) {
        if(fightClock-arrivalClock<4.)return;  // a spawn settling on the relief is not a fall
        const double damage=r1::combat::fallDamage(speed);
        if(damage<=0||health.invincible())return;
        sounds.play(r1::CombatSounds::Fall,followDistance,std::min(1.,speed/12.));
        hurtPlayer(damage,glm::vec3(0,-1,0),"the fall");
        if(!playerDown&&damage>=35)knockPlayerDown(glm::vec3(0,-1.f,0),glm::vec3(0),false);
    }
    // A car the engine says his feet touched, closing on him: thrown.
    bool hitByVehicle(const saida::CharacterBodyNode::Contact& contact) {
        if(contact.node->name()!=r1::VehicleRuntime::kBodyName||!contact.node->physicsWorld())return false;
        // Only the car's own motion toward him counts: running into a parked
        // car is his speed, not a blow.
        const glm::vec3 velocity=contact.node->physicsWorld()->linearVelocity(contact.node->bodyId());
        const float closing=glm::dot(velocity,glm::normalize(contact.normal));
        const auto hit=r1::combat::carImpact(closing);
        if(!hit.knockDown||playerDown)return false;
        const float speed=glm::length(velocity);
        const glm::vec3 along=speed>.1f?velocity/speed:glm::normalize(contact.normal);
        sounds.play(r1::CombatSounds::CarHit,followDistance);
        const bool killed=health.take(hit.damage);
        hurtShown=fightClock;
        saida::Log::info("[World combat] player hit by a car at ",closing," m/s: ",hit.damage," (health ",health.hp,")");
        knockPlayerDown(velocity*.8f+glm::vec3(0,1.f+.12f*speed,0),glm::cross(along,glm::vec3(0,1,0))*std::clamp(speed*.5f,2.f,8.f),killed);
        return true;
    }
    // Where the camera looks while he is down: his pelvis.
    glm::vec3 downAnchor() const {
        if(playerRagdoll&&playerRagdoll->ragdoll()&&playerRagdoll->simulating())return playerRagdoll->ragdoll()->rootPosition();
        return nodePosition(*playerBody);
    }
    // Down and still, he gets up where he lies; dead, he wakes at a hospital.
    void updatePlayerDown(double dt) {
        downClock+=dt;
        const auto state=playerRagdoll->state();
        if(wasted) {
            if(downClock>=r1::combat::kWastedSeconds)respawn();
            return;
        }
        // A body pinned against a car or a wall may never come to rest: it is
        // frozen where it lies after a while rather than kept on the ground.
        if(state==saida::RagdollBehaviour::State::Simulated&&downClock>=kLongestTumble)playerRagdoll->freeze();
        if(playerRagdoll->state()==saida::RagdollBehaviour::State::Frozen&&downClock>=r1::combat::kPlayerLieSeconds) {
            playerRagdoll->blendOut(float(r1::combat::kStandSeconds),sceneGround(nodePosition(*playerBody)));
            // His node is where his body now stands; his body back at rest
            // under it, the same place on screen.
            const glm::mat4 bodyWorld=player->transform().matrix()*playerBody->transform().matrix();
            const glm::mat4 standing=bodyWorld*glm::inverse(playerBodyRest.matrix());
            const glm::vec3 at(standing[3]);
            glm::mat3 axes(standing);for(int c=0;c<3;++c)axes[c]=glm::normalize(axes[c]);
            const auto geo=sceneToGeo(at);
            lon=geo.x;lat=geo.y;alt=height(lon,lat,geo.z+.5);if(!std::isfinite(alt))alt=geo.z;
            jumpOffset=jumpVelocity=0;
            player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
            player->transform().rotation=glm::normalize(glm::quat_cast(axes));
            playerBody->transform()=playerBodyRest;
            // The ragdoll put the snapshot in the body node's frame where it
            // stood; that frame is now the rest one, through the player node.
            const glm::vec3 front=player->transform().rotation*glm::vec3(0,0,1);
            yaw=wrap(std::atan2(front.x,-front.z)/rad);
            return;
        }
        if(state==saida::RagdollBehaviour::State::Animated){playerDown=false;downClock=0;}
    }
    // The nearest mapped hospital, clinic or doctor's (gen/interior_uses.cpp
    // names them all "clinic"), across the resident tiles: he wakes outside
    // its door. With none mapped near, where he fell, and the screen says so.
    void respawn() {
        const glm::dvec3 here=ecef(lon,lat,alt);
        double best=1e30;glm::dvec2 at(lon,lat);std::string name;
        for(auto& [key,t]:loaded)for(const auto& room:t.interiors) {
            if(room.plan.recipe!="clinic")continue;
            const r1::P2 out{room.plan.door.x-room.plan.inward.x*2.5,room.plan.door.y-room.plan.inward.y*2.5};
            const glm::dvec3 local(out.x,room.plan.floor,out.y);
            const auto point=t.frame.origin+t.frame.basis*local;
            const double d=glm::length(point-here);
            if(d<best) {
                best=d;const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
                at={geo.x,geo.y};name=room.plan.name.empty()?"the hospital":room.plan.name;
            }
        }
        playerRagdoll->release();
        playerBody->transform()=playerBodyRest;
        playerDown=wasted=false;downClock=0;
        health.restore();
        lon=at.x;lat=at.y;alt=height(lon,lat);jumpOffset=jumpVelocity=0;
        player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
        arrivalClock=fightClock;
        request(lon,lat);
        respawnSaid=best<1e29?"Waking up outside "+name+".":"No mapped hospital nearby: waking up where you fell.";
        saida::Log::info("[World combat] respawn ",best<1e29?"at "+name:std::string("where he fell")," (",lon,", ",lat,")");
        showWasted(false);
        text("stream-status",respawnSaid);
    }
    void showWasted(bool show) {
        style("wasted","display",show?"block":"none");
        if(show)text("wasted-detail","Waking up at the nearest hospital…");
    }

    // ── the player's weapons ──

    void showHeldPistol() {
        if(pistol)pistol->setVisible(arsenal.held()==r1::combat::Weapon::Pistol&&(!playerBikeRider||aiming));
    }
    void setAiming(bool on) {
        aiming=on;
        if(!on&&playerBikeAim)playerBikeAim->release();
        showHeldPistol();
        style("crosshair","display",on?"block":"none");
        if(!on)for(auto* arm:playerArms)if(arm){arm->release();arm->clearTipRotation();}
    }
    void selectWeapon(r1::combat::Weapon wanted) {
        if(arsenal.held()==wanted)return;
        arsenal.select(wanted);
        showHeldPistol();
        if(wanted!=r1::combat::Weapon::Pistol&&aiming)setAiming(false);
    }
    // The physics filter of a shot or a punch: everything but his own feet.
    saida::QueryFilter combatFilter() const {
        saida::QueryFilter filter;filter.ignoreInner=feet->innerBodyId();
        if(ridingBike())if(auto* body=r1::VehicleRuntime::body(*car))filter.ignore=body->bodyId();
        return filter;
    }
    // A passer-by, by the capsule or ragdoll part a query hit.
    bool personByBody(const saida::RaycastHit& hit,Loaded*& outTile,size_t& outSlot) {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return false;
        auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
        for(auto tile:ring) {
            auto* t=tileByKey(tile.key());if(!t)continue;
            for(size_t i=0;i<t->people.size();++i) {
                const Person& p=t->people[i];
                if(!p.node||!p.node->enabled())continue;
                if((node&&p.body==node)||(p.ragdoll&&p.ragdoll->ragdoll()&&p.ragdoll->ragdoll()->partOf(hit.body)>=0)) {
                    outTile=t;outSlot=i;return true;
                }
            }
        }
        return false;
    }
    // Witnesses: everyone near a blow or a shot runs from it.
    void alarm(const glm::vec3& at,double radius,double seconds) {
        for(auto tile:ring) {
            auto* t=tileByKey(tile.key());if(!t)continue;
            const glm::dvec3 local=t->frame.local(origin.origin+origin.basis*glm::dvec3(at));
            t->crowd.scare(local.x,local.z,radius,seconds);
        }
    }
    // A person struck (fist or round): health, then a stagger, a fall or death.
    void strikePerson(Loaded& tile,size_t slot,double damage,const glm::vec3& direction,const glm::vec3& point,
                      float impulse,bool always) {
        Person& p=tile.people[slot];
        const auto& walkers=tile.crowd.walkers();
        if(slot>=walkers.size()||!walkers[slot].alive)return;
        const bool wasDown=walkers[slot].activity==r1::Activity::Down;
        tile.crowd.damage(slot,damage);
        const r1::Walker& w=walkers[slot];
        effects.burst(r1::CombatEffects::Blood,point);
        if(wasDown) {
            // Already on the ground: a round moves the body again.
            if(p.ragdoll&&p.ragdoll->state()!=saida::RagdollBehaviour::State::Simulated){limitRagdolls();p.ragdoll->activate();}
            if(p.ragdoll)p.ragdoll->applyImpulse(direction*impulse,point);
            p.lying=0;
            return;
        }
        if(always||w.dead||w.health<100.*r1::combat::kKnockDownBelow) {
            const glm::vec3 spin=glm::cross(glm::vec3(0,1,0),direction)*2.5f;
            if(knockDown(tile,slot,direction*2.2f+glm::vec3(0,.6f,0),spin)) {
                p.ragdoll->applyImpulse(direction*impulse,point);
                sounds.play(r1::CombatSounds::Fall,glm::length(camera->transform().position-point),.7f);
            }
            return;
        }
        if(p.impact) {
            const glm::quat facing=p.node->transform().rotation;
            const glm::dvec3 d=toTile(tile,direction);
            p.impact->push(glm::inverse(facing)*glm::vec3(float(d.x),0.f,float(d.z)),7.f);
        }
        tile.crowd.provoke(slot);
    }

    void throwPunch() {
        if(!punch.throwAt(fightClock))return;
        sounds.play(r1::CombatSounds::Swing,followDistance,.7f);
        // The nearest person in front within reach, their head the target.
        const glm::vec3 me=player->transform().position;
        const glm::vec3 ahead(float(std::sin(yaw*rad)),0.f,float(-std::cos(yaw*rad)));
        double best=r1::combat::kFists.range*kPeopleVehicleResize+.25;punchAimed=false;
        for(auto tile:ring) {
            auto* t=tileByKey(tile.key());if(!t)continue;
            const auto& walkers=t->crowd.walkers();
            for(size_t i=0;i<t->people.size()&&i<walkers.size();++i) {
                const Person& p=t->people[i];
                if(!p.node||!p.node->enabled()||!walkers[i].alive||walkers[i].activity==r1::Activity::Down)continue;
                const glm::vec3 d=nodePosition(*p.node)-me;
                const double flat=std::hypot(d.x,d.z);
                if(flat>=best||flat<1e-3)continue;
                if((d.x*ahead.x+d.z*ahead.z)/flat<.45)continue;
                best=flat;punchTileKey=tile.key();punchSlot=i;punchAimed=true;
            }
        }
        // Squared up to whom he hits.
        if(punchAimed)if(auto* t=tileByKey(punchTileKey)) {
            const glm::vec3 d=nodePosition(*t->people[punchSlot].node)-me;
            yaw=wrap(std::atan2(d.x,-d.z)/rad);
        }
    }
    void landPunch() {
        Loaded* t=punchAimed?tileByKey(punchTileKey):nullptr;
        if(!t||punchSlot>=t->people.size()||!t->people[punchSlot].node)return;
        const Person& p=t->people[punchSlot];
        const glm::vec3 me=player->transform().position,them=nodePosition(*p.node);
        glm::vec3 d=them-me;d.y=0;
        if(glm::length(d)>r1::combat::kFists.range*kPeopleVehicleResize+.4)return;  // they stepped out of reach
        d=glm::normalize(d);
        const glm::vec3 point=them+glm::vec3(0.f,float(playerHeight*(punch.hook()?.82:.88)),0.f);
        sounds.play(r1::CombatSounds::Punch,followDistance);
        alarm(them,r1::combat::kFightSeen,6.);
        strikePerson(*t,punchSlot,punch.damage(),d,point,float(r1::combat::kFists.impulse*(punch.hook()?1.6:1.)),punch.hook());
    }

    void fire() {
        const auto result=arsenal.pull();
        if(result==r1::combat::Arsenal::Trigger::Empty){sounds.play(r1::CombatSounds::DryFire);if(arsenal.reloading())sounds.play(r1::CombatSounds::Reload,0,.8f);return;}
        if(result!=r1::combat::Arsenal::Trigger::Fired)return;
        if(arsenal.reloading())sounds.play(r1::CombatSounds::Reload,0,.8f);
        auto* physics=engine.sceneTree().world().physics();
        const glm::vec3 eye=camera->transform().position;
        // Aim error grows with movement and with the last shot's recoil.
        static uint32_t spreadSeed=0x9e3779b9u;
        auto random=[&]{spreadSeed^=spreadSeed<<13;spreadSeed^=spreadSeed>>17;spreadSeed^=spreadSeed<<5;return double(spreadSeed)/4294967296.;};
        const double spread=r1::combat::kPistol.spread*(1+2.5*std::min(1.,(ridingBike()?std::abs(carSpeed)/18.:std::hypot(footEast,footNorth)/3.))+recoil*4);
        const double a=random()*6.283185307179586,rr=std::sqrt(random())*spread;
        const glm::quat jitter=glm::angleAxis(float(rr*std::cos(a)),glm::vec3(0,1,0))*glm::angleAxis(float(rr*std::sin(a)),glm::vec3(1,0,0));
        const glm::vec3 look=glm::normalize(camera->transform().rotation*(jitter*glm::vec3(0,0,-1)));
        const float range=float(r1::combat::kPistol.range);
        glm::vec3 aimPoint=eye+look*range;
        if(physics){const auto hit=physics->raycast(eye+look*float(followDistance*.8),look,range,combatFilter());if(hit.hit)aimPoint=hit.point;}
        // The round leaves the muzzle for whatever the camera put under the
        // crosshair; a wall between the two takes it instead.
        const glm::vec3 muzzle=pistol?glm::vec3(pistol->worldTransform()*glm::vec4(r1::kPistolMuzzle,1.f)):eye;
        const glm::vec3 path=aimPoint-muzzle;
        const float length=glm::length(path);
        effects.muzzle(muzzle);
        sounds.play(r1::CombatSounds::Shot);
        recoil=std::min(1.,recoil+.55);
        pitch=std::clamp(pitch+1.1,-65.,25.);
        alarm(ridingBike()?car->transform().position:player->transform().position,r1::combat::kGunshotHeard,10.);
        if(!physics||length<1e-3)return;
        const glm::vec3 dir=path/length;
        const auto hit=physics->raycast(muzzle,dir,length+.05f,combatFilter());
        if(!hit.hit)return;
        const double heard=glm::length(eye-hit.point);
        Loaded* t=nullptr;size_t slot=0;
        if(personByBody(hit,t,slot)) {
            const float feet=nodePosition(*t->people[slot].node).y;
            const bool head=hit.point.y-feet>float(playerHeight*.84);
            strikePerson(*t,slot,r1::combat::kPistol.damage*(head?r1::combat::kPistol.headMultiplier:1.),dir,hit.point,
                         float(r1::combat::kPistol.impulse),true);
            sounds.play(r1::CombatSounds::Impact,heard,.5f);
            return;
        }
        auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
        if(node&&node->name()==r1::VehicleRuntime::kBodyName) {
            effects.burst(r1::CombatEffects::Sparks,hit.point);
            if(auto* dent=node->getBehaviour<saida::DeformableMeshBehaviour>()) {
                const glm::mat4 toBody=glm::inverse(node->worldTransform());
                dent->applyImpact(glm::vec3(toBody*glm::vec4(hit.point,1.f)),glm::normalize(glm::vec3(toBody*glm::vec4(dir,0.f))),
                                  dent->yieldEnergy*4.f);
            }
            physics->applyImpulse(hit.body,dir*float(r1::combat::kPistol.impulse),hit.point);
            sounds.play(r1::CombatSounds::Impact,heard);
            return;
        }
        effects.burst(r1::CombatEffects::Dust,hit.point+hit.normal*.03f);
        // A loose body is pushed; a static one absorbs it (PhysicsWorld).
        physics->applyImpulse(hit.body,dir*float(r1::combat::kPistol.impulse),hit.point);
        sounds.play(r1::CombatSounds::Impact,heard);
    }

    // The arms each frame: the aim holds the pistol on the crosshair, a punch
    // reaches for its target, and both ease out when done.
    void poseArms() {
        auto* animator=animators.front();
        if(!animator||!playerBody)return;
        const glm::mat4 toObject=glm::inverse(playerBody->worldTransform());
        const auto& pose=animator->globalPose().globalMatrices;
        const saida::Rig& rig=*animator->rig();
        const int shoulderBone[2]={rig.findBoneIndex("Bip01 L UpperArm"),rig.findBoneIndex("Bip01 R UpperArm")};
        if(shoulderBone[0]<0||shoulderBone[1]<0||pose.size()<rig.boneCount())return;
        const glm::vec3 shoulder[2]={glm::vec3(pose[size_t(shoulderBone[0])][3]),glm::vec3(pose[size_t(shoulderBone[1])][3])};
        if(aiming&&arsenal.held()==r1::combat::Weapon::Pistol) {
            const glm::vec3 look=camera->transform().rotation*glm::vec3(0,0,-1);
            glm::vec3 aim=glm::normalize(glm::vec3(toObject*glm::vec4(look,0.f)));
            if(ridingBike()){playerBikeAim->lookAt(aim);playerBikeLook->lookAt(shoulder[1]+aim*100.f);}
            const glm::vec3 up(0,1,0);
            // Out in front at eye height, the arm nearly straight.
            const float overShoulder=ridingBike()?.18f*std::clamp(-aim.z*2.f,0.f,1.f):0.f;
            const glm::vec3 grip=shoulder[1]+aim*(playerReach*.9f)+glm::vec3(-.05f,overShoulder,0.f);
            glm::vec3 side=glm::cross(up,aim);if(glm::dot(side,side)<1e-6f)side=glm::vec3(1,0,0);
            side=glm::normalize(side);
            const glm::vec3 top=glm::cross(aim,side);
            glm::mat3 gun;gun[0]=side;gun[1]=top;gun[2]=aim;
            const glm::quat hand=glm::normalize(glm::quat_cast(gun)*glm::inverse(pistolGrip.rotation));
            playerArms[1]->reach(grip);playerArms[1]->setTipRotation(hand);
            playerArms[1]->setPole(shoulder[1]-side*.4f-top*.5f+aim*.12f);
            // Riders keep their left hand on the handlebar.
            if(ridingBike())return;
            playerArms[0]->reach(grip+side*.035f-top*.045f-aim*.02f);playerArms[0]->clearTipRotation();
            playerArms[0]->setPole(shoulder[0]+glm::vec3(.4f,-.6f,-.1f));
            return;
        }
        for(int h=0;h<2;++h) {
            auto* arm=playerArms[h];
            const bool throwing=punch.active()&&(punch.rightHand()==(h==1));
            if(!throwing){if(arm->weight()>0)arm->release();continue;}
            glm::vec3 target=shoulder[h]+glm::vec3(h?-.08f:.08f,.05f,playerReach*.95f);
            if(Loaded* t=punchAimed?tileByKey(punchTileKey):nullptr)if(punchSlot<t->people.size()&&t->people[punchSlot].node) {
                const glm::vec3 head=nodePosition(*t->people[punchSlot].node)+glm::vec3(0.f,float(playerHeight*(punch.hook()?.8:.88)),0.f);
                target=glm::vec3(toObject*glm::vec4(head,1.f));
            }
            const float w=float(punch.extension());
            arm->clearTipRotation();arm->reach(target,w);arm->setWeightNow(w);
            arm->setPole(shoulder[h]+glm::vec3(h?-.45f:.45f,-.35f,-.25f));
        }
    }

    // A car driven into people: whoever stands in its path is thrown over
    // the bonnet, harder the faster it goes.
    void runOverPeople(double dt) {
        if(!driving||!car||std::abs(carSpeed)<2.5)return;
        auto* body=r1::VehicleRuntime::body(*car);
        if(!body||!body->physicsWorld())return;
        const glm::vec3 velocity=body->physicsWorld()->linearVelocity(body->bodyId());
        const float speed=glm::length(velocity);
        if(speed<2.5f)return;
        const auto& spec=vehicleSpec(*car);
        const glm::mat4 world=car->worldTransform();
        const glm::mat4 toCar=glm::inverse(world);
        const float halfWidth=float(spec.width*.5+.25),halfLength=float(spec.length*.5),reach=float(speed*dt*1.6+.35);
        const glm::vec3 dir=velocity/speed;
        for(auto tile:ring) {
            auto* t=tileByKey(tile.key());if(!t)continue;
            const auto& walkers=t->crowd.walkers();
            for(size_t i=0;i<t->people.size()&&i<walkers.size();++i) {
                Person& p=t->people[i];
                if(!p.node||!p.node->enabled()||!walkers[i].alive||walkers[i].activity==r1::Activity::Down)continue;
                const glm::vec3 at=nodePosition(*p.node);
                const glm::vec3 local=glm::vec3(toCar*glm::vec4(at,1.f));
                if(std::abs(local.x)>halfWidth||local.y<-1.f||local.y>float(spec.height+.5))continue;
                if(local.z<-halfLength-reach||local.z>halfLength+reach)continue;
                if(glm::dot(at-glm::vec3(world[3]),dir)<0.f)continue;  // behind the car as it goes
                const auto hit=r1::combat::carImpact(speed);
                if(!hit.knockDown)continue;
                t->crowd.damage(i,hit.damage);
                // Legs taken forward, the body over the bonnet.
                const glm::vec3 spin=glm::cross(dir,glm::vec3(0,1,0))*std::clamp(speed*.55f,2.f,9.f);
                if(knockDown(*t,i,velocity*.85f+glm::vec3(0,1.2f+.1f*speed,0),spin)) {
                    sounds.play(r1::CombatSounds::CarHit,followDistance);
                    effects.burst(r1::CombatEffects::Blood,at+glm::vec3(0.f,float(playerHeight*.5),0.f));
                    alarm(at,r1::combat::kFightSeen*1.5,8.);
                    saida::Log::info("[World combat] a passer-by run over at ",speed*3.6," km/h, damage ",hit.damage);
                }
            }
        }
    }

    void updateCombat(double dt) {
        SAIDA_PROFILE_SCOPE("World/Combat");
        fightClock+=dt;
        if(health.invincible()) {
            health.tick(dt);
            if(!health.invincible()){cheatSay("Invincibility over.");saida::Log::info("[World cheat] invincibility over");}
        }
        recoil=std::max(0.,recoil-dt*2.2);
        arsenal.update(dt);
        effects.update(float(dt));
        runOverPeople(dt);
        if(playerDown)updatePlayerDown(dt);
        const bool onFoot=!driving&&!sailing&&!piloting&&!swimming&&!playerDown&&player->enabled();
        const bool onBike=ridingBike()&&playerBikeRider&&!playerDown;
        if((!onFoot&&!onBike)||menu||smoke) {
            if(aiming)setAiming(false);
            wasFire=clicked(GLFW_MOUSE_BUTTON_LEFT);
            if(!onFoot)for(auto* arm:playerArms)if(arm&&arm->weight()>0&&!playerDown)arm->release();
            if(onFoot&&smoke)poseArms();
            if(onBike)bikeHandlePose();
            return;
        }
        const bool keys[2]={held(GLFW_KEY_1),held(GLFW_KEY_2)};
        if(keys[0]&&!wasWeaponKey[0])selectWeapon(r1::combat::Weapon::Fists);
        if(keys[1]&&!wasWeaponKey[1])selectWeapon(r1::combat::Weapon::Pistol);
        wasWeaponKey[0]=keys[0];wasWeaponKey[1]=keys[1];
        const bool reloadKey=held(GLFW_KEY_E);
        if(reloadKey&&!wasReloadKey&&arsenal.reload())sounds.play(r1::CombatSounds::Reload,0,.8f);
        wasReloadKey=reloadKey;
        const bool wantAim=arsenal.held()==r1::combat::Weapon::Pistol&&clicked(GLFW_MOUSE_BUTTON_RIGHT);
        if(wantAim!=aiming)setAiming(wantAim);
        const bool fireDown=clicked(GLFW_MOUSE_BUTTON_LEFT);
        if(fireDown&&!wasFire) {
            if(arsenal.held()==r1::combat::Weapon::Fists){if(onFoot&&arsenal.pull()==r1::combat::Arsenal::Trigger::Fired)throwPunch();}
            else if(onFoot||aiming)fire();
        }
        wasFire=fireDown;
        if(punch.update(dt))landPunch();
        if(onBike){poseArms();bikeHandlePose();return;}
        // Aiming, he faces where the camera looks.
        if(aiming)player->transform().rotation=glm::slerp(player->transform().rotation,
            glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0)),float(1-std::exp(-18*dt)));
        else if(punch.active())player->transform().rotation=glm::slerp(player->transform().rotation,
            glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0)),float(1-std::exp(-20*dt)));
        poseArms();
    }

    // The fight's HUD: health and weapon, changed only when they change (the
    // interface is rasterised whole on every change, WebCanvasNode).
    void combatHud() {
        const int hp=int(std::ceil(health.hp));
        style("health-fill","width",std::to_string(int(std::round(health.fraction()*196)))+"px");
        style("health-fill","background-color",health.invincible()?"#f2d16b":hp>30?"#d5f5a4":"#e8664f");
        style("shield","display",health.invincible()?"block":"none");
        if(health.invincible())text("shield","Invincible · "+r1::cheats::clock(health.shield));
        text("health-value",std::to_string(hp));
        const bool pistolHeld=arsenal.held()==r1::combat::Weapon::Pistol;
        text("weapon-name",r1::combat::spec(arsenal.held()).name);
        text("weapon-ammo",pistolHeld?(arsenal.reloading()?"Reloading…":std::to_string(arsenal.loaded())+" / "+std::to_string(arsenal.reserve())):"");
        style("hurt","display",fightClock-hurtShown<.45?"block":"none");
    }

    // ── cheat codes ─────────────────────────────────────────────────────────
    //
    // GTA's line: T opens it over the HUD, a word and Enter does what
    // native/cheats.hpp says it does, Escape closes it. Open, it has the
    // keyboard -- the engine routes keys and text to the canvas it is told
    // has the focus -- and the game reads none of its own keys (World::held),
    // or "comet" would walk the player forward and toggle the debug panel.
    // A code that cannot be done here says why, on screen and in the log
    // (CLAUDE.md rule 3).
    bool held(int key) const {return !cheatOpen&&engine.window().keyDown(key);}
    bool clicked(int button) const {return !cheatOpen&&engine.window().mouseButtonDown(button);}
    // The process's private memory, in MB: what a bad_alloc runs out of.
    static double privateMegabytes() {
        PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
        return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))
            ?double(counters.PrivateUsage)/1048576.:0.;
    }
    void openCheat() {
        if(!smokeCheatCycles)saida::Log::info("[World cheat] line opened, private memory ",int(privateMegabytes())," MB");
        cheatOpen=true;
        field("cheat-input",std::string());
        style("cheat-said","display","none");
        style("cheat","display","block");
        if(auto* input=ui->findElementById("cheat-input")){input->Focus(true);ui->notifyJsMutation();}
        engine.uiInteraction().focusWebCanvas(ui);
    }
    void closeCheat() {
        cheatOpen=false;
        style("cheat","display","none");
        if(auto* input=ui->findElementById("cheat-input")){input->Blur();ui->notifyJsMutation();}
        engine.uiInteraction().focusWebCanvas(nullptr);
    }
    void cheatSay(const std::string& said) {text("cheat-said",said);cheatSaidAt=clock;}
    void updateCheats(double dt) {
        if(smokeCheatCycles){cycleSmokeCheat(dt);return;}
        auto& w=engine.window();
        const bool key=w.keyDown(GLFW_KEY_T),escape=w.keyDown(GLFW_KEY_ESCAPE),
                   enter=w.keyDown(GLFW_KEY_ENTER)||w.keyDown(GLFW_KEY_KP_ENTER);
        if(cheatOpen) {
            if(enter&&!wasCheatEnter){const std::string typed=value("cheat-input");closeCheat();runCheat(typed);}
            else if(escape&&!wasCheatEscape)closeCheat();
            // A click with the cursor captured lands on no control and takes
            // the canvas's focus away: the line keeps it while it is open.
            else if(engine.uiInteraction().focusedWebCanvas()!=ui)engine.uiInteraction().focusWebCanvas(ui);
        }
        // With the debug panel open, T is its trees (ui/performance.html).
        else if(key&&!wasCheatKey&&!performanceDebug&&!wasted&&!smoke)openCheat();
        wasCheatKey=key;wasCheatEnter=enter;wasCheatEscape=escape;
        style("cheat-said","display",!cheatOpen&&clock-cheatSaidAt<4.?"block":"none");
    }
    void runCheat(const std::string& typed) {
        using r1::cheats::Code;
        const Code code=r1::cheats::parse(typed);
        if(code==Code::Unknown&&typed.find_first_not_of(" \t\r\n")==std::string::npos)return;
        std::string refused;
        switch(code) {
        case Code::Helicopter: refused=spawnAircraft("helicopter");break;
        case Code::Plane: refused=spawnAircraft("bizjet");break;
        case Code::SportsCar: refused=spawnSportsCar();break;
        case Code::Moto: refused=spawnParkedVehicle("moto_city");break;
        case Code::Rocket: refused=spawnParkedVehicle("moto_cross");break;
        case Code::Invincible: health.protect(r1::cheats::kInvincibleSeconds);combatHud();break;
        case Code::Unknown: break;
        }
        cheatSay(code==Code::Unknown?"Unknown code: "+typed:refused.empty()?r1::cheats::said(code):refused);
        saida::Log::info("[World cheat] \"",typed,"\": ",code==Code::Unknown?"unknown":refused.empty()?"done":"refused");
    }
    // Where a vehicle `length` long and `width` wide stands parallel to the
    // player: on loaded ground near his level, out of the water, clear of
    // walls and bodies at every corner, middle and end, and clear of him.
    // Ahead first for an aircraft, so he faces its tail within reach of it;
    // to his right first for a car, where parkCar puts one; then a spiral
    // out to 40 m. `wings`: when nowhere has room for a whole aircraft -- a
    // city street is narrower than a jet's span -- one whose fuselage is
    // clear may stand with its wing tips over a pavement; it taxis out of
    // what it overhangs (World::moveAircraft lets a hull leave an obstacle).
    bool roomBeside(double length,double width,bool ahead,bool wings,double& outLon,double& outLat) {
        const double s=std::sin(yaw*rad),c=std::cos(yaw*rad);
        auto at=[&](double x,double y,double along,double across){return onward(x,y,s*along+c*across,c*along-s*across);};
        auto clear=[&](double x,double y){
            if(!tile(x,y))return false;
            const double level=height(x,y,alt+1.);
            return std::abs(level-alt)<2.*kStepUp&&!blocked(x,y,level);
        };
        const double margin=driving?2.5:1.;
        std::vector<glm::dvec2> spots;  // (along, across) from the player
        if(ahead)spots.push_back({length*.5+3.,0.});
        spots.push_back({0.,width*.5+2.5});spots.push_back({0.,-width*.5-2.5});
        if(!ahead)spots.push_back({length*.5+3.,0.});
        spots.push_back({-length*.5-3.,0.});
        const double inner=std::max(length,width)*.5+2.;
        for(int i=1;i<=160;++i) {
            const double a=i*2.39996323,d=inner+(40.-inner)*std::sqrt(i/160.);
            spots.push_back({d*std::cos(a),d*std::sin(a)});
        }
        for(const bool whole:{true,false}) {
            if(!whole&&!wings)break;
            for(const auto& spot:spots) {
                if(std::abs(spot.x)<length*.5+margin&&std::abs(spot.y)<width*.5+margin)continue;  // on him
                const auto centre=at(lon,lat,spot.x,spot.y);
                bool fits=true;
                for(double along:{-length*.5,0.,length*.5})
                    for(double across:{-width*.5,0.,width*.5}) {
                        if(!fits)break;
                        const auto q=at(centre.x,centre.y,along,across);
                        fits=across!=0.&&!whole?tile(q.x,q.y)!=nullptr:clear(q.x,q.y);
                    }
                // Nor on an aircraft standing there already: they have no body.
                for(const auto& a:leftAircraft)
                    if(fits&&a.type&&metresBetween(a.lon,a.lat,centre.x,centre.y)<(std::max(a.type->length,a.type->span)+std::max(length,width))*.5)fits=false;
                if(!fits)continue;
                outLon=centre.x;outLat=centre.y;
                return true;
            }
        }
        return false;
    }
    // BUZZOFF and PLANE: a helicopter or a business jet beside him, left
    // standing like one he stepped out of -- F takes it, and the oldest of
    // those beyond kLeftAircraft is cleared as always.
    std::string spawnAircraft(const std::string& name) {
        const auto* type=r1::aircraftType(name);
        const auto prototype=aircraftPrototypes.find(name);
        const bool rotor=type&&type->klass=="helicopter";
        if(!type||prototype==aircraftPrototypes.end()) {
            saida::Log::error("[World cheat] the aircraft fleet has no ",name);
            return "This aircraft is not in the fleet.";
        }
        if(piloting&&plane.airborne) {
            saida::Log::info("[World cheat] no ",name," in flight");
            return "Not while flying — land first.";
        }
        double x=0,y=0;
        parkingCar=true;  // a shop a walker may enter is a wall to it
        const bool found=roomBeside(type->length,type->span,true,true,x,y);
        parkingCar=false;
        if(!found) {
            saida::Log::warn("[World cheat] no room for a ",name," (",type->length," x ",type->span,
                             " m) within 40 m of ",lon,", ",lat);
            return std::string("Not enough room here for ")+(rotor?"a helicopter":"a plane")+" — find an open spot.";
        }
        auto node=clonePlant(*prototype->second);
        node->setName("cheat-"+name);
        aircraftLod(*node);
        Aircraft a;
        a.node=engine.sceneTree().world().addChild(std::move(node));
        a.type=type;a.lon=x;a.lat=y;a.yaw=yaw;a.alt=height(x,y,alt+1.);
        collectAircraftParts(a);
        keepAircraft(std::move(a));
        saida::Log::info("[World cheat] a ",name," at ",x,", ",y," beside the player");
        return {};
    }
    // COMET: the fleet's sports car in its reddest paint, parked beside him
    // like a car he left; F takes it.
    std::string spawnSportsCar() {
        const auto spec=std::find_if(fleet.begin(),fleet.end(),[](const VehicleModel& m){return m.name=="sport";});
        if(spec==fleet.end()) {
            saida::Log::error("[World cheat] the road vehicle fleet has no sport car");
            return "No sports car in the fleet.";
        }
        if(piloting&&plane.airborne) {
            saida::Log::info("[World cheat] no car in flight");
            return "Not while flying — land first.";
        }
        double x=0,y=0;
        parkingCar=true;
        const bool found=roomBeside(spec->length,spec->width,false,false,x,y);
        parkingCar=false;
        if(!found) {
            saida::Log::warn("[World cheat] no room for a car within 40 m of ",lon,", ",lat);
            return "Not enough room here for a car.";
        }
        auto node=clonePlant(*spec->prototype);
        vehicleLod(*node);vehicleCollider(*node,*spec);
        const auto red=std::max_element(paints.begin(),paints.end(),[](glm::vec3 a,glm::vec3 b){
            return a.x/(a.y+a.z+.01f)<b.x/(b.y+b.z+.01f);});
        if(red!=paints.end())paintCar(*node,*red);
        auto* root=engine.sceneTree().world().addChild(std::move(node));
        if(auto* drive=r1::VehicleRuntime::driver(*root))drive->parked=true;
        keepParked({root,x,y,groundAt(x,y,alt),yaw});
        placeCar();
        saida::Log::info("[World cheat] a sport car at ",x,", ",y," beside the player");
        return {};
    }
    // MOTO and ROCKET: spawn the plain city bike or the motocross bike from
    // the same light-weight fleet used by traffic, ready for F to take it.
    std::string spawnParkedVehicle(const std::string& name) {
        const auto spec=std::find_if(fleet.begin(),fleet.end(),[&](const VehicleModel& m){return m.name==name;});
        if(spec==fleet.end()) {
            saida::Log::error("[World cheat] the road vehicle fleet has no ",name);
            return name=="moto_cross"?"No dirt bike in the fleet.":"No city motorcycle in the fleet.";
        }
        if(piloting&&plane.airborne) {
            saida::Log::info("[World cheat] no ",name," while in flight");
            return "Not while flying — land first.";
        }
        double x=0,y=0;
        parkingCar=true;
        const bool found=roomBeside(spec->length,spec->width,false,false,x,y);
        parkingCar=false;
        if(!found) {
            saida::Log::warn("[World cheat] no room for a ",name," within 40 m of ",lon,", ",lat);
            return name=="moto_cross"?"Not enough room here for a dirt bike.":"Not enough room here for a motorcycle.";
        }
        auto node=clonePlant(*spec->prototype);
        vehicleCollider(*node,*spec);
        auto* root=engine.sceneTree().world().addChild(std::move(node));
        if(auto* drive=r1::VehicleRuntime::driver(*root))drive->parked=true;
        keepParked({root,x,y,groundAt(x,y,alt),yaw});
        placeCar();
        saida::Log::info("[World cheat] a ",name," at ",x,", ",y," beside the player");
        return {};
    }

    // ── the smoke's fight ──
    //
    // After the walk: the nearest passer-by is knocked down by the code path a
    // punch or a car takes, and must tumble in the engine's physics, come to
    // rest, and freeze; a round fired at the ground must land. A refusal at
    // any step names itself (CLAUDE.md rule 3).
    void startSmokeFight() {
        smokeFought=true;
        // People appear out of sight (r1::Crowd::kSpawnNear and beyond): the
        // nearest anywhere in the neighbourhood will do for the physics.
        const glm::vec3 me=player->transform().position;
        const double reach=r1::Crowd::kDespawn;
        double best=reach;
        for(auto& [key,t]:loaded) {
            const auto& walkers=t.crowd.walkers();
            for(size_t i=0;i<t.people.size()&&i<walkers.size();++i) {
                if(!t.people[i].node||!t.people[i].node->enabled()||!walkers[i].alive)continue;
                const double d=glm::length(nodePosition(*t.people[i].node)-me);
                if(d<best){best=d;smokeFightTile=key;smokeFightSlot=i;}
            }
        }
        if(best>=reach) {
            saida::Log::info("[World E2E] fight: nobody in the neighbourhood to knock down; skipped");
            if(!smokeDroveOnce&&!onSeaIce(lon,lat))startSmokeDrive();else smokeFinishPhase();
            return;
        }
        Loaded* t=tileByKey(smokeFightTile);
        const glm::vec3 d=glm::normalize(nodePosition(*t->people[smokeFightSlot].node)-me+glm::vec3(0,.001f,0));
        if(!knockDown(*t,smokeFightSlot,d*3.f+glm::vec3(0,1.5f,0),glm::cross(glm::vec3(0,1,0),d)*3.f)) {
            saida::Log::error("[World E2E] FAIL fight: the knocked-down body did not enter the physics");
            testFailed=true;engine.sceneTree().quit();return;
        }
        selectWeapon(r1::combat::Weapon::Pistol);
        smokeFighting=true;smokeFightTime=0;smokeFightPhase=0;
        saida::Log::info("[World E2E] fight: knocked down a passer-by ",best," m away");
    }
    void runSmokeFight(double dt) {
        smokeFightTime+=dt;
        Loaded* t=tileByKey(smokeFightTile);
        if(!t||smokeFightSlot>=t->people.size()||!t->people[smokeFightSlot].ragdoll) {
            saida::Log::error("[World E2E] FAIL fight: the knocked-down body's tile left before it came to rest");
            testFailed=true;engine.sceneTree().quit();return;
        }
        auto* ragdoll=t->people[smokeFightSlot].ragdoll;
        if(smokeFightPhase==0) {
            if(ragdoll->state()==saida::RagdollBehaviour::State::Simulated&&smokeFightTime<8.)return;
            const auto state=ragdoll->state();
            if(state==saida::RagdollBehaviour::State::Simulated) {
                saida::Log::error("[World E2E] FAIL fight: the body still tumbles after 8 s (fastest part ",
                                  ragdoll->ragdoll()->fastestSpeed()," m/s)");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] fight: the body came to rest in ",smokeFightTime," s (",
                             state==saida::RagdollBehaviour::State::Frozen?"frozen":"getting up",")");
            const size_t magazine=size_t(arsenal.loaded());
            fire();
            if(size_t(arsenal.loaded())+1!=magazine) {
                saida::Log::error("[World E2E] FAIL fight: the pistol did not fire (",arsenal.loaded()," of ",magazine,")");
                testFailed=true;engine.sceneTree().quit();return;
            }
            smokeFightPhase=1;smokeFightTime=0;
            return;
        }
        if(smokeFightTime<1.)return;
        selectWeapon(r1::combat::Weapon::Fists);
        smokeFighting=false;
        saida::Log::info("[World E2E] PASS fight: knock-down, rest and a pistol round");
        if(!smokeDroveOnce&&!onSeaIce(lon,lat)){startSmokeDrive();return;}
        smokeFinishPhase();
    }
    size_t crowdWanted() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=size_t(tile.crowd.wanted());
        return n;
    }
    size_t crowdLive() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=size_t(tile.crowd.live());
        return n;
    }
    // Tiles are cooked in this process, on the service's threads (gen/).
    std::unique_ptr<r1::WorldService> service;
    std::map<std::string,saida::AssetID> textures;
    size_t residentVertexBudget=0,residentIndexBudget=0;
    bool buildingTrimPending=false;
    // A tile was mounted: it may be the one the player stands on, cooked
    // again with the buildings its first, provisional cook did not have.
    bool checkStanding=false;
    Frame origin; double lon=2.3522,lat=48.8566,alt=0,yaw=0,pitch=-12;
    double pickLon=2.3522,pickLat=48.8566,zoom=1,mapCenterLon=2.3522,mapCenterLat=48.8566;
    // OpenStreetMap allows two connections (tile usage policy); EOX serves the
    // Sentinel-2 cloudless 2016 mosaic, CC BY 4.0, under its attribution.
    MenuTileCache menuTiles{game,{"osm",".png",[](const r1::MapTile& t){
        return "https://tile.openstreetmap.org/"+std::to_string(t.z)+"/"+std::to_string(t.x)+"/"+std::to_string(t.y)+".png";},2}};
    MenuTileCache imageryTiles{game,{"s2cloudless-2016",".jpg",[](const r1::MapTile& t){
        return "https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless_3857/default/g/"+std::to_string(t.z)+"/"+std::to_string(t.y)+"/"+std::to_string(t.x)+".jpg";},6}};
    std::shared_ptr<r1::MenuGlobeState> globe;
    r1::GlobeView globeTarget,globeDragView;
    glm::dvec2 globePress{};
    bool globeDragging=false,globeMoved=false;
    bool menu=true,playing=false,pending=false,warming=false,wasMenuKey=false;
    bool performanceDebug=false,debugTraffic=true,debugGrass=true,debugWater=true,debugTrees=true;
    saida::WebCanvasNode* performanceUi=nullptr;
    bool performanceKeys[5]{};
    double performanceSeconds=0; size_t performanceFrames=0;
    static constexpr double kPerformanceRefreshSeconds=.5;
    bool performanceSmoke=false; int performanceSmokePhase=0; double performanceSmokeWait=0;
    std::chrono::steady_clock::time_point goStarted;
    double lastMountMs=0;
    // How smooth the arrival was: frames over 33 ms in the ten seconds after
    // Go, and the worst of them. The one number "it freezes" becomes.
    std::chrono::steady_clock::time_point lastFrame{};
    int arrivalFrames=0,arrivalHitches=0; double arrivalWorst=0; bool arrivalSaid=true;
    // What the last update spent, by step, so a slow arrival frame says why.
    struct FrameCost { double stream=0,parts=0,warm=0,props=0,distant=0,world=0; } cost;
    template<class F> double timed(F&& f) {
        const auto t=std::chrono::steady_clock::now();f();
        return msSince(t);
    }
    double poll=0,hud=0,smokeWalk=0; std::string requested;
    // The nine tiles around the player, computed once a frame. `nearby` sorts
    // and allocates, and three callers wanted the same answer.
    std::vector<Tile> ring;
    // Index work accumulated across refreshes, including render-time LOD changes.
    uint64_t indexedAtFrame=0; double churn=0,churnFrames=0,dirtyFrames=0;
    bool smoke=false,smokeStarted=false,smokeWaterSpawn=false; glm::dvec3 smokeStart;
    const bool idleSmoke=std::getenv("R1WORLD_IDLE_SMOKE")!=nullptr;
    const std::string perfSmoke=std::getenv("R1WORLD_PERF_SMOKE")?std::getenv("R1WORLD_PERF_SMOKE"):"";
    bool perfReady=false;
    double perfReadyWait=0;
    const bool qualitySwitchSmoke=std::getenv("R1WORLD_QUALITY_SWITCH_SMOKE")!=nullptr;
    int qualitySwitches=0;
    double qualitySwitchWait=0;
    // R1WORLD_WADE_SMOKE="<lon> <lat>": walk from the spawn to that point and
    // fail the moment he swims. Across a breakwater, a rock or the swash the
    // walk must stay a walk while less than half of him is under the water.
    const char* wadeSmoke=std::getenv("R1WORLD_WADE_SMOKE");
    std::optional<glm::dvec2> wadeTo;
    double wadeOverWater=0,wadeDeepest=0;
    bool idleSmokeStopped=false;
    glm::dvec3 idleSmokeStart{};
    double idleSmokeDrift=0;
    bool testFailed=false,testResume=false; double testElapsed=0,resumeWait=0;
    // Set when a tile the spawn is waiting for will not fit in the resident
    // budget. Without it the spawn simply never completes: `surroundingReady`
    // wants all nine tiles, one of them never arrives, and the only report is
    // a data timeout three minutes later blaming the network for a decision
    // this process made in one line. Cleared at the start of every spawn.
    std::string refused;
    // Actions answer the press immediately. Its matching click must not run
    // them again, even after a long hold or a slow frame; keyboard-generated
    // clicks without a matching press still activate the control.
    double clock=0; std::string pressedId;
    std::string cityInput,citySubmitted,cityInFlight;
    double cityChangedAt=0,cityLastRequestAt=-100;
    std::future<std::vector<r1::PlaceChoice>> cityFuture;
    std::vector<r1::PlaceChoice> cityChoices;
    std::map<std::string,std::vector<r1::PlaceChoice>> cityCache;
    // The driver used to test one cold spawn and stop. Teleporting from a
    // place you are already standing in is a different path -- tiles are
    // evicted, the resource arena is trimmed and the origin moves half a
    // planet -- and it is the path a player takes every time after the
    // first. --spawn2 makes the run do it.
    double hopLon=0,hopLat=0,hopWait=0; bool hopWanted=false,hopDone=false,hopArmed=false;
    std::optional<double> smokeArrivalHeading;
    saida::ScriptBehaviour* sunScript=nullptr; bool sunReported=false;
    double subsolarAge=1e9; bool subsolarRefused=false;
    // Options screen. `forcedMinutes` is "Forcer l'heure à" (minutes after
    // local midnight), kept in cache/options.json between runs; `forcedSent`
    // is the instant last handed to the Sun, so the script is told only when
    // the hour or the time zone under the player changes.
    std::optional<int> forcedMinutes; std::optional<double> forcedSent; bool forcedSaid=false,optionsOpen=false;
    r1::WeatherOverride forcedWeather=r1::WeatherOverride::Real;
    bool toasterMode=false;
    int standardWidth=1600,standardHeight=900;
    saida::CaptureRequest worldCapture;
    saida::runtime::CaptureViewpoint captureView;
    bool captureVehicleDent=false,motorcycleCaptureStarted=false,motorcycleCaptureFired=false;
    double motorcycleCaptureWait=0;
    double captureVehicleWait=0;
    bool captureQueued=false;
    // --at <unix seconds>: an inspection capture, lit at that instant under a
    // clear sky and without the HUD, so two builds photograph the same place
    // in the same light (tools/gallery.py). Without it a capture keeps the
    // real instant it started at and today's weather.
    std::optional<double> inspectAt;
    // Cloud fraction, rain mm/h, visibility metres (zero keeps clear-air
    // extinction). Inspection weather never replaces live observations.
    std::array<double,3> inspectWeather{0.,0.,0.};
    std::optional<double> inspectTemperature;
    // --camera-altitude <metres>: the viewpoint's heights are above the sea
    // rather than above the player's feet. A capture laid beside a photograph
    // stands where the photographer stood, whatever this build's relief says
    // the ground there is (tools/gallery.py's reference views).
    std::optional<double> captureAltitude;
    // Photo camera coordinates, independent of gameplay's safe spawn and
    // keepStanding relocation. z is the eye's height above surveyed ground.
    std::optional<std::array<double,3>> captureGeo;
    double captureWait=0,captureReport=0;
    std::string number(double n,int precision=6) {std::ostringstream s;s<<std::fixed<<std::setprecision(precision)<<n;return s.str();}
    // Every write re-lays and re-renders the whole interface -- the map is
    // 1.3 million pixels, some 240 ms -- so a write that changes nothing is
    // not made. The status line used to be rewritten every 16 ms while a
    // destination loaded: that was the freeze after Go.
    std::map<std::string,std::string> written;
    void style(const std::string& id,const std::string& key,const std::string& value) {
        auto& last=written["style:"+id+":"+key];
        if(last==value)return;
        if(auto* e=ui->findElementById(id)){e->SetProperty(key,value);ui->notifyJsMutation();last=value;}
    }
    void text(const std::string& id,const std::string& s) {
        auto& last=written["text:"+id];
        if(last==s)return;
        // setElementText escapes a straight apostrophe to &apos;, which the
        // interface then shows as written ("l&apos;eau"): French is written
        // with the typographic one anyway.
        std::string shown=s;
        for(size_t at;(at=shown.find('\''))!=std::string::npos;)shown.replace(at,1,"’");
        if(ui->setElementText(id,shown))last=s;
        if(id=="status")style("status","display",s.empty()?"none":"block");
    }
    void miniText(const std::string& id,const std::string& s) {
        auto& last=written["mini:text:"+id];
        if(last==s)return;
        std::string shown=s;
        for(size_t at;(at=shown.find('\''))!=std::string::npos;)shown.replace(at,1,"’");
        if(minimapUi->setElementText(id,shown))last=s;
    }
    void layoutMinimap() {
        const float scale=ui->screenSize().x/1440.f;
        const glm::vec2 edge=ui->screenPosition();
        const glm::vec3 position(edge.x+32.f*scale,edge.y+(900.f-28.f-344.f)*scale,0.f);
        auto& transform=minimapUi->transform();
        if(glm::distance(transform.position,position)>.1f)transform.position=position;
        if(std::abs(transform.scale.x-scale)>.001f)transform.scale=glm::vec3(scale);
    }
    void updateMinimap() {
        layoutMinimap();
        std::vector<const r1::MiniMapTile*> tiles;
        tiles.reserve(loaded.size());
        for(const auto& [key,entry]:loaded)tiles.push_back(&entry.served->cooked.minimap);
        const std::string zone=localConditions()?conditions.value("timezone",std::string()):"";
        miniText("mini-time",localClock());
        miniText("mini-place",r1::miniMapPlace(tiles,zoneCountries,countryNames,zone,lon,lat));
        if(minimapDrawnRevision!=minimapRevision||metresBetween(minimapLon,minimapLat,lon,lat)>8.) {
            const std::string roads=r1::miniMapRoadRml(tiles,lon,lat);
            auto& last=written["mini:roads"];
            if(last!=roads&&minimapUi->setElementRml("mini-roads",roads))last=roads;
            minimapLon=lon;minimapLat=lat;minimapDrawnRevision=minimapRevision;
        }
        const double heading=driving?carYaw:sailing?boat.yaw:piloting?plane.yaw:yaw;
        const std::string rotation="rotate("+number(heading,0)+"deg)";
        auto& previous=written["mini:heading"];
        if(previous!=rotation)if(auto* marker=minimapUi->findElementById("mini-player")) {
            marker->SetProperty("transform",rotation);minimapUi->notifyJsMutation();previous=rotation;
        }
    }
    std::string value(const std::string& id) {
        auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));return e?e->GetValue():"";
    }
    // Compared with what the field shows, not with what was last written: the
    // player may have typed in it since.
    void field(const std::string& id,const std::string& value) {
        auto* e=dynamic_cast<Rml::ElementFormControl*>(ui->findElementById(id));
        if(!e||e->GetValue()==value)return;
        e->SetValue(value);
        ui->notifyJsMutation();
    }
    void field(const std::string& id,double x) {field(id,number(x));}
    void showCityChoices(const std::vector<r1::PlaceChoice>& choices,const std::string& feedback={}) {
        cityChoices=choices;
        style("city-results","display",choices.empty()&&feedback.empty()?"none":"block");
        style("city-feedback","display",feedback.empty()?"none":"block");
        if(!feedback.empty())text("city-feedback",feedback);
        for(size_t i=0;i<5;++i) {
            const std::string id="city-choice-"+std::to_string(i);
            style(id,"display",i<choices.size()?"block":"none");
            if(auto* e=ui->findElementById(id))e->SetAttribute("data-available",i<choices.size()?"true":"false");
            if(i<choices.size())text(id,choices[i].label);
        }
    }
    void updateCityLookup() {
        if(menu) {
            std::string query=value("city-query");
            const auto first=query.find_first_not_of(" \t\r\n");
            query=first==std::string::npos?"":query.substr(first,query.find_last_not_of(" \t\r\n")-first+1);
            if(query!=cityInput) {
                cityInput=query;cityChangedAt=clock;touchGlobe();
                showCityChoices({},query.size()<2?"":"Searching…");
            }
        }
        if(cityFuture.valid()&&cityFuture.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
            const std::string completed=cityInFlight;
            cityInFlight.clear();
            try {
                auto choices=cityFuture.get();
                if(cityCache.size()>=64)cityCache.clear();
                cityCache[completed]=choices;
                if(cityInput==completed)showCityChoices(choices,choices.empty()?"No city found.":"");
            } catch(const std::exception& e) {
                saida::Log::info("[World places] search unavailable: ",e.what());
                if(cityInput==completed)showCityChoices({},"Search unavailable. Pick a spot on the globe or in Settings → Destination.");
            }
        }
        if(!menu)return;
        const std::string& query=cityInput;
        if(query.size()<2||query==citySubmitted||cityFuture.valid()||clock-cityChangedAt<.45||
           clock-cityLastRequestAt<1.0)return;
        citySubmitted=query;
        if(const auto cached=cityCache.find(query);cached!=cityCache.end()) {
            showCityChoices(cached->second,cached->second.empty()?"No city found.":"");
            return;
        }
        cityInFlight=query;cityLastRequestAt=clock;
        cityFuture=std::async(std::launch::async,[query]{return r1::searchPlaceChoices(query);});
    }
    // Press-only checks immediate activation; a full gesture also checks that
    // its release/click does not activate the same control a second time.
    void testClick(const std::string& id,bool release=true) {
        if(auto* e=ui->findElementById(id))e->GetContext()->Update();
        auto* element=ui->findElementById(id);
        if(!element)throw std::runtime_error("Missing UI target "+id);
        auto p=element->GetAbsoluteOffset();
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Move,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::None);
        ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Down,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::Left);
        if(release)ui->fireMouseEvent(saida::WebCanvasNode::MouseEvent::Up,int(p.x+10),int(p.y+10),saida::WebCanvasNode::MouseButton::Left);
    }
    void select(double x,double y) {
        pickLon=wrap(x);pickLat=std::clamp(y,-90.,90.);
        field("latitude",pickLat);field("longitude",pickLon);
        if(globe)globe->selected={pickLon,pickLat};
        updateGlobePin();
    }
    void updateGlobePin() {
        if(!globe)return;
        const auto pixel=globe->view.project(pickLon,pickLat,ui->width(),ui->height());
        const bool visible=pixel&&pixel->x>16&&pixel->x<ui->width()-16&&pixel->y>200&&pixel->y<ui->height()-130;
        style("pin","display",visible?"block":"none");
        if(visible){style("pin","left",number(pixel->x,2)+"px");style("pin","top",number(pixel->y,2)+"px");}
    }
    // The menu's music, through the engine's audio: looping, quiet, fading out
    // when the world takes over and back in -- where it left off -- on return.
    static constexpr const char* kMenuMusic="assets/audio/menu/la.mp3";
    static constexpr float kMenuMusicVolume=.25f;
    static constexpr double kMenuMusicFade=1.5; // seconds, either way
    saida::AudioID menuMusic=saida::kInvalidAudioID; float menuMusicGain=0; bool menuMusicSaid=false;
    void updateMenuMusic(double dt) {
        if(smoke)return;
        auto& audio=saida::AudioManager::get();
        const float target=menu?1.f:0.f;
        if(target>0&&menuMusic==saida::kInvalidAudioID) {
            saida::AudioSettings settings;settings.loop=true;settings.volume=0;
            menuMusic=audio.play(kMenuMusic,settings);
            if(menuMusic==saida::kInvalidAudioID) {
                if(!menuMusicSaid)saida::Log::error("[World menu] music ",kMenuMusic," could not play; the menu stays silent");
                menuMusicSaid=true;return;
            }
        }
        if(menuMusic==saida::kInvalidAudioID)return;
        const float step=float(std::min(dt,.1)/kMenuMusicFade);
        menuMusicGain=target>menuMusicGain?std::min(target,menuMusicGain+step):std::max(target,menuMusicGain-step);
        if(menuMusicGain>0&&!audio.isPlaying(menuMusic))audio.resume(menuMusic);
        audio.setVolume(menuMusic,kMenuMusicVolume*menuMusicGain);
        if(menuMusicGain<=0&&audio.isPlaying(menuMusic))audio.pause(menuMusic);
    }
    // Left alone at the whole-Earth view, the globe turns slowly eastward under
    // the Sun. Any click, wheel or typing stops it; it resumes only after 20 s
    // untouched and only at the minimum zoom -- never while a place is sought.
    static constexpr double kGlobeSpinDegreesPerSecond=2.5,kGlobeIdleSeconds=20;
    double globeIdle=0; bool globeSpinning=true;
    void touchGlobe(){globeIdle=0;globeSpinning=false;}
    void spinGlobe(double dt) {
        if(smoke||std::getenv("R1WORLD_MENU_SMOKE")){globeSpinning=false;return;}
        globeIdle+=dt;
        const bool wholeEarth=zoom<=1&&globe->view.zoom<1.01;
        if(!wholeEarth||globeDragging||optionsOpen){if(!wholeEarth)globeSpinning=false;return;}
        if(!globeSpinning&&globeIdle>=kGlobeIdleSeconds)globeSpinning=true;
        if(globeSpinning)globeTarget.lon=wrap(globeTarget.lon+kGlobeSpinDegreesPerSecond*std::min(dt,.1));
    }
    void updateGlobe(double dt) {
        updateMenuMusic(dt);
        if(!globe)return;
        globe->visible=menu;
        if(!menu)return;
        int w=1,h=1;engine.window().size(w,h);
        const auto origin=ui->screenPosition(),size=ui->screenSize();
        globe->viewport={origin.x/w,origin.y/h,size.x/w,size.y/h};
        spinGlobe(dt);
        const double blend=1.-std::exp(-std::min(dt,.1)*10.);
        auto& view=globe->view;
        view.lon=wrap(view.lon+wrap(globeTarget.lon-view.lon)*blend);
        view.lat+=(globeTarget.lat-view.lat)*blend;
        view.zoom=std::exp(std::log(view.zoom)+(std::log(zoom)-std::log(view.zoom))*blend);
        // The Sun moves 0.25 degrees a minute; once a second is smooth.
        if((subsolarAge+=dt)>=1.&&findSun()) {
            json point;
            if(sunScript->callExport("subsolarPoint",json::array(),point)==saida::ScriptCallStatus::Succeeded&&point.is_object()) {
                globe->subsolar={point.value("lon",0.),point.value("lat",0.)};subsolarAge=0;
            } else if(!subsolarRefused) {
                subsolarRefused=true;saida::Log::warn("[World menu] sun_cycle.js gave no subsolar point; the globe keeps its last light");
            }
        }
        updateGlobePin();
        renderMapTiles();
    }
    void showMap(bool show) {
        if(show&&cheatOpen)closeCheat();
        menu=show;style("menu","display",show?"block":"none");style("hud","display",show?"none":"block");
        optionsOpen=false;style("options-screen","display","none");
        if(globe)globe->visible=show;
        minimapUi->setEnabled(!show);
        if(performanceUi)performanceUi->setEnabled(performanceDebug&&!show);
        style("resume","display",playing?"inline-block":"none");
        engine.window().setCursorCaptured(!show);
    }
    void resetPerformanceSample() {
        performanceSeconds=0;performanceFrames=0;
        if(performanceDebug)performanceText("debug-fps","FPS: measuring…");
    }
    void performanceText(const std::string& id,const std::string& value) {
        if(!performanceUi)return;
        auto& previous=written["debug:"+id];
        if(previous!=value&&performanceUi->setElementText(id,value))previous=value;
    }
    void placePerformanceCanvas() {
        if(!performanceUi||!performanceDebug||menu)return;
        const glm::vec2 scale=ui->screenSize()/glm::vec2(ui->width(),ui->height());
        const glm::vec2 at=ui->screenPosition()+glm::vec2((1440.f-360.f-32.f)*scale.x,130.f*scale.y);
        performanceUi->transform().position={at.x,at.y,0};
        performanceUi->transform().scale={scale.x,scale.y,1};
    }
    void ensurePerformanceCanvas() {
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
    void performanceStatus() {
        performanceText("debug-traffic",std::string("V · Road traffic: ")+(debugTraffic?"ON":"OFF"));
        performanceText("debug-grass",std::string("Y · Volumetric grass: ")+(debugGrass?"ON":"OFF"));
        performanceText("debug-water",std::string("M · Water (WaterNode): ")+(debugWater?"ON":"OFF"));
        performanceText("debug-trees",std::string("T · Trees: ")+(debugTrees?"ON":"OFF"));
    }
    void applyPerformanceTile(Loaded& tile) {
        if(tile.grass)tile.grass->setEnabled(debugGrass);
        for(auto* water:tile.waters)water->setEnabled(debugWater);
        for(auto& plant:tile.vegetation)if(plant.tree)plant.node->setEnabled(debugTrees);
        if(tile.trunks)tile.trunks->setEnabled(debugTrees);
        for(size_t i=0;i<tile.cars.size();++i)
            if(tile.cars[i])tile.cars[i]->setEnabled(trafficSlotLive(tile,i));
    }
    void handlePerformanceKeys(const bool (&keys)[5]) {
        bool pressed[5];
        for(size_t i=0;i<5;++i){pressed[i]=keys[i]&&!performanceKeys[i];performanceKeys[i]=keys[i];}
        // Latch even in menus so typing, holding a key, or resuming cannot toggle.
        if(!playing||menu)return;
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
    void samplePerformance(double seconds) {
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
    void pressPerformanceKey(size_t key) {
        bool keys[5]{};handlePerformanceKeys(keys);
        keys[key]=true;handlePerformanceKeys(keys);handlePerformanceKeys(keys);
        keys[key]=false;handlePerformanceKeys(keys);
    }
    void runPerformanceSmoke(double dt) {
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
    int mapZoomLevel() const {return globe?globe->view.tileLevel(ui->height()):3;}
    // One detail layer: the tiles of the level shown, what is on disk to
    // stand in for them, and the downloads in the order they matter -- what is
    // seen (centre first), where the wheel is taking the view, the next level
    // in, the level out -- so a zoom lands on tiles that are already there.
    void fillLayer(MenuTileCache& cache,r1::GlobeLayer& layer,int z,int coarsest,int finest,
                   const r1::GlobeView& target,int targetZ) {
        const auto& view=globe->view;
        const double w=ui->width(),h=ui->height();
        layer.z=z;layer.coarsest=coarsest;
        layer.visible=view.tiles(w,h,z);
        layer.visible.erase(std::remove_if(layer.visible.begin(),layer.visible.end(),[&](const auto& t){return cache.missingForGood(t);}),layer.visible.end());
        const double n=double(1<<z);
        const double cx=(view.lon+180.)/360.*n;
        const double cy=(1.-std::asinh(std::tan(std::clamp(view.lat,-r1::MapView::mercatorLimit,r1::MapView::mercatorLimit)*r1::kPi/180.))/r1::kPi)*.5*n;
        auto distance=[&](const r1::MapTile& t){const double dx=std::remainder(t.x+.5-cx,n),dy=t.y+.5-cy;return dx*dx+dy*dy;};
        std::sort(layer.visible.begin(),layer.visible.end(),[&](const auto& a,const auto& b){return distance(a)<distance(b);});
        std::vector<r1::MapTile> wanted=layer.visible;
        auto add=[&](std::vector<r1::MapTile> more){wanted.insert(wanted.end(),more.begin(),more.end());};
        if(targetZ!=z&&targetZ>=coarsest&&targetZ<=finest)add(target.tiles(w,h,targetZ));
        if(z+1<=finest)add(view.tiles(w,h,z+1,.5));
        if(z-1>=coarsest)add(view.tiles(w,h,z-1));
        layer.cached.clear();
        std::set<std::tuple<int,int,int>> seen;
        auto keep=[&](const r1::MapTile& t){
            if(cache.ready(t)&&seen.insert({t.z,t.x,t.y}).second)layer.cached.push_back({t,cache.path(t)});
        };
        for(const auto& t:wanted)keep(t);
        for(const auto& t:layer.visible)
            for(int up=1;z-up>=coarsest;++up)keep({z-up,t.x>>up,t.y>>up});
        cache.setVisible(std::move(wanted));
    }
    void renderMapTiles() {
        if(!globe)return;
        const auto& view=globe->view;
        const double h=ui->height();
        const int level=view.tileLevel(h);
        r1::GlobeView target=view;target.lon=globeTarget.lon;target.lat=globeTarget.lat;target.zoom=zoom;
        const int targetLevel=target.tileLevel(h);
        size_t missing=0;bool unavailable=false;
        auto count=[&](MenuTileCache& cache,const r1::GlobeLayer& layer){
            for(const auto& t:layer.visible)if(!cache.ready(t))++missing;
            unavailable=unavailable||cache.unavailable();
        };
        // Imagery once the 8K day map runs out of pixels, or as soon as the
        // wheel is heading there.
        if(std::max(level,targetLevel)>=r1::kImageryFirstLevel) {
            fillLayer(imageryTiles,globe->imagery,std::clamp(level,r1::kImageryFirstLevel,r1::kImageryLastLevel),
                      r1::kImageryFirstLevel,r1::kImageryLastLevel,target,std::min(targetLevel,r1::kImageryLastLevel));
            if(level<r1::kImageryFirstLevel)globe->imagery.z=-1; // fetched ahead, not drawn yet
            else count(imageryTiles,globe->imagery);
        } else {globe->imagery={};imageryTiles.setVisible({});}
        const bool osm=view.zoom>=r1::kOsmZoom;
        if(std::max(view.zoom,zoom)>=r1::kOsmZoom/2) {
            fillLayer(menuTiles,globe->map,level,std::max(0,level-6),19,target,targetLevel);
            globe->map.opacity=float(std::clamp(std::log2(view.zoom/r1::kOsmZoom),0.,1.));
            if(!osm)globe->map.z=-1;
            else count(menuTiles,globe->map);
        } else {globe->map={};menuTiles.setVisible({});}
        style("map-detail-status","display",missing?"block":"none");
        if(missing)text("map-detail-status",unavailable?"Detailed map unavailable offline":"Loading the detailed map…");
        const bool imagery=level>=r1::kImageryFirstLevel;
        style("map-attribution","display",osm||imagery?"block":"none");
        if(osm||imagery)text("map-attribution",osm?"© OpenStreetMap contributors":
            "Sentinel-2 cloudless – s2maps.eu by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016)");
    }
    void zoomMap(double z) {
        zoom=std::clamp(z,1.,131072.);
        mapCenterLon=pickLon;mapCenterLat=pickLat;
        globeTarget.lon=pickLon;globeTarget.lat=pickLat;
        renderMapTiles();
        select(pickLon,pickLat);
    }
    double streamPriority(Tile t,double x,double y,double heading) const {
        const r1::P2 middle=t.gen().center();
        const double east=wrap(middle.x-x)*kMetresPerDegree*std::cos(y*rad);
        const double north=(middle.y-y)*kMetresPerDegree;
        const double ahead=east*std::sin(heading*rad)+north*std::cos(heading*rad);
        return std::hypot(east,north)-.8*std::max(0.,ahead);
    }
    bool streamMotion(double& heading,double& speed) const {
        const double velocity=driving?carSpeed:sailing?boat.speed:piloting?plane.speed:0.;
        if(!playing||pending||warming||std::abs(velocity)<kFastDetail)return false;
        heading=wrap((driving?carYaw:sailing?boat.yaw:plane.yaw)+(velocity<0?180.:0.));
        speed=std::abs(velocity);
        return true;
    }
    std::vector<Tile> orderedNearby(double x,double y) const {
        auto tiles=nearby(x,y);
        double heading=0,speed=0;
        if(streamMotion(heading,speed))
            std::stable_sort(tiles.begin()+1,tiles.end(),[&](Tile a,Tile b){
                return streamPriority(a,x,y,heading)<streamPriority(b,x,y,heading);
            });
        return tiles;
    }
    void request(double x,double y) {
        refused.clear();
        auto local=orderedNearby(x,y);
        std::vector<std::vector<Tile>> groups{local};
        std::vector<Tile> forecast;
        std::set<Tile> seen(local.begin(),local.end());
        double heading=0,speed=0;
        if(streamMotion(heading,speed)) {
            const double horizon=std::clamp(speed*kStreamAheadSeconds,600.,1500.);
            for(double fraction:{1./3.,2./3.,1.}) {
                const double distance=horizon*fraction;
                const auto q=advance(x,y,std::sin(heading*rad)*distance,
                                            std::cos(heading*rad)*distance);
                auto group=nearby(q.x,q.y);
                groups.push_back(group);
                for(Tile t:group)if(seen.insert(t).second)forecast.push_back(t);
            }
            std::stable_sort(forecast.begin(),forecast.end(),[&](Tile a,Tile b){
                return streamPriority(a,x,y,heading)<streamPriority(b,x,y,heading);
            });
        }
        std::vector<Tile> priority{local.front()};
        auto append=[&](Tile t){
            if(priority.size()<kStreamRequestLimit&&
               std::find(priority.begin(),priority.end(),t)==priority.end())priority.push_back(t);
        };
        for(size_t i=1;i<std::min(size_t(3),local.size());++i)append(local[i]);
        for(size_t i=0;i<std::min(size_t(3),forecast.size());++i)append(forecast[i]);
        for(size_t i=3;i<local.size();++i)append(local[i]);
        for(size_t i=3;i<forecast.size();++i)append(forecast[i]);
        json tiles=json::array(),sourceGroups=json::array();
        for(Tile t:priority)tiles.push_back({t.r,t.c});
        for(const auto& group:groups) {
            json members=json::array();for(Tile t:group)members.push_back({t.r,t.c});
            sourceGroups.push_back(std::move(members));
        }
        json payload={{"tiles",tiles},{"groups",sourceGroups}};
        std::string signature=payload.dump();if(signature==requested)return;
        std::vector<r1::Tile> genTiles;for(Tile t:priority)genTiles.push_back(t.gen());
        std::vector<std::vector<r1::Tile>> genGroups;
        for(const auto& group:groups){genGroups.emplace_back();for(Tile t:group)genGroups.back().push_back(t.gen());}
        service->want(std::move(genTiles),std::move(genGroups));requested=signature;
    }
    int goCount=0;
    bool go() {
        ++goCount;
        try {
            size_t a,b;std::string x=value("longitude"),y=value("latitude");
            double lo=std::stod(x,&a),la=std::stod(y,&b);
            if(a!=x.size()||b!=y.size()||!std::isfinite(lo)||!std::isfinite(la)||la< -90||la>90||lo< -180||lo>180)throw std::runtime_error("coordinate");
            select(lo,la);pending=true;request(pickLon,pickLat);
            goStarted=std::chrono::steady_clock::now();poll=1;
            arrivalFrames=arrivalHitches=0;arrivalWorst=0;arrivalSaid=false;
            text("status",kPreparing);
            saida::Log::info("[World] destination ",lo,", ",la);
            text("destination-status",std::string());
            return true;
        } catch(...) {
            text("status","Invalid coordinates: latitude −90 to 90, longitude −180 to 180 (decimal point).");
            saida::Log::info("[World] destination refused: '",value("latitude"),"', '",value("longitude"),"'");
            return false;
        }
    }
    // The Sun is a function of where the player stands, and in this game that
    // moves: a fixed anchor would light Sydney at Paris's hour, on the wrong
    // side of the sky, in the wrong season. `scripts/sun_cycle.js` owns the
    // model -- porting it a third time into C++ is exactly what the parity test
    // exists to prevent -- so the game only tells it where the observer is, and
    // the script keeps the beam, the ambient, the horizon and the two exposures
    // consistent with that place. Called wherever the floating origin is
    // rebased, which is every spawn and every 350 m walked.
    bool findSun() {
        if(!sunScript) {
            auto* node=engine.sceneTree().firstInGroup("sun");
            if(!node)return false;
            for(auto& b:node->behaviours())
                if(auto* script=dynamic_cast<saida::ScriptBehaviour*>(b.get()))sunScript=script;
        }
        return sunScript!=nullptr;
    }
    bool tellSun() {
        if(!findSun())return false;
        nlohmann::json result;
        if(!worldCapture.pngPath.empty()) {
            if(sunScript->callExport("setInspectionMode",inspectAt?json::array({true,*inspectAt}):json::array({true}),result)
                    !=saida::ScriptCallStatus::Succeeded)return false;
        }
        if(sunScript->callExport("setObserver",json::array({lon,lat,alt}),result)
               !=saida::ScriptCallStatus::Succeeded)return false;
        if(!result.is_boolean()||!result.get<bool>())return false;
        return applyWeather();
    }
    // The air thins with altitude over the Earth, not over the scene's
    // tangent plane: the engine's fog is given the sphere that fits the
    // ground under the player (centre of curvature, Gaussian radius), so a
    // ray to the horizon climbs out of the haze as it does.
    void tellPlanet() {
        const double p=lat*rad,l=lon*rad,w=1.0-r1::kE2*std::sin(p)*std::sin(p);
        const double radius=std::sqrt(r1::kA/std::sqrt(w)*r1::kA*(1.0-r1::kE2)/(w*std::sqrt(w)));
        const glm::dvec3 normal(std::cos(p)*std::cos(l),std::cos(p)*std::sin(l),std::sin(p));
        auto& settings=engine.sceneTree().world().settings();
        settings.fogPlanetRadius=float(radius);
        settings.fogPlanetCentre=glm::vec3(origin.local(ecef(lon,lat,0.)-normal*radius));
    }
    void moveSun() {
        tellPlanet();
        // A failure here is a scene that keeps lighting the wrong hemisphere
        // rather than a crash, so it has to be said out loud once.
        if(tellSun()||sunReported)return;
        sunReported=true;
        saida::Log::error("[World] the sun cycle did not accept the observer or weather; "
                          "the light stays on the scene's opening instant");
    }
    Loaded* tile(double x,double y) {
        auto i=loaded.find(tileAt(x,y).key());return i==loaded.end()?nullptr:&i->second;
    }
    bool onSeaIce(double x,double y) {
        auto* t=tile(x,y);return t&&t->seaIce;
    }
    // ── headings ────────────────────────────────────────────────────────────
    //
    // A heading is an angle in the origin's frame, the frame the camera and
    // every model are drawn in. A step along it is turned into the local east
    // and north where the step starts. Far from a pole the two are the same
    // to a hundredth of a degree; near one they are not, and a heading kept
    // against the local north walks a circle around the pole and turns back
    // at it. This keeps a straight line straight, over the pole included.
    glm::dvec2 onward(double x,double y,double east,double north) const {
        const glm::dvec3 d=origin.basis[0]*east-origin.basis[2]*north;
        const double l=x*rad,p=y*rad;
        const double e=glm::dot(d,glm::dvec3(-std::sin(l),std::cos(l),0.));
        const double n=glm::dot(d,glm::dvec3(-std::sin(p)*std::cos(l),-std::sin(p)*std::sin(l),std::cos(p)));
        const double length=std::hypot(east,north),flat=std::hypot(e,n);
        if(flat<1e-12)return {x,y};
        return advance(x,y,e/flat*length,n/flat*length);
    }
    // The origin follows the player every 350 m. What the player owns is
    // written in its frame, so every heading turns with it: at the pole the
    // new frame can face the other way from the old one.
    void rebaseOrigin() {
        const Frame next(lon,lat,alt);
        auto rebaseVehicle=[&](saida::Node* node) {
            if(!node)return;
            r1::VehicleRuntime::synchronize(*node);
            const auto worldPoint=origin.origin+origin.basis*glm::dvec3(node->transform().position);
            const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(next.basis)*origin.basis));
            engine.sceneTree().world().rebaseSubtreeTo(*node,glm::vec3(next.local(worldPoint)),
                glm::normalize(rotation*node->transform().rotation));
        };
        rebaseVehicle(car);for(auto& entry:parked)rebaseVehicle(entry.node);
        auto turn=[&](double heading){
            const glm::dvec3 d=origin.basis[0]*std::sin(heading*rad)-origin.basis[2]*std::cos(heading*rad);
            return wrap(std::atan2(glm::dot(d,next.basis[0]),-glm::dot(d,next.basis[2]))/rad);
        };
        const double delta=wrap(turn(yaw)-yaw);
        yaw=turn(yaw);carYaw=turn(carYaw);swimHeading=turn(swimHeading);
        boat.yaw=turn(boat.yaw);plane.yaw=turn(plane.yaw);
        for(auto& entry:parked)entry.yaw=turn(entry.yaw);
        if(std::abs(delta)>1e-9) {
            const auto spin=glm::angleAxis(float(-delta*rad),glm::vec3(0,1,0));
            player->transform().rotation=spin*player->transform().rotation;
        }
        origin=next;placeTiles();moveSun();
    }
    // ── the tile's data, in the form the frame reads it ─────────────────────
    //
    // The tile's manifest is the contract with the generator. What
    // it must not be is the thing a hot loop reads: every `data["bounds"]` is a
    // hash lookup, every `poly[i][0]` is a bounds-checked variant unwrap, and
    // `water[row].get<std::string>()` allocates a string per query. Walking or
    // driving asks these questions tens of thousands of times a second --
    // Height sampling used to repeat JSON accesses across nine tiles for
    // numbers that never change once a tile is mounted.
    //
    // So they are unpacked once, at mount, into plain arrays. Nothing here is a
    // different answer to any question: it is the same data, read the way a
    // frame can afford to read it.
    static void unpack(Loaded& tile) {
        if(auto rooms=tile.data.find("interiors");rooms!=tile.data.end())for(const auto& entry:*rooms) {
            LiveInterior room;room.plan=r1::InteriorPlan::read(entry);
            for(auto p:room.plan.ring){room.low.x=std::min(room.low.x,p.x);room.low.y=std::min(room.low.y,p.y);
                room.high.x=std::max(room.high.x,p.x);room.high.y=std::max(room.high.y,p.y);}
            if(room.plan.footprint<tile.footprints.size())tile.footprints[room.plan.footprint].interior=int(tile.interiors.size());
            tile.interiors.push_back(std::move(room));
        }
        if(auto streaming=tile.data.find("interiorStreaming");streaming!=tile.data.end()&&streaming->contains("unavailable")) {
            std::map<std::string,size_t> reasons;for(const auto& p:streaming->at("unavailable"))++reasons[p.at("reason").get<std::string>()];
            for(const auto& [reason,count]:reasons)saida::Log::info("[World interiors] ",count," buildings unavailable: ",reason);
        }
        if(auto retail=tile.data.find("retail");retail!=tile.data.end())for(const auto& area:retail->at("parking")) {
            StoreParking lot;lot.name=area.at("storeName");
            for(const auto& path:area.at("rings")){r1::Ring r;for(const auto& p:path)r.push_back({p[0],p[1]});lot.rings.push_back(std::move(r));}
            tile.parking.push_back(std::move(lot));
        }
        const auto& bounds=tile.data.at("bounds");
        tile.west=bounds.at("west");tile.east=bounds.at("east");
        tile.south=bounds.at("south");tile.north=bounds.at("north");
        const auto& grid=tile.data.at("elevations");
        tile.gridSize=int(grid.size());
        tile.elevation.resize(size_t(tile.gridSize)*size_t(tile.gridSize));
        tile.groundGrid = r1::ElevationGrid{{tile.south,tile.west,tile.north,tile.east},tile.gridSize,{}};
        for(int row=0;row<tile.gridSize;++row) {
            const auto& line=grid[size_t(row)];
            for(int col=0;col<tile.gridSize;++col) {
                tile.elevation[size_t(row)*size_t(tile.gridSize)+size_t(col)]=float(line[size_t(col)]);
                tile.groundGrid.values.push_back(line[size_t(col)].get<double>());
            }
        }
        if(auto edges=tile.data.find("groundEdges");edges!=tile.data.end()) {
            for(const auto& p:edges->at("south"))tile.groundGrid.southEdge.push_back({p[0],p[1]});
            for(const auto& p:edges->at("north"))tile.groundGrid.northEdge.push_back({p[0],p[1]});
        }
        auto water=tile.data.find("water");
        if(water!=tile.data.end()&&water->is_array()&&!water->empty()) {
            tile.waterRows=int(water->size());
            const std::string first=(*water)[0].get<std::string>();
            tile.waterCols=int(first.size());
            tile.water.assign(size_t(tile.waterRows)*size_t(tile.waterCols),0);
            for(int row=0;row<tile.waterRows;++row) {
                const std::string line=(*water)[size_t(row)].get<std::string>();
                for(int col=0;col<tile.waterCols&&col<int(line.size());++col)
                    tile.water[size_t(row)*size_t(tile.waterCols)+size_t(col)]=uint8_t(std::max(0,line[size_t(col)]-'0'));
            }
        }
        tile.ocean=tile.data.value("surface",std::string())=="ocean";
        tile.seaIce=tile.data.value("surface",std::string())=="sea-ice";
        auto decks=tile.data.find("decks");
        if(decks!=tile.data.end())
            for(const auto& entry:*decks) {
                Deck deck;deck.y=entry.at("y");
                for(const auto& point:entry.at("points")) {
                    glm::dvec2 q{double(point[0]),double(point[1])};
                    deck.points.push_back(q);deck.low=glm::min(deck.low,q);deck.high=glm::max(deck.high,q);
                }
                tile.deckLow=glm::min(tile.deckLow,deck.low);tile.deckHigh=glm::max(tile.deckHigh,deck.high);
                tile.decks.push_back(std::move(deck));
            }
        if(auto dry=tile.data.find("dryStreets");dry!=tile.data.end())
            for(const auto& ring:*dry) {
                Deck piece;
                for(const auto& point:ring) {
                    glm::dvec2 q{double(point[0]),double(point[1])};
                    piece.points.push_back(q);piece.low=glm::min(piece.low,q);piece.high=glm::max(piece.high,q);
                }
                if(piece.points.size()<3)continue;
                tile.dryLow=glm::min(tile.dryLow,piece.low);tile.dryHigh=glm::max(tile.dryHigh,piece.high);
                tile.dryStreets.push_back(std::move(piece));
            }
        auto raised=tile.data.find("raised");
        if(raised!=tile.data.end())
            for(const auto& entry:*raised) {
                RaisedPiece r;
                r.a={double(entry.at("a")[0]),double(entry.at("a")[2])};r.ya=entry.at("a")[1];
                r.b={double(entry.at("b")[0]),double(entry.at("b")[2])};r.yb=entry.at("b")[1];
                for(int s=0;s<2;++s){r.half[s]=entry.at("half")[s];r.rail[s]=entry.at("rail")[s];}
                r.solid=entry.at("solid");
                // An embankment's slopes reach out 2 in 3 below its road.
                const double reach=std::max(r.half[0],r.half[1])+(r.solid?1.5*20.:1.2);
                r.low=glm::min(r.a,r.b)-glm::dvec2(reach);r.high=glm::max(r.a,r.b)+glm::dvec2(reach);
                tile.raisedLow=glm::min(tile.raisedLow,r.low);tile.raisedHigh=glm::max(tile.raisedHigh,r.high);
                tile.raised.push_back(r);
            }
        auto boats=tile.data.find("boats");
        if(boats!=tile.data.end())
            for(const auto& entry:*boats) {
                Mooring m;
                m.name=entry.value("name",std::string());m.kind=entry.value("kind",std::string("motor"));
                m.model=entry.value("model",std::string());
                m.local={entry.value("x",0.),entry.value("y",0.),entry.value("z",0.)};
                m.heading=entry.value("heading",0.);m.length=entry.value("length",8.);
                m.beam=entry.value("beam",3.);m.top=entry.value("top",10.);
                m.accel=entry.value("accel",2.);m.turn=entry.value("turn",30.);
                tile.boats.push_back(std::move(m));
            }
        auto aircraft=tile.data.find("aircraft");
        if(aircraft!=tile.data.end())
            for(const auto& entry:*aircraft) {
                AircraftSpot spot;
                spot.name=entry.value("name",std::string());spot.type=entry.value("type",std::string());
                spot.local={entry.value("x",0.),entry.value("y",0.),entry.value("z",0.)};
                spot.heading=entry.value("heading",0.);
                tile.aircraft.push_back(std::move(spot));
            }
    }
    bool inside(const Loaded& tile,double x,double y,double margin=0.) const {
        return x>=tile.west-margin&&x<=tile.east+margin
             &&y>=tile.south-margin&&y<=tile.north+margin;
    }
    // Is this coordinate on a pier deck? Decks are drawn over the water and
    // walked on, so they answer before the water and before the terrain.
    bool onDeck(const Loaded& t,double x,double y,double* level=nullptr) const {
        if(t.decks.empty())return false;
        const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
        const glm::dvec2 q(p.x,p.z);
        if(q.x<t.deckLow.x||q.x>t.deckHigh.x||q.y<t.deckLow.y||q.y>t.deckHigh.y)return false;
        for(const Deck& d:t.decks) {
            if(q.x<d.low.x||q.x>d.high.x||q.y<d.low.y||q.y>d.high.y)continue;
            bool in=false;const auto& poly=d.points;
            for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++)
                if((poly[i].y>q.y)!=(poly[j].y>q.y)&&q.x<(poly[j].x-poly[i].x)*(q.y-poly[i].y)/(poly[j].y-poly[i].y)+poly[i].x)in=!in;
            if(in){if(level)*level=d.y;return true;}
        }
        return false;
    }
    // Is this coordinate on a street that crosses a water cell? A cell is
    // water or not as a whole; a street through it is dry, always.
    bool onDryStreet(const Loaded& t,double x,double y) const {
        if(t.dryStreets.empty())return false;
        const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
        const glm::dvec2 q(p.x,p.z);
        if(q.x<t.dryLow.x||q.x>t.dryHigh.x||q.y<t.dryLow.y||q.y>t.dryHigh.y)return false;
        bool in=false;
        for(const Deck& d:t.dryStreets) {
            if(q.x<d.low.x||q.x>d.high.x||q.y<d.low.y||q.y>d.high.y)continue;
            const auto& poly=d.points;
            for(size_t i=0,j=poly.size()-1;i<poly.size();j=i++)
                if((poly[i].y>q.y)!=(poly[j].y>q.y)&&q.x<(poly[j].x-poly[i].x)*(q.y-poly[i].y)/(poly[j].y-poly[i].y)+poly[i].x)in=!in;
        }
        return in;
    }
    // The surfaces off the ground at (x, y): bridge decks and embankments
    // (gen/bridges.hpp). Each is handed to `seen(level, solid, parapet)`:
    // `solid` says whoever is below it meets a wall, not a ceiling; a
    // parapet is the edge of a deck, a wall for whoever stands on it.
    template<class F> void raisedAt(const Loaded& t,double x,double y,F&& seen) const {
        if(t.raised.empty())return;
        const glm::dvec3 p=t.frame.local(ecef(x,y,0.));
        const glm::dvec2 q(p.x,p.z);
        if(q.x<t.raisedLow.x||q.x>t.raisedHigh.x||q.y<t.raisedLow.y||q.y>t.raisedHigh.y)return;
        for(const RaisedPiece& r:t.raised) {
            if(q.x<r.low.x||q.x>r.high.x||q.y<r.low.y||q.y>r.high.y)continue;
            const glm::dvec2 d=r.b-r.a;const double l2=glm::dot(d,d);
            if(l2<=0)continue;
            // Each piece is a capsule: round past its ends, so the outside of
            // a bend is covered where two pieces meet.
            const double f=std::clamp(glm::dot(q-r.a,d)/l2,0.,1.);
            const glm::dvec2 across=q-(r.a+f*d);
            const int side=glm::dot(across,glm::dvec2(-d.y,d.x))>=0?0:1;
            const double top=r.ya+(r.yb-r.ya)*f,off=glm::length(across),half=r.half[side];
            if(off<=half)seen(top,r.solid,false);
            else if(r.solid)seen(top-(off-half)/1.5,true,false);
            // A band wider than a fast car's step in one frame.
            else if(r.rail[side]&&off<=half+1.2)seen(top,false,true);
        }
    }
    // What a walker or a car at altitude `standing` stands on at (x, y): the
    // highest surface within a step of him, so a bridge carries whoever is on
    // it and passes over whoever is under it. With no `standing`, the highest.
    double height(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        auto* t=tile(x,y);if(!t)throw std::runtime_error("Missing terrain");
        const double ground=terrainHeight(*t,x,y);
        const bool any=std::isnan(standing);
        double best=ground;
        const auto worldPoint=ecef(x,y,ground);
        for(const auto& [key,other]:loaded) {
          const auto local=other.frame.local(worldPoint);const r1::P2 q{local.x,local.z};
          for(const auto& room:other.interiors) {
            const auto& p=room.plan;
            const double reach=std::max(4.5,p.approachRun()+.5);
            if(q.x<room.low.x-reach||q.x>room.high.x+reach||q.y<room.low.y-reach||q.y>room.high.y+reach)continue;
            const auto door=p.local(q);
            double level=0;
            if(room.contains(q))level=p.floor;
            else if(std::abs(door.x)<p.width/2+.4&&door.y>=-p.approachRun()&&door.y<=0)
                level=p.floor+(p.approach-p.floor)*(-door.y/p.approachRun());
            else continue;
            // Store floors replace the terrain inside, even on a slope.
            return level-other.frame.local(ecef(x,y,0.)).y;
          }
        }
        // A piece belongs to the tile its middle is in: near a tile's edge,
        // the deck under a foot may be its neighbour's.
        for(const auto& [key,other]:loaded)
            raisedAt(other,x,y,[&](double level,bool,bool parapet){
                if(parapet||level<=best)return;
                if(any||level<=standing+kStepUp)best=level;
            });
        return best;
    }
    double terrainHeight(const Loaded& tile,double x,double y) const {
        double deck=0;if(onDeck(tile,x,y,&deck))return deck;
        return gridHeight(tile,x,y);
    }
    // The tile's own ground grid, decks aside; clamped to the tile.
    static double gridHeight(const Loaded& tile,double x,double y) {
        const Loaded* t=&tile;
        const int n=t->gridSize;
        double u=std::clamp((wrap(x)-t->west)/(t->east-t->west),0.,1.)*(n-1);
        double v=std::clamp((y-t->south)/(t->north-t->south),0.,1.)*(n-1);
        if((v<1&&!t->groundGrid.southEdge.empty())||(v>n-2&&!t->groundGrid.northEdge.empty()))
            return r1::terrainElevation(wrap(x),y,t->groundGrid);
        int ix=std::min(n-2,int(u)),iy=std::min(n-2,int(v));u-=ix;v-=iy;
        const float* g=t->elevation.data();
        const size_t low=size_t(iy)*size_t(n),high=low+size_t(n);
        // The two triangles the ground is drawn and collided with (gen/terrain
        // buildTerrain: south-west to north-east diagonal), never a bilinear
        // patch: on an embankment the two disagree by decimetres, and a car
        // riding the patch met the drawn ground as an obstacle nobody could see.
        const double sw=g[low+size_t(ix)],se=g[low+size_t(ix)+1],nw=g[high+size_t(ix)],ne=g[high+size_t(ix)+1];
        return u>=v?sw*(1-u)+se*(u-v)+ne*v:sw*(1-v)+ne*u+nw*(v-u);
    }
    // The tile's water grid and fully oceanic tiles must answer the same
    // question for walkers, cars and boats. Piers remain dry walkable decks.
    // `standing`: whoever is at that altitude on a bridge over the water is dry.
    bool onWater(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        const auto* t=tile(x,y);
        if(!t||onDeck(*t,x,y)||!(t->ocean||waterCode(*t,x,y)!=0)||onDryStreet(*t,x,y))return false;
        // A bridge's parapet stands between whoever is on it and the river.
        if(!std::isnan(standing)&&(height(x,y,standing)>terrainHeight(*t,x,y)+.5))return false;
        return true;
    }
    int waterCode(const Loaded& t,double x,double y) const {
        if(t.water.empty())return 0;
        const int row=std::clamp(int((y-t.south)/(t.north-t.south)*t.waterRows),0,t.waterRows-1);
        const int col=std::clamp(int((x-t.west)/(t.east-t.west)*t.waterCols),0,t.waterCols-1);
        return t.water[size_t(row)*size_t(t.waterCols)+size_t(col)];
    }
    // Where a boat may be: an ocean tile, or a water cell of a land tile that
    // is not under a pier. The same cells the terrain is drawn from, so a boat
    // stops at the shore the eye sees.
    bool navigable(double x,double y) {
        return onWater(x,y);
    }
    // The surface a boat floats on: the sea's 0 m, or a lake's own level.
    double waterLevel(double x,double y) {
        auto* t=tile(x,y);
        if(!t||t->ocean)return 0.;
        return waterCode(*t,x,y)==1?height(x,y):0.;
    }
    // The ground under the water, where something says what it is. The relief
    // sources stop at the shore: open sea is 0.0 m, exactly, at every grid
    // point (Mapzen), and inland water is drawn on its own ground. Only a
    // shore profile the beach generator sculpted, a rock or a shoal gives the
    // sea a bed. A grid quad whose four corners all read 0.0 has none.
    std::optional<double> waterBed(double x,double y) {
        const auto* t=tile(x,y);
        if(!t||t->ocean||waterCode(*t,x,y)==1)return std::nullopt;
        const int n=t->gridSize;
        const double u=std::clamp((wrap(x)-t->west)/(t->east-t->west),0.,1.)*(n-1);
        const double v=std::clamp((y-t->south)/(t->north-t->south),0.,1.)*(n-1);
        const int ix=std::min(n-2,int(u)),iy=std::min(n-2,int(v));
        const float* g=t->elevation.data()+size_t(iy)*size_t(n)+size_t(ix);
        if(g[0]==0.f&&g[1]==0.f&&g[n]==0.f&&g[n+1]==0.f)return std::nullopt;
        return gridHeight(*t,x,y);
    }
    // He swims when half of him is under the surface, and walks otherwise:
    // a rock, a sandbar or the swash is ground. Water whose bed nobody
    // knows is deep, as it always was. `standing` is where his feet rest,
    // as the character solver found it: a breakwater, a slipway or a rock
    // the physics carries above the ground grid is his bed there, though
    // neither the grid nor `height` knows it.
    bool deepWater(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        if(!onWater(x,y,standing))return false;
        auto bed=waterBed(x,y);
        if(const auto* t=tile(x,y);t&&std::isfinite(standing)&&standing>gridHeight(*t,x,y)+.1)
            bed=std::max(bed.value_or(standing),standing);
        return !bed||waterLevel(x,y)-*bed>=playerHeight*kSwimDepth;
    }
    // Collider authoring only: detection and response belong to Saida/Jolt.
    static saida::StaticBodyNode* boxCollider(saida::Node& parent,const std::string& name,
                                            glm::vec3 size,glm::vec3 center) {
        auto body=std::make_unique<saida::StaticBodyNode>();body->setName(name);
        auto shape=std::make_unique<saida::CollisionShapeNode>();
        shape->shapeType=saida::CollisionShapeType::Box;shape->halfExtents=size*.5f;shape->offset=center;
        body->addChild(std::move(shape));return static_cast<saida::StaticBodyNode*>(parent.addChild(std::move(body)));
    }
    static void meshCollider(saida::Node& parent,std::unique_ptr<saida::MeshNode> mesh) {
        auto body=std::make_unique<saida::StaticBodyNode>();body->setName(mesh->name()+" collider");
        auto shape=std::make_unique<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Mesh;
        body->addChild(std::move(shape));body->addChild(std::move(mesh));parent.addChild(std::move(body));
    }
    void vehicleCollider(saida::Node& node,const VehicleModel& spec) {
        if(!r1::VehicleRuntime::body(node))if(auto* lod=node.getBehaviour<saida::LODGroupBehaviour>())node.removeBehaviour(lod);
        r1::VehicleRuntime::install(node,{float(spec.length),float(spec.width),float(spec.height),float(spec.wheelbase),float(spec.wheelRadius)},spec.name);
        vehicleLod(node);
    }
    saida::QueryFilter obstacleFilter() const {
        saida::QueryFilter filter;filter.ignoreInner=feet->innerBodyId();
        if(ridingBike())if(auto* body=r1::VehicleRuntime::body(*car))filter.ignore=body->bodyId();
        if(car)if(auto* body=dynamic_cast<saida::CollisionObjectNode*>(car->findByPath("Vehicle collider")))filter.ignore=body->bodyId();
        return filter;
    }
    bool solidAt(double x,double y,double level,float radius=.32f) const {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
        const glm::vec3 at(origin.local(ecef(x,y,level)));
        return !physics->overlapSphere(at,radius,obstacleFilter()).empty();
    }
    // Spawn/exit/parking occupancy uses the engine's scene queries. Movement
    // itself uses the character solver, without a second footprint test.
    bool blocked(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        return onWater(x,y,standing)||obstructed(x,y,standing);
    }
    // On foot, only water out of his depth stops him: he wades in the rest.
    bool footBlocked(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        return deepWater(x,y,standing)||obstructed(x,y,standing);
    }
    // A wall, a body or a roof over him, water aside.
    bool obstructed(double x,double y,double standing=std::numeric_limits<double>::quiet_NaN()) {
        const double ground=std::isnan(standing)?height(x,y):standing;
        if(solidAt(x,y,ground+.6)||solidAt(x,y,ground+1.3))return true;
        // A mesh encloses air: an arrival below an opaque roof is not an outdoor
        // spawn, even when its capsule would initially touch no triangles.
        if(std::isnan(standing)||parkingCar) {
            auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
            const glm::vec3 above(origin.local(ecef(x,y,ground+500.)));
            const auto hit=physics->raycast(above,{0,-1,0},500.f,obstacleFilter());
            if(hit.hit&&hit.point.y>origin.local(ecef(x,y,ground)).y+2.)return true;
        }
        return false;
    }
    // ── the car ─────────────────────────────────────────────────────────────
    //
    // Everything below shares the walk's vocabulary on purpose: the same
    // `advance` on the ellipsoid, the same `blocked` against streamed
    // engine bodies and water, the same `height` off the tile's own grid. A car
    // that used a second notion of where the ground is would disagree with the
    // player about it the first time he stepped out.

    // Terrain height, or `fallback` where no tile answers yet. `height` throws
    // by design -- walking off the edge of the loaded world is a refusal the
    // walk reports -- and the car asks four times a frame to sit on a slope, so
    // it asks the question that has an answer.
    double groundAt(double x,double y,double fallback) {
        return tile(x,y)?height(x,y,fallback):fallback;
    }
    // Yaw, then the slope the wheels are actually standing on. Measured off the
    // terrain grid over the car's own wheelbase and track rather than inferred
    // from a normal: it is the same surface the collision reads (§3 I5).
    glm::quat carRotation() {
        const auto& spec=vehicleSpec(*car);
        const double halfLength=spec.wheelbase*.5,halfWidth=spec.width*.43;
        auto sample=[&](double east,double north){
            auto q=onward(carLon,carLat,east,north);return groundAt(q.x,q.y,carAlt);
        };
        double s=sin(carYaw*rad),c=cos(carYaw*rad);
        double ahead=sample(s*halfLength,c*halfLength),behind=sample(-s*halfLength,-c*halfLength);
        double right=sample(c*halfWidth,-s*halfWidth),left=sample(-c*halfWidth,s*halfWidth);
        auto yawQ=glm::angleAxis(float(-carYaw*rad),glm::vec3(0,1,0));
        auto pitchQ=glm::angleAxis(float(std::atan2(ahead-behind,2*halfLength)),glm::vec3(1,0,0));
        auto rollQ=glm::angleAxis(float(std::atan2(right-left,2*halfWidth)),glm::vec3(0,0,1));
        return yawQ*pitchQ*rollQ;
    }
    // The four wheels of whatever car the player is now in. Re-run on every
    // take-over, because the wheels that turn must belong to the car he is
    // actually sitting in.
    //
    // Outermost match only, and it is not a detail: the importer wraps each
    // named node around a mesh node that inherits the name, so descending into
    // a wheel finds the same wheel again and every turn would be applied to it
    // twice. Eight wheels on a saloon is what said so.
    void collectWheels(saida::Node& root) {
        frontWheels.clear();rearWheels.clear();
        std::function<void(saida::Node&)> collect=[&](saida::Node& n){
            if(n.name().rfind("wheel-",0)==0) {
                (n.name().find("front")!=std::string::npos?frontWheels:rearWheels).push_back(&n);
                return;
            }
            for(auto& child:n.children())collect(*child);
        };
        collect(root);
        const size_t levels=(root.findByPath("Far")||root.findByPath("Vehicle collider/Far"))?2:1;
        // The player's scene car is collected before buildTrafficPrototype()
        // populates `fleet`; do not ask vehicleSpec() for its fallback then.
        // Traffic instances have their manifest name on the root node.
        const bool bike=root.name()=="vehicle-moto_city"||root.name()=="vehicle-moto_cross";
        const size_t expected=levels*(bike?1:2);
        if(frontWheels.size()!=expected||rearWheels.size()!=expected)
            saida::Log::warn("[World car] this car has ",frontWheels.size()," front and ",
                             rearWheels.size()," rear wheels named wheel-*; they will not turn");
    }
    void placeCar() {
        // Abandoned cars are written in the origin's frame like everything else
        // the player owns, so they have to be rewritten when the origin moves.
        // There are at most six of them.
        for(auto& entry:parked) {
            if(r1::VehicleRuntime::physical(*entry.node)) {
                r1::VehicleRuntime::synchronize(*entry.node);
                const auto p=entry.node->transform().position;
                const auto worldPoint=origin.origin+origin.basis*glm::dvec3(p);
                const auto geo=r1::ecefToGeodetic(worldPoint.x,worldPoint.y,worldPoint.z);
                entry.lon=geo.x;entry.lat=geo.y;entry.alt=geo.z-r1::VehicleRuntime::kBodyClearance;
                continue;
            }
            entry.node->transform().position=glm::vec3(origin.local(ecef(entry.lon,entry.lat,entry.alt+.06)));
            entry.node->transform().rotation=glm::angleAxis(float(-entry.yaw*rad),glm::vec3(0,1,0));
        }
        if(!carParked||!car)return;
        if(r1::VehicleRuntime::physical(*car))captureCarPose();
        else {
            car->transform().position=glm::vec3(origin.local(ecef(carLon,carLat,carAlt+.06)));
            car->transform().rotation=carSinking?glm::angleAxis(float(-carYaw*rad),glm::vec3(0,1,0)):carRotation();
        }
        // Wheels turn because a car whose wheels do not turn is a car sliding
        // on ice, and this one is visible from three metres behind it.
        const bool bike=r1::motorcycle(vehicleSpec(*car).name);
        auto spin=glm::angleAxis(float(bike?wheelSpin:-wheelSpin),glm::vec3(1,0,0));
        const auto* drive=r1::VehicleRuntime::driver(*car);
        const double steering=drive?drive->steeringAngle*steerShown/(1.+drive->speedSteering*carSpeed*carSpeed):kCarSteerAngle*steerShown;
        auto lock=glm::angleAxis(float(-steering*rad),glm::vec3(0,1,0));
        for(auto* w:rearWheels)w->transform().rotation=spin;
        for(auto* w:frontWheels)w->transform().rotation=lock*spin;
    }
    // Somewhere within `radius` metres of (x0,y0) that is on loaded terrain,
    // out of the water and out of a building. The golden-angle spiral is the
    // spawn search's, for the same reason it is used there: it covers a disc
    // evenly without preferring a compass direction.
    // `standing`: the level the spot must be on -- beside a car on a bridge
    // is on the bridge, not on the ground six metres below its parapet.
    bool freeSpot(double x0,double y0,double radius,double& outLon,double& outLat,
                  double standing=std::numeric_limits<double>::quiet_NaN()) {
        auto free=[&](double x,double y){
            if(!tile(x,y)||blocked(x,y,standing))return false;
            return std::isnan(standing)||std::abs(height(x,y,standing)-standing)<kStepUp;
        };
        if(free(x0,y0)){outLon=x0;outLat=y0;return true;}
        for(int i=1;i<=120;++i) {
            double a=i*2.39996323,d=radius*std::sqrt(double(i)/120.);
            auto q=advance(x0,y0,d*cos(a),d*sin(a));
            if(free(q.x,q.y)){outLon=q.x;outLat=q.y;return true;}
        }
        return false;
    }
    // How far one can go from (x, y) at `level` along a heading before a
    // wall, a parapet, water or the edge of the loaded world: the level is
    // followed step by step, up a ramp and onto a deck.
    double clearAhead(double x,double y,double level,double heading,double step,double limit) {
        double clear=0;
        for(double d=step;d<=limit;d+=step) {
            auto q=onward(x,y,sin(heading*rad)*d,cos(heading*rad)*d);
            if(!tile(q.x,q.y)||blocked(q.x,q.y,level))break;
            level=height(q.x,q.y,level);
            clear=d;
        }
        return clear;
    }
    // A building owned by a neighbouring tile can stream in over the spot the
    // car was parked on before that tile mounted: move the car out, and say so.
    void reparkIfCovered(const std::string& key) {
        if(!carParked||driving||!car)return;
        parkingCar=true;
        const bool covered=blocked(carLon,carLat,carAlt);
        double x=carLon,y=carLat;
        const bool found=covered&&freeSpot(carLon,carLat,20.,x,y,carAlt);
        parkingCar=false;
        if(!covered)return;
        if(!found){saida::Log::warn("[World car] ",key," covered the parked car and no free spot is within 20 m");return;}
        saida::Log::info("[World car] ",key," streamed in over the parked car; moved to ",x,", ",y);
        r1::VehicleRuntime::deactivate(*car);
        carLon=x;carLat=y;carAlt=groundAt(x,y,carAlt);placeCar();
    }
    // Park the car beside a dry spawn. A swimmer cannot arrive with a car.
    void parkCar() {
        carParked=false;carSinking=false;carSinkDepth=0;
        carSpeed=0;wheelSpin=0;steerShown=0;
        // Half a planet away from where they were left, so they are not kept.
        clearAbandonedCars();
        if(!car)return;
        r1::VehicleRuntime::reset(*car);
        if(auto* drive=r1::VehicleRuntime::driver(*car))drive->parked=true;
        car->setEnabled(false);
        if(swimming)return;
        // No road has ever reached the pack ice: nobody drives to the Pole.
        if(onSeaIce(lon,lat)) {
            saida::Log::info("[World car] on the sea ice: the player arrives on foot, with no car");
            return;
        }
        // Three metres to his right, which is where a car that dropped him off
        // would be, and a spiral out from there when that spot is a wall.
        auto beside=onward(lon,lat,cos(yaw*rad)*3.,-sin(yaw*rad)*3.);
        double x,y;
        // A car's spot is judged as a car: a shop a walker may enter is a
        // wall to it, or the car would be parked inside the store.
        parkingCar=true;
        const bool found=freeSpot(beside.x,beside.y,14.,x,y,alt);
        parkingCar=false;
        if(!found) {
            // Loud, and said to the player too. A car that silently is not
            // there looks exactly like a car that failed to load, which is the
            // shape of failure this file has already been bitten by three
            // times (see `refused`, and the spawn refusal below).
            saida::Log::warn("[World car] no free kerb within 14 m of ",lon,", ",lat,
                             "; the player spawns on foot with no car");
            text("stream-status","No room for the car here — starting on foot.");
            return;
        }
        carLon=x;carLat=y;carAlt=groundAt(x,y,alt);carYaw=yaw;carParked=true;
        car->setEnabled(true);placeCar();
        saida::Log::info("[World car] parked at ",carLon,", ",carLat);
    }
    // ── taking a car ────────────────────────────────────────────────────────
    //
    // Any car, not one car. The one the spawn parks is simply the first, and
    // from there the player takes whichever is nearest — his own, one he left
    // in a side street an hour ago, or one that was driving past a second ago.
    //
    // The car he takes is the car he drives: the traffic node is handed over
    // rather than copied, and the node he was using stays exactly where he
    // stepped out of it. Copying would be less code and it would teleport the
    // car he just parked to wherever he happens to be standing, which is the
    // one thing a player notices immediately.
    void abandonCurrentCar() {
        if(!car||!carParked)return;
        keepParked({car,carLon,carLat,carAlt,carYaw});
        car=nullptr;carParked=false;carSinking=false;carSinkDepth=0;
    }
    void keepParked(const Parked& entry) {
        parked.push_back(entry);
        while(parked.size()>kAbandonedCars) {
            // The scene's own car is in this list like any other once it has
            // been left; nothing here is special-cased, so nothing here can
            // disagree with itself about which car is the player's.
            parked.front().node->queueFree();
            parked.erase(parked.begin());
        }
    }
    void clearAbandonedCars() {
        for(auto& entry:parked)entry.node->queueFree();
        parked.clear();
    }
    // Where a point in this tile's frame is on the planet, to within the metre
    // or two that separates it from the player. Going through `advance` rather
    // than inverting the frame keeps this file to one direction of geodesy:
    // the offset is read in the tile's tangent plane, where east is +x and
    // north is -z, and walked from the player's own coordinate.
    glm::dvec2 geodeticOf(const Loaded& tile,const glm::vec3& local) const {
        const glm::dvec3 here=tile.frame.local(ecef(lon,lat,alt));
        return advance(lon,lat,double(local.x)-here.x,-(double(local.z)-here.z));
    }
    // The nearest car the player can reach: his own, one he abandoned, or one
    // in traffic. Returns false when there is none.
    struct Reach {saida::Node* node=nullptr; Loaded* tile=nullptr; size_t agent=0; int kind=0;};
    bool nearestCar(Reach& out,double& distance) {
        distance=kCarReach;
        bool found=false;
        const glm::dvec3 me=ecef(lon,lat,alt);
        if(carParked&&car&&!carSinking) {
            const double d=glm::length(me-ecef(carLon,carLat,carAlt));
            if(d<distance){distance=d;out={car,nullptr,0,0};found=true;}
        }
        for(size_t i=0;i<parked.size();++i) {
            const auto& entry=parked[i];
            const double d=glm::length(me-ecef(entry.lon,entry.lat,entry.alt));
            if(d<distance){distance=d;out={entry.node,nullptr,i,1};found=true;}
        }
        for(auto& [key,tile]:loaded) {
            for(size_t i=0;i<tile.cars.size();++i) {
                if(!tile.cars[i]||!trafficSlotLive(tile,i))continue;
                const auto here=geodeticOf(tile,tile.cars[i]->transform().position);
                const double d=glm::length(me-ecef(here.x,here.y,alt));
                if(d<distance){distance=d;out={tile.cars[i],&tile,i,2};found=true;}
            }
        }
        return found;
    }
    // Hand a traffic car over: it leaves the flow, leaves its tile, and becomes
    // the car the player is in. The flow must forget it first -- a car being
    // driven by the player and by the simulation at once is two cars.
    bool takeTrafficCar(const Reach& target) {
        Loaded& tile=*target.tile;
        r1::VehicleRuntime::synchronize(*target.node);
        target.node->updateTransforms(tile.node->worldTransform(),true);
        const glm::mat4 previousWorld=target.node->worldTransform();
        const glm::vec3 local=target.node->transform().position;
        const auto here=geodeticOf(tile,local);
        auto owned=tile.node->detachChild(target.node);
        if(!owned) {
            saida::Log::warn("[World car] the traffic car could not be detached from ",
                             tile.data.value("key",std::string()));
            return false;
        }
        tile.flow.retire(target.agent);
        tile.cars[target.agent]=nullptr;
        abandonCurrentCar();
        car=engine.sceneTree().world().addChild(std::move(owned));
        removeTrafficRider(*car);
        car->transform().position=glm::vec3(previousWorld[3]);
        car->transform().rotation=glm::quat_cast(glm::mat3(previousWorld));
        carParked=true;
        carLon=here.x;carLat=here.y;carAlt=groundAt(carLon,carLat,alt);
        // Keep its heading: the car was driving somewhere, and snapping it to
        // the player's facing is a jolt the eye catches at two metres.
        const glm::vec3 forward=car->transform().rotation*glm::vec3(0,0,-1);
        carYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
        carSpeed=0;wheelSpin=0;steerShown=0;
        collectWheels(*car);
        saida::Log::info("[World car] took a traffic car at ",carLon,", ",carLat);
        return true;
    }
    void takeAbandonedCar(const Reach& target) {
        const Parked entry=parked[target.agent];
        parked.erase(parked.begin()+long(target.agent));
        abandonCurrentCar();
        car=entry.node;carParked=true;
        carLon=entry.lon;carLat=entry.lat;carAlt=entry.alt;carYaw=entry.yaw;
        carSpeed=0;wheelSpin=0;steerShown=0;
        collectWheels(*car);
    }

    // Why the car last stopped against something, for whoever reads the log.
    std::string carStop="nothing";
    std::string carStopSaid; // the obstacle the journal last named, cleared once the car moves on
    bool parkingCar=false;
    double carDistance() const {
        return glm::length(ecef(lon,lat,alt)-ecef(carLon,carLat,carAlt));
    }
    void removeTrafficRider(saida::Node& vehicle) {
        auto* body=r1::VehicleRuntime::body(vehicle);
        auto* owner=body?static_cast<saida::Node*>(body):&vehicle;
        if(auto* rider=owner->findByPath("Rider"))owner->removeChild(rider);
    }
    bool ridingBike() const {return driving&&car&&r1::motorcycle(vehicleSpec(*car).name);}
    glm::vec3 motorcycleContact(saida::Node& rider,const std::string& vehicle,int hand,bool grip) {
        const bool cross=vehicle=="moto_cross";
        // Contact points measured on the pinned models, before the shared resize.
        const float halfBar=cross?.361f:.285f;
        const glm::vec3 point=grip?glm::vec3(hand?halfBar:-halfBar,cross?1.20f:1.116f,cross?-.344f:-.390f)
            :glm::vec3(hand?.24f:-.24f,cross?.565f:.44f,cross?.338f:.14f);
        return glm::vec3(glm::inverse(rider.localMatrix())*glm::vec4(point*float(kVehicleScale),1.f));
    }
    void bikeHandlePose() {
        if(!playerBikeRider)return;
        const auto& kind=vehicleSpec(*car).name;
        playerBikeLean->lookAt({0.f,-.4f,1.2f});
        playerBikeHands->hold(true);
        if(!aiming)playerBikeLook->lookAt({0.f,1.5f,100.f});
        for(int h=0;h<2;++h) {
            const auto foot=motorcycleContact(*playerBody,kind,h,false);
            playerBikeLegs[h]->reach(foot);playerBikeLegs[h]->setWeightNow(1.f);
            playerBikeLegs[h]->setTipRotation(r1::rocketboxFootRotation(*animators.front()->rig(),h==1));
            playerBikeLegs[h]->setPole({foot.x*2.f,foot.y+.35f,foot.z+.45f});
            if(h==1&&aiming&&arsenal.held()==r1::combat::Weapon::Pistol)continue;
            auto* arm=playerArms[h];const auto grip=motorcycleContact(*playerBody,kind,h,true);
            const auto palm=r1::rocketboxBarGrip(*animators.front()->rig(),h==1);
            const auto wrist=grip-palm.rotation*palm.position;
            if(h==0)playerBikeAim->keepLeftHandAt(wrist);
            arm->setTipRotation(palm.rotation);arm->reach(wrist);arm->setWeightNow(1.f);
            arm->setPole({grip.x*1.7f,grip.y-.15f,grip.z-.25f});
        }
    }
    void mountPlayerOnBike() {
        if(!car||!playerBody||animators.empty()||!r1::motorcycle(vehicleSpec(*car).name))return;
        HumanKind* seated=humanKind(0);
        auto* body=r1::VehicleRuntime::body(*car);
        if(!seated||!seated->rig||!body)return;
        removeTrafficRider(*car);
        auto owned=playerBody->parent()->detachChild(playerBody);
        const bool motocross=vehicleSpec(*car).name=="moto_cross";
        playerBikeRider=body->addChild(std::move(owned));
        playerBody->transform().position=r1::rocketboxRidingSeat(*animators.front()->rig(),motocross,
            float(kVehicleScale),float(humanScale),float(seated->seatDrop));
        playerBody->transform().rotation=glm::quat(0,0,1,0); // Rig +Z faces vehicle -Z.
        playerBody->transform().scale=glm::vec3(float(humanScale));
        auto* animator=animators.front();
        for(const auto& [name,clip]:seated->clips)if(name=="sit")animator->addClip("bike-sit",clip);
        animator->play("bike-sit",true,0.f);animator->setPoseRate(0.f);
        r1::holdRocketboxRidingBase(*animator);
        showHeldPistol();
        bikeHandlePose();
        saida::Log::info("[World motorcycle] player body attached to solver on ",vehicleSpec(*car).name);
    }
    void dismountPlayerFromBike() {
        if(!playerBikeRider)return;
        if(aiming)setAiming(false);
        auto owned=playerBikeRider->parent()->detachChild(playerBikeRider);
        player->addChild(std::move(owned));
        playerBody->transform()=playerBodyRest;
        playerBikeRider=nullptr;
        showHeldPistol();
        for(auto* arm:playerArms)if(arm){arm->release();arm->clearTipRotation();}
        for(auto* leg:playerBikeLegs)if(leg)leg->release();
        if(playerBikeLean)playerBikeLean->release();
        if(playerBikeLook)playerBikeLook->release();
        if(playerBikeAim)playerBikeAim->release();
        if(playerBikeHands)playerBikeHands->hold(false);
        for(auto* animator:animators)animator->play("idle",true,0.f);
    }
    bool enterCar() {
        if(driving||swimming)return false;
        Reach target;double distance=0;
        if(!nearestCar(target,distance))return false;
        if(target.kind==2){if(!takeTrafficCar(target))return false;}
        else if(target.kind==1)takeAbandonedCar(target);
        if(!carParked||!car)return false;
        if(aiming)setAiming(false);
        driving=true;lookYaw=0;lookIdle=0;
        if(auto* physics=engine.sceneTree().world().physics())r1::VehicleRuntime::activate(*car,*physics,glm::vec3(0));
        captureCarPose();
        if(auto* drive=r1::VehicleRuntime::driver(*car))drive->parked=false;
        // The car is where the player is now, and the walk's own lon/lat drive
        // it from here: one position, one streamer, one sun. Nothing downstream
        // has to learn that the player is sitting down.
        lon=carLon;lat=carLat;alt=carAlt;yaw=carYaw;
        player->setEnabled(false);
        for(auto* a:animators)a->play("idle");
        mountPlayerOnBike();
        jumpOffset=jumpVelocity=0;
        text("stream-status",ridingBike()?"Riding. F: get off · Space: rear brake · 2: pistol · Right click: aim.":"Driving. F: get out · Space: handbrake.");
        saida::Log::info("[World car] entered at ",carLon,", ",carLat);
        return true;
    }
    // Exit at any speed or attitude. A blocked door falls back to the clear
    // space above the chassis; an airborne exit falls from its actual height.
    bool leaveCar() {
        if(!driving)return false;
        captureCarPose();
        const auto& spec=vehicleSpec(*car);
        const r1::VehicleDimensions size{float(spec.length),float(spec.width),float(spec.height),float(spec.wheelbase),float(spec.wheelRadius)};
        glm::vec3 exitPosition;bool found=false;
        for(float turn:{0.f,180.f,90.f,270.f}) {
            const double bearing=(carYaw+turn)*rad;
            const auto offset=r1::VehicleRuntime::exitOffset(size,car->transform().rotation,
                {float(cos(bearing)),0,float(-sin(bearing))});
            const auto candidate=car->transform().position+offset;
            const auto point=origin.origin+origin.basis*glm::dvec3(candidate);
            const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
            if(tile(geo.x,geo.y)&&!blocked(geo.x,geo.y,geo.z)){exitPosition=candidate;found=true;break;}
        }
        if(!found)exitPosition=car->transform().position+
            r1::VehicleRuntime::exitOffset(size,car->transform().rotation,{0,1,0});
        const auto point=origin.origin+origin.basis*glm::dvec3(exitPosition);
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        const double exitSpeed=carSpeed;
        float verticalVelocity=0;
        if(auto* body=r1::VehicleRuntime::body(*car);body&&body->physicsWorld())
            verticalVelocity=body->physicsWorld()->linearVelocity(body->bodyId()).y;
        driving=false;carSpeed=0;steerShown=0;
        dismountPlayerFromBike();
        if(auto* drive=r1::VehicleRuntime::driver(*car))drive->parked=true;
        // Step out looking where you were looking, not snapped back to the
        // bonnet: the offset becomes the walk's own yaw.
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        lon=geo.x;lat=geo.y;alt=groundAt(lon,lat,carAlt);
        jumpOffset=std::max(0.,geo.z-alt);jumpVelocity=std::clamp(double(verticalVelocity),-kFallTerminal,15.);
        swimming=false;
        player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+jumpOffset+.06)));
        player->setEnabled(true);
        player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        followDistance=std::min(followDistance,kOnFootFollow);
        text("stream-status","On foot. F: get back in the vehicle.");
        saida::Log::info("[World car] left at ",lon,", ",lat," speed=",exitSpeed," m/s, fall=",jumpOffset," m");
        request(lon,lat);
        return true;
    }
    void sinkCar(double x,double y) {
        if(car)if(auto* body=r1::VehicleRuntime::body(*car)) {
            r1::VehicleRuntime::synchronize(*car);body->kinematic=true;body->markDirty();
        }
        carWaterLevel=waterLevel(x,y);carSinkDepth=0;carSinking=true;
        carLon=x;carLat=y;carAlt=carWaterLevel;carSpeed=0;steerShown=0;
        // The driver swims out only when the water is over half his height.
        driving=false;swimming=deepWater(x,y);swimTime=0;swimLean=0;
        dismountPlayerFromBike();
        lon=x;lat=y;alt=swimming?carWaterLevel:height(x,y);yaw=wrap(carYaw+lookYaw);swimHeading=yaw;
        lookYaw=0;lookIdle=0;jumpOffset=jumpVelocity=0;
        followDistance=std::min(followDistance,kOnFootFollow);
        player->setEnabled(true);
        text("stream-status",swimming?"The vehicle is sinking — swim for the shore.":"The vehicle is bogged down — you can stand.");
        saida::Log::info("[World car] sank at ",lon,", ",lat);
        request(lon,lat);
    }
    void updateSinkingCar(double dt) {
        if(!carSinking||!carParked||!car)return;
        carSinkDepth=std::min(kCarSinkDepth,carSinkDepth+kCarSinkRate*dt);
        carAlt=carWaterLevel-carSinkDepth;
    }
    void captureCarPose() {
        r1::VehicleRuntime::synchronize(*car);
        const glm::dvec3 point=origin.origin+origin.basis*glm::dvec3(car->transform().position);
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        carLon=geo.x;carLat=geo.y;carAlt=geo.z-r1::VehicleRuntime::kBodyClearance;
        const glm::vec3 forward=car->transform().rotation*glm::vec3(0,0,-1);
        carYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
        if(auto* body=r1::VehicleRuntime::body(*car);body&&body->physicsWorld())
            carSpeed=glm::dot(body->physicsWorld()->linearVelocity(body->bodyId()),forward);
        if(driving){lon=carLon;lat=carLat;alt=carAlt;yaw=carYaw;}
    }
public:
    bool needsAfterSimulation() const {
        return driving&&car&&!pending&&playing&&(worldCapture.pngPath.empty()||!captureView.set);
    }
    void afterSimulation(float) {
        if(!needsAfterSimulation())return;
        auto* body=r1::VehicleRuntime::body(*car);
        if(!body||!body->physicsWorld()||body->kinematic)return;
        glm::vec3 position;glm::quat rotation;
        body->physicsWorld()->getBodyTransform(body->bodyId(),position,rotation);
        const auto forward=rotation*glm::vec3(0,0,-1);
        const double currentYaw=std::atan2(double(forward.x),-double(forward.z))/rad;
        const auto turn=glm::angleAxis(float(-wrap(currentYaw-carYaw)*rad),glm::vec3(0,1,0));
        // Input/camera placement precedes Jolt. Render the camera against the
        // same completed solver pose as the car, including multi-step frames.
        camera->transform().position=position+turn*(camera->transform().position-car->transform().position);
        camera->transform().rotation=glm::normalize(turn*camera->transform().rotation);
    }
private:
    void driveCar(double dt,double throttle,double steerInput,bool handbrake) {
        if(!car)return;
        captureCarPose();
        if(onWater(lon,lat,alt)){sinkCar(lon,lat);return;}
        auto* drive=r1::VehicleRuntime::driver(*car);if(!drive)return;
        constexpr double kReverseBrakeThreshold=.2;
        constexpr double kTerrainGuardMargin=8.;
        const double stoppingDistance=carSpeed*carSpeed/(2.*std::max(1.f,drive->braking))+kTerrainGuardMargin;
        const double guardDirection=std::abs(carSpeed)>kReverseBrakeThreshold?carSpeed:throttle;
        const double guardDistance=std::copysign(stoppingDistance,guardDirection);
        const auto ahead=onward(lon,lat,sin(carYaw*rad)*guardDistance,cos(carYaw*rad)*guardDistance);
        if(!tile(ahead.x,ahead.y))stream();
        float brake=0;
        if(!tile(ahead.x,ahead.y)) {
            throttle=0;brake=1;
            if(carStop!="terrain")saida::Log::info("[World car] braking before unloaded terrain at ",ahead.x,", ",ahead.y);
            carStop="terrain";
            text("stream-status","Edge of the loaded area — more data is on its way.");
        } else {
            carStop.clear();
            if(throttle*carSpeed<0&&std::abs(carSpeed)>kReverseBrakeThreshold) { throttle=0;brake=1; }
        }
        drive->command(float(throttle),brake,float(steerInput),handbrake);
        steerShown=drive->steering();
        wheelSpin+=carSpeed/vehicleSpec(*car).wheelRadius*dt;
        request(lon,lat);
    }
    // ── boats ───────────────────────────────────────────────────────────────
    //
    // Every boat the harbours moored can be taken: the manifest says where each
    // lies and how it handles, and the node that stood there is handed over,
    // exactly as a traffic car is. From the helm, the throttle and the rudder
    // are the walk's two axes; the boat carries its speed and heading from one
    // frame to the next, and it stops -- out loud -- where the water does.
    enum class BoatSource {None, Moored, Left, Sea};
    struct BoatReach {Loaded* tile=nullptr; size_t index=0; BoatSource source=BoatSource::None;};
    enum class HopResult {NoTarget, Refused, Boarded};
    static double segmentDistance(glm::dvec2 p,glm::dvec2 a,glm::dvec2 b) {
        const glm::dvec2 d=b-a;const double len=glm::dot(d,d);
        const double f=len>0?std::clamp(glm::dot(p-a,d)/len,0.,1.):0.;
        return glm::length(p-(a+f*d));
    }
    // Distance from the player to a hull's side, in metres, in `frame`.
    double hullDistance(const Frame& frame,glm::dvec3 centre,double heading,double length,double beam) const {
        const glm::dvec3 me=frame.local(ecef(lon,lat,alt));
        const glm::dvec2 axis(std::sin(heading*rad),-std::cos(heading*rad));
        const glm::dvec2 c(centre.x,centre.z);
        return segmentDistance({me.x,me.z},c-axis*(length*.5),c+axis*(length*.5))-beam*.5;
    }
    saida::Node* mooredNode(Loaded& t,Mooring& m) {
        if(!m.node&&!m.taken)m.node=t.node->findByPath(m.name);
        return m.node;
    }
    bool nearestBoat(BoatReach& out,double& distance,double extra=0) {
        distance=kBoatReach+extra;bool found=false;
        for(auto& [key,t]:loaded)
            for(size_t i=0;i<t.boats.size();++i) {
                Mooring& m=t.boats[i];
                if(m.taken||!mooredNode(t,m))continue;
                const double d=hullDistance(t.frame,m.local,m.heading,m.length,m.beam);
                if(d<distance){distance=d;out={&t,i,BoatSource::Moored};found=true;}
            }
        for(size_t i=0;i<leftBoats.size();++i) {
            const Vessel& v=leftBoats[i];
            const Frame here(v.lon,v.lat,v.alt);
            const double d=hullDistance(here,glm::dvec3(0),v.yaw,v.length,v.beam);
            if(d<distance){distance=d;out={nullptr,i,BoatSource::Left};found=true;}
        }
        for(size_t i=0;i<seaShips.size();++i) {
            const Vessel& v=seaShips[i].v;
            const Frame here(v.lon,v.lat,v.alt);
            const double d=hullDistance(here,glm::dvec3(0),v.yaw,v.length,v.beam);
            if(d<distance){distance=d;out={nullptr,i,BoatSource::Sea};found=true;}
        }
        return found;
    }
    bool enterBoat() {
        if(sailing||driving)return false;
        BoatReach target;double distance=0;
        if(!nearestBoat(target,distance))return false;
        return board(target);
    }
    bool board(const BoatReach& target) {
        if(target.source==BoatSource::None)return false;
        if(target.source==BoatSource::Sea) {
            // A ship under way is taken under way: the helm keeps her speed.
            boat=seaShips[target.index].v;
            seaTaken.insert(seaShips[target.index].id);
            seaShips.erase(seaShips.begin()+long(target.index));
        } else if(target.source==BoatSource::Left) {
            boat=leftBoats[target.index];
            leftBoats.erase(leftBoats.begin()+long(target.index));
        } else {
            Loaded& t=*target.tile;Mooring& m=t.boats[target.index];
            auto owned=t.node->detachChild(m.node);
            if(!owned) {
                saida::Log::warn("[World boat] ",m.name," could not be detached from its tile");
                return false;
            }
            m.taken=true;m.node=nullptr;
            const auto where=geodeticOf(t,glm::vec3(m.local));
            boat=Vessel{};
            boat.node=engine.sceneTree().world().addChild(std::move(owned));
            boat.kind=m.kind;boat.length=m.length;boat.beam=m.beam;
            boat.top=m.top;boat.accel=m.accel;boat.turn=m.turn;
            boat.lon=where.x;boat.lat=where.y;boat.yaw=m.heading;
            boat.alt=waterLevel(boat.lon,boat.lat);
        }
        sailing=true;swimming=false;lookYaw=0;lookIdle=0;
        lon=boat.lon;lat=boat.lat;alt=boat.alt;yaw=boat.yaw;
        player->setEnabled(false);
        for(auto* a:animators)a->play("idle");
        jumpOffset=jumpVelocity=0;
        text("stream-status","At the helm. F: disembark near a quay · S: reverse.");
        saida::Log::info("[World boat] took the helm of a ",boat.kind," (",boat.length," m) at ",lon,", ",lat);
        return true;
    }
    void keepBoat(Vessel vessel) {
        vessel.speed=0;
        leftBoats.push_back(std::move(vessel));
        while(leftBoats.size()>kLeftBoats) {
            if(leftBoats.front().node)leftBoats.front().node->queueFree();
            leftBoats.erase(leftBoats.begin());
        }
    }
    struct BoatExit {glm::dvec2 where; bool swimming;};
    std::optional<BoatExit> boatExit() {
        const double s=std::sin(boat.yaw*rad),c=std::cos(boat.yaw*rad);
        std::optional<BoatExit> water;
        for(double off:{1.,2.,3.5})
            for(double along=0;along<=boat.length*.5;along+=2.)
                for(double sign:{1.,-1.})for(double side:{1.,-1.}) {
                    const double a=along*sign,o=(boat.beam*.5+off)*side;
                    const auto q=onward(boat.lon,boat.lat,s*a+c*o,c*a-s*o);
                    if(!tile(q.x,q.y))continue;
                    if(deepWater(q.x,q.y)) {
                        if(!water)water=BoatExit{q,true};
                    } else if(!footBlocked(q.x,q.y))return BoatExit{q,false};
                }
        return water;
    }
    // At a quay the player steps ashore. Offshore, he drops into the water and
    // can swim back to a bank or to the same hull. Speed still makes either
    // manoeuvre unsafe.
    bool leaveBoat() {
        if(!sailing)return false;
        if(std::abs(boat.speed)>kBoatExitSpeed) {
            text("stream-status","Too fast to disembark — slow down.");
            saida::Log::info("[World boat] landing refused at ",std::abs(boat.speed)," m/s");
            return false;
        }
        const auto exit=boatExit();
        if(!exit) {
            text("stream-status","Water and shore outside the loaded area — wait for the data.");
            saida::Log::info("[World boat] no loaded exit beside ",boat.lon,", ",boat.lat);
            return false;
        }
        sailing=false;keepBoat(boat);boat=Vessel{};
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        lon=exit->where.x;lat=exit->where.y;swimming=exit->swimming;
        swimTime=0;swimHeading=yaw;swimLean=0;
        alt=swimming?waterLevel(lon,lat):height(lon,lat);
        player->setEnabled(true);
        player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        followDistance=std::min(followDistance,kOnFootFollow);
        text("stream-status",swimming?"In the water. WASD/ZQSD: swim · F: climb back aboard."
                                     :"Ashore. F: take the boat again.");
        saida::Log::info(swimming?"[World boat] swimming at ":"[World boat] ashore at ",lon,", ",lat);
        request(lon,lat);
        return true;
    }
    // From one hull to another: alongside, at matched speeds. The boat left
    // behind stays where she is, like any boat stepped off.
    HopResult hopAboard() {
        if(!sailing)return HopResult::NoTarget;
        BoatReach target;double distance=0;
        if(!nearestBoat(target,distance,boat.beam*.5))return HopResult::NoTarget;
        const Vessel* other=target.source==BoatSource::Sea?&seaShips[target.index].v
                            :target.source==BoatSource::Left?&leftBoats[target.index]:nullptr;
        const glm::dvec2 mine(std::sin(boat.yaw*rad)*boat.speed,std::cos(boat.yaw*rad)*boat.speed);
        const glm::dvec2 theirs=other?glm::dvec2(std::sin(other->yaw*rad)*other->speed,
                                                  std::cos(other->yaw*rad)*other->speed):glm::dvec2(0);
        const double relativeSpeed=glm::length(mine-theirs);
        if(relativeSpeed>kHopSpeed) {
            text("stream-status","Match its speed to step aboard.");
            saida::Log::info("[World boat] hop refused at ",relativeSpeed," m/s relative");
            return HopResult::Refused;
        }
        Vessel left=boat;left.speed=0;
        sailing=false;boat=Vessel{};
        if(!board(target)) {boat=left;sailing=true;return HopResult::Refused;}
        keepBoat(std::move(left));
        saida::Log::info("[World boat] stepped across to a ",boat.kind," (",boat.length," m)");
        return HopResult::Boarded;
    }
    void clearBoats() {
        for(auto& v:leftBoats)if(v.node)v.node->queueFree();
        leftBoats.clear();
        for(auto& s:seaShips)if(s.v.node)s.v.node->queueFree();
        seaShips.clear();seaTaken.clear();seaFirst=true;seaAsk=5.;
        if(boat.node)boat.node->queueFree();
        boat=Vessel{};sailing=false;
    }
    // One step of the boat. Water resists with the square of the speed, so a
    // hull settles at its top speed rather than being clamped to it; the rudder
    // needs way on to bite, and bites the other way going astern.
    void sailBoat(double dt,double throttle,double rudder) {
        const double drag=boat.accel/(boat.top*boat.top);
        double thrust=0;
        if(throttle>0)thrust=boat.accel*throttle;
        else if(throttle<0)thrust=boat.speed>.3?-2.*boat.accel:boat.accel*throttle*.5;
        double coast=throttle==0?.12*boat.accel*(boat.speed>0?1.:boat.speed<0?-1.:0.):0.;
        boat.speed+=(thrust-drag*boat.speed*std::abs(boat.speed)-coast)*dt;
        if(throttle==0&&std::abs(boat.speed)<.05)boat.speed=0;
        boat.speed=std::clamp(boat.speed,-.3*boat.top,boat.top);
        const double way=std::clamp(std::abs(boat.speed)/(.25*boat.top),.15,1.);
        boat.yaw=wrap(boat.yaw+rudder*boat.turn*way*dt*(boat.speed<-.05?-1.:1.));
        yaw=boat.yaw;
        if(std::abs(boat.speed)<1e-3)return;
        const double s=std::sin(boat.yaw*rad),c=std::cos(boat.yaw*rad);
        auto next=onward(lon,lat,s*boat.speed*dt,c*boat.speed*dt);
        const double lead=boat.length*.5*(boat.speed>=0?1.:-1.);
        auto bow=onward(next.x,next.y,s*lead,c*lead);
        if(!tile(next.x,next.y)||!tile(bow.x,bow.y))stream();
        if(!tile(next.x,next.y)||!tile(bow.x,bow.y)) {
            boat.speed=0;
            text("stream-status","Edge of the loaded area — more data is on its way.");
            return;
        }
        if(!navigable(next.x,next.y)||!navigable(bow.x,bow.y)) {
            // Aground is a refusal like any other: the boat stops, and the
            // player is told which way is out.
            boat.speed=0;
            text("stream-status","Shore or shallows — the boat stops. S: reverse.");
            return;
        }
        lon=next.x;lat=next.y;alt=waterLevel(lon,lat);
        boat.lon=lon;boat.lat=lat;boat.alt=alt;
        request(lon,lat);
    }
    // Afloat: a heave and a slow pitch and roll that shrink with the hull, so
    // a dinghy bobs and a container ship does not.
    glm::quat boatSway(const Vessel& v,double phase,double& heave) const {
        const double size=std::min(1.,9./std::max(1.,v.length));
        heave=.12*size*std::sin(seaTime*1.1+phase);
        const double pitch=2.5*size*std::sin(seaTime*1.3+phase*1.7);
        const double roll=3.5*size*std::sin(seaTime*.9+phase*.6);
        return glm::angleAxis(float(-v.yaw*rad),glm::vec3(0,1,0))
              *glm::angleAxis(float(pitch*rad),glm::vec3(1,0,0))
              *glm::angleAxis(float(roll*rad),glm::vec3(0,0,1));
    }
    void placeBoats() {
        auto place=[&](const Vessel& v,double phase){
            if(!v.node)return;
            double heave=0;const glm::quat sway=boatSway(v,phase,heave);
            v.node->transform().position=glm::vec3(origin.local(ecef(v.lon,v.lat,v.alt+heave)));
            v.node->transform().rotation=sway;
        };
        if(sailing)place(boat,0.);
        for(size_t i=0;i<leftBoats.size();++i)place(leftBoats[i],double(i+1)*1.9);
        for(auto& s:seaShips)place(s.v,s.phase);
    }

    // ── aircraft ────────────────────────────────────────────────────────────
    //
    // Every aircraft the generator parked can be taken, the way a moored boat
    // or a traffic car is: the node on the stand is handed over, never copied.
    // Three handlings, one per class (assets/models/aircraft/fleet.json):
    //
    //  - an airliner is heavy: its engines take seconds to spool, it rolls
    //    slowly, it needs a long run to rotate;
    //  - a business jet is quick in everything and turns on a wingtip;
    //  - a helicopter spins up, hovers by itself when the stick is let go,
    //    climbs and descends on its own keys and goes where it points.
    //
    // Arcade, as the car is (README, "The aircraft"): turns are coordinated
    // and faster than real ones, and nothing crashes. A building stops an
    // aircraft where it touches it, out loud, and the ground takes a hard
    // landing as a stop.
    static void aircraftLod(saida::Node& node) {
        auto* lod=node.addBehaviour<saida::LODGroupBehaviour>();
        lod->setLevels({{"Near",.03f},{"Far",0.f}});
    }
    void buildAircraftPrototypes() {
        for(const auto& type:r1::palette().aircraft) {
            auto root=std::make_unique<saida::Node>("aircraft-"+type.name);
            for(const auto& level:std::vector<std::pair<std::string,std::string>>{{type.nearModel,"Near"},{type.farModel,"Far"}}) {
                auto* source=prototypeForStartup(level.first);
                if(!source)throw std::runtime_error("Aircraft model failed: "+level.first);
                auto child=clonePlant(*source);child->setName(level.second);
                child->transform().rotation=glm::quat(0,0,1,0); // Authored nose +Z.
                root->addChild(std::move(child));
            }
            aircraftPrototypes[type.name]=prototypes->addChild(std::move(root));
        }
        saida::Log::info("[World aircraft] ",aircraftPrototypes.size()," aircraft types ready");
    }
    // The tile's parked aircraft, as scene nodes in its frame. Children of the
    // tile, so evicting the tile clears them with it.
    // Lettering (gen/predict facadeLettering): the sign font's glyphs, inked
    // with its coverage so no baked plate shows, in `colour`, facing +Z.
    std::unique_ptr<saida::Node> lettering(const std::string& text,double width,double height,glm::vec3 colour) {
        auto letters=r1::facadeLettering(text,width,height);if(!letters)return nullptr;
        auto sign=plantNode(*letters);if(!sign)return nullptr;
        saida::MaterialDesc desc;
        desc.baseColor={colour.x,colour.y,colour.z,1.f};
        desc.roughness=.55f;desc.alphaCutoff=.35f;
        desc.albedoId=texture("assets/textures/interiors/fascia_letters.png",true);
        auto* ink=engine.resources().getMaterial(desc);
        std::function<void(saida::Node&)> paint=[&](saida::Node& node) {
            if(auto* mesh=dynamic_cast<saida::MeshNode*>(&node)){mesh->setMaterial(ink);mesh->castShadows()=false;}
            for(auto& child:node.children())paint(*child);
        };
        paint(*sign);
        return sign;
    }
    // A tile's own lettering: fuel canopies and totems (gen/fuel).
    void mountLettering(Loaded& t) {
        const auto started=std::chrono::steady_clock::now();
        auto list=t.data.find("lettering");if(list==t.data.end())return;
        while(t.nextLettering<list->size()) {
            const auto& entry=(*list)[t.nextLettering];
            const std::string text=entry.at("text");
            const auto letters=r1::facadeLettering(text,entry.at("width"),entry.at("height"));
            if(letters&&!modelsReady(*letters))return;
            ++t.nextLettering;
            const auto& c=entry.at("colour");
            auto sign=lettering(text,entry.at("width"),entry.at("height"),{float(c[0]),float(c[1]),float(c[2])});
            if(!sign){saida::Log::warn("[World lettering] font cannot spell: ",text);continue;}
            const auto& at=entry.at("at");const auto& n=entry.at("normal");
            sign->transform().position={float(at[0]),float(at[1]),float(at[2])};
            sign->transform().rotation=glm::angleAxis(float(std::atan2(n[0].get<double>(),n[1].get<double>())),glm::vec3(0,1,0));
            t.node->addChild(std::move(sign));
            if(msSince(started)>=kPropImportMs)return;
        }
    }
    void mountAircraft(Loaded& t) {
        for(auto& spot:t.aircraft) {
            auto found=aircraftPrototypes.find(spot.type);
            if(found==aircraftPrototypes.end()) {
                saida::Log::warn("[World aircraft] ",t.data.value("key",std::string())," parks an unknown type: ",spot.type);
                continue;
            }
            auto node=clonePlant(*found->second);
            node->setName(spot.name);
            aircraftLod(*node);
            node->transform().position=glm::vec3(spot.local);
            node->transform().rotation=glm::angleAxis(float(-spot.heading*rad),glm::vec3(0,1,0));
            spot.node=t.node->addChild(std::move(node));
        }
    }
    static void collectAircraftParts(Aircraft& a) {
        a.gear.clear();a.mainRotors.clear();a.tailRotors.clear();
        // Outermost match only, as with the car's wheels: the importer wraps a
        // named node around a mesh node that inherits its name.
        std::function<void(saida::Node&)> walk=[&](saida::Node& n){
            if(n.name()=="gear"){a.gear.push_back(&n);return;}
            if(n.name()=="rotor-main"){a.mainRotors.push_back(&n);return;}
            if(n.name()=="rotor-tail"){a.tailRotors.push_back(&n);return;}
            for(auto& c:n.children())walk(*c);
        };
        if(a.node)walk(*a.node);
    }
    static double fuselageHalf(const r1::AircraftType& t) {return std::max(1.2,t.length*.05);}
    double roofAt(double x,double y,double above) {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return -1e30;
        const glm::vec3 start(origin.local(ecef(x,y,above+1.)));
        const auto hit=physics->raycast(start,{0,-1,0},500.f,obstacleFilter());
        if(!hit.hit||hit.normal.y<.5f)return -1e30;
        const double surface=above+1.+double(hit.point.y-start.y);
        return surface>groundAt(x,y,above)+1.?surface:-1e30;
    }
    bool buildingAt(double x,double y,double level) {
        return solidAt(x,y,level,.45f);
    }
    // What an aircraft rests on at (x, y): the terrain, a deck, the water's
    // level, or a roof it is above. Where no tile answers yet, the last
    // surface it saw -- said on screen, never guessed at.
    double surfaceUnder(double x,double y,double above,bool* onRoof=nullptr,bool* wet=nullptr) {
        if(onRoof)*onRoof=false;
        if(wet)*wet=false;
        if(!tile(x,y))return planeGround;
        double surface=height(x,y,above);
        if(onWater(x,y)){surface=waterLevel(x,y);if(wet)*wet=true;}
        const double roof=roofAt(x,y,above);
        if(roof>surface){surface=roof;if(onRoof)*onRoof=true;if(wet)*wet=false;}
        planeGround=surface;
        return surface;
    }
    // The aircraft's extremities -- nose, tail and the two tips of its wings
    // or its rotor -- against the buildings, at the height of its underside.
    bool aircraftHits(double x,double y,double alt,double heading,bool sweep=false) {
        const auto& t=*plane.type;
        const double s=std::sin(heading*rad),c=std::cos(heading*rad);
        const double half=t.length*.5,span=t.span*.5,body=alt+.4;
        for(const auto& [a,o]:{std::pair{0.,0.},std::pair{half,0.},std::pair{-half,0.},std::pair{0.,span},std::pair{0.,-span}}) {
            const auto q=onward(x,y,s*a+c*o,c*a-s*o);
            if(buildingAt(q.x,q.y,body))return true;
            if(sweep) {
                const auto from=onward(plane.lon,plane.lat,s*a+c*o,c*a-s*o);
                const glm::vec3 start(origin.local(ecef(from.x,from.y,body))),end(origin.local(ecef(q.x,q.y,body)));
                const glm::vec3 delta=end-start;const float length=glm::length(delta);
                auto* physics=engine.sceneTree().world().physics();
                if(physics&&length>.001f&&physics->raycast(start,delta/length,length,obstacleFilter()).hit)return true;
            }
        }
        return false;
    }
    enum class PlaneSource {None, Parked, Left};
    struct PlaneReach {Loaded* tile=nullptr; size_t index=0; PlaneSource source=PlaneSource::None;};
    bool nearestAircraft(PlaneReach& out,double& distance) {
        distance=kAircraftReach;bool found=false;
        for(auto& [key,t]:loaded)
            for(size_t i=0;i<t.aircraft.size();++i) {
                const AircraftSpot& spot=t.aircraft[i];
                const auto* type=r1::aircraftType(spot.type);
                if(spot.taken||!spot.node||!type)continue;
                const double d=hullDistance(t.frame,spot.local,spot.heading,type->length,2*fuselageHalf(*type));
                if(d<distance){distance=d;out={&t,i,PlaneSource::Parked};found=true;}
            }
        for(size_t i=0;i<leftAircraft.size();++i) {
            const Aircraft& a=leftAircraft[i];
            if(a.airborne)continue;  // still falling
            const Frame here(a.lon,a.lat,a.alt);
            const double d=hullDistance(here,glm::dvec3(0),a.yaw,a.type->length,2*fuselageHalf(*a.type));
            if(d<distance){distance=d;out={nullptr,i,PlaneSource::Left};found=true;}
        }
        return found;
    }
    bool enterAircraft() {
        if(piloting||driving||sailing)return false;
        PlaneReach target;double distance=0;
        if(!nearestAircraft(target,distance))return false;
        Aircraft a;
        if(target.source==PlaneSource::Left) {
            a=leftAircraft[target.index];
            leftAircraft.erase(leftAircraft.begin()+long(target.index));
        } else {
            Loaded& t=*target.tile;AircraftSpot& spot=t.aircraft[target.index];
            auto owned=t.node->detachChild(spot.node);
            if(!owned) {
                saida::Log::warn("[World aircraft] ",spot.name," could not be detached from its tile");
                text("stream-status","This aircraft cannot be taken.");
                return false;
            }
            spot.taken=true;spot.node=nullptr;
            const auto where=geodeticOf(t,glm::vec3(spot.local));
            a.node=engine.sceneTree().world().addChild(std::move(owned));
            a.type=r1::aircraftType(spot.type);
            a.lon=where.x;a.lat=where.y;a.yaw=spot.heading;
            a.alt=surfaceUnder(a.lon,a.lat,alt+1.);
        }
        a.speed=0;a.climb=0;a.lever=0;a.thrust=0;a.airborne=false;a.pitch=0;a.roll=0;
        collectAircraftParts(a);
        plane=std::move(a);piloting=true;swimming=false;planeStopped=false;
        lookYaw=0;lookIdle=0;
        lon=plane.lon;lat=plane.lat;alt=plane.alt;yaw=plane.yaw;
        player->setEnabled(false);
        for(auto* anim:animators)anim->play("idle");
        jumpOffset=jumpVelocity=0;
        text("stream-status",plane.helicopter()
            ?"Flying. Space/Shift: climb/descend · W/S: forward/back · A/D: turn · F: get out."
            :"Flying. W/S: throttle · A/D: bank · Space/Shift: pitch up/down · F: get out.");
        saida::Log::info("[World aircraft] took the controls of a ",plane.type->name," at ",lon,", ",lat,
                         plane.helicopter()?" (helicopter)":"");
        return true;
    }
    void keepAircraft(Aircraft a) {
        a.speed=0;a.climb=0;a.lever=0;a.thrust=0;
        leftAircraft.push_back(std::move(a));
        while(leftAircraft.size()>kLeftAircraft) {
            if(leftAircraft.front().node)leftAircraft.front().node->queueFree();
            leftAircraft.erase(leftAircraft.begin());
        }
    }
    // F leaves the controls at any moment, as in GTA. On the ground the pilot
    // steps down beside the nose, on the left where the doors are; on water he
    // drops into it. In the air or on a roof he jumps: he falls from the
    // aircraft's height to the ground beside it, and an aircraft left in the
    // air falls too (World::dropLeftAircraft).
    bool leaveAircraft() {
        if(!piloting)return false;
        const auto& t=*plane.type;
        const double s=std::sin(plane.yaw*rad),c=std::cos(plane.yaw*rad),half=fuselageHalf(t);
        std::optional<glm::dvec2> dry,wet;
        for(double along:{t.length*.3,t.length*.42,0.,-t.length*.25})
            for(double side:{-1.,1.})
                for(double off:{2.,4.}) {
                    if(dry)break;
                    const double o=(half+off)*side;
                    const auto q=onward(plane.lon,plane.lat,s*along+c*o,c*along-s*o);
                    if(!tile(q.x,q.y))continue;
                    if(deepWater(q.x,q.y)){if(!wet)wet=q;}
                    else if(!footBlocked(q.x,q.y))dry=q;
                }
        double x=0,y=0;
        // Over a city the ground beside the fuselage may all be roofs: in the
        // air he can come down further off, in the open.
        const double around=plane.airborne?t.span*.5+80.:t.span*.5+8.;
        if(!dry&&!wet&&freeSpot(plane.lon,plane.lat,around,x,y))dry=glm::dvec2(x,y);
        if(!dry&&!wet) {
            text("stream-status","Cannot get out here — the ground below is not loaded yet.");
            saida::Log::warn("[World aircraft] no loaded ground beside the aircraft at ",plane.lon,", ",plane.lat);
            return false;
        }
        const glm::dvec2 at=dry?*dry:*wet;
        const double from=plane.alt,climb=plane.climb;
        piloting=false;keepAircraft(plane);plane=Aircraft{};
        yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
        lon=at.x;lat=at.y;
        swimTime=0;swimHeading=yaw;swimLean=0;
        alt=dry?height(lon,lat):waterLevel(lon,lat);
        // Above what he lands on, he falls, into the water too; he swims once
        // he is in it (the landing, in the frame's walk).
        const double drop=from-alt;
        const bool falls=drop>.5;
        swimming=!dry&&!falls;
        jumpOffset=falls?drop:0.;
        jumpVelocity=falls?std::clamp(climb,-kFallTerminal,15.):0.;
        player->setEnabled(true);
        player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
        followDistance=std::min(followDistance,kOnFootFollow);
        text("stream-status",falls?"Jump!":swimming?"In the water. F: climb back aboard.":"On foot. F: take the aircraft again.");
        saida::Log::info("[World aircraft] ",falls?"jumped":"stepped down"," at ",lon,", ",lat,
                         falls?", falling "+std::to_string(int(drop))+" m":"",!dry?" into the water":"");
        request(lon,lat);
        return true;
    }
    // An aircraft left in the air falls, levelling, until it rests on what is
    // under it: the ground, a roof or the water. It stops there, no crash (the
    // player's call). Over ground not loaded yet it waits for it.
    void dropLeftAircraft(Aircraft& a,double dt) {
        if(!a.airborne||!tile(a.lon,a.lat))return;
        a.climb=std::max(a.climb-kGravity*dt,-kFallTerminal);
        a.alt+=a.climb*dt;
        double under=onWater(a.lon,a.lat)?waterLevel(a.lon,a.lat):height(a.lon,a.lat);
        under=std::max(under,roofAt(a.lon,a.lat,a.alt+1.));
        a.pitch*=std::exp(-dt);a.roll*=std::exp(-dt);
        if(a.alt>under)return;
        a.alt=under;a.climb=0;a.airborne=false;a.pitch=0;a.roll=0;
        saida::Log::info("[World aircraft] a ",a.type->name," left in the air came down at ",a.lon,", ",a.lat);
    }
    void clearAircraft() {
        for(auto& a:leftAircraft)if(a.node)a.node->queueFree();
        leftAircraft.clear();
        if(plane.node)plane.node->queueFree();
        plane=Aircraft{};piloting=false;
    }
    // One horizontal step. A building stops the aircraft where it touches it
    // and says so -- no crash (the request was explicit). The edge of the
    // loaded world stops it on the ground; in the air it flies on over the
    // last surface it saw, and the HUD says the ground ahead is unknown.
    bool moveAircraft(double dt,double horizontal) {
        if(std::abs(horizontal)<1e-4)return true;
        const double s=std::sin(plane.yaw*rad),c=std::cos(plane.yaw*rad);
        const auto next=onward(plane.lon,plane.lat,s*horizontal*dt,c*horizontal*dt);
        if(!tile(next.x,next.y))stream();
        if(!tile(next.x,next.y)&&!plane.airborne) {
            plane.speed=0;
            text("stream-status","Edge of the loaded area — more data is on its way.");
            return false;
        }
        const double heading=horizontal<0?wrap(plane.yaw+180.):plane.yaw;
        if(aircraftHits(next.x,next.y,plane.alt,heading,true)&&!aircraftHits(plane.lon,plane.lat,plane.alt,heading)) {
            plane.speed=0;
            if(!planeStopped) {
                text("stream-status","Building — the aircraft stops.");
                saida::Log::info("[World aircraft] stopped by a building at ",plane.lon,", ",plane.lat,
                                 ", ",plane.alt-planeGround," m up");
            }
            planeStopped=true;
            return false;
        }
        if(!plane.airborne&&!plane.helicopter()&&tile(next.x,next.y)&&onWater(next.x,next.y)&&!onWater(plane.lon,plane.lat)) {
            plane.speed=0;
            text("stream-status","Water's edge — the aircraft stops.");
            return false;
        }
        planeStopped=false;
        plane.lon=next.x;plane.lat=next.y;
        return true;
    }
    // A plane. W/S move the throttle lever, and the engines follow it at
    // their own pace; on the ground S at idle brakes, then pushes back. A/D
    // steer the nose wheel on the ground and bank in the air; the turn
    // follows the bank. Space pulls the nose up, Shift pushes it down, and
    // with neither the nose settles back to level.
    void flyPlane(double dt,double f,double r,double pitchIn) {
        Aircraft& p=plane;const auto& t=*p.type;
        p.lever=std::clamp(p.lever+f*.45*dt,0.,1.);
        p.thrust+=(p.lever-p.thrust)*(1-std::exp(-t.spool*3.*dt));
        const double ratio=p.speed/t.top,drag=t.accel*ratio*std::abs(ratio);
        if(!p.airborne) {
            bool wet=false;
            surfaceUnder(p.lon,p.lat,p.alt+2.,nullptr,&wet);
            const double v=p.speed;
            const double sign=v>0?1.:v<0?-1.:0.;
            double a=t.accel*p.thrust-drag-(wet?1.5:.3)*sign;
            if(f<0&&p.lever<=0.)a=v>.3?-t.brake:(v>-3.?-1.5:0.);
            p.speed=v+a*dt;
            if(sign>0&&p.speed<0&&!(f<0&&p.lever<=0.))p.speed=0;
            if(sign<0&&p.speed>0)p.speed=0;
            const double speed=std::abs(p.speed);
            if(speed>.2)p.yaw=wrap(p.yaw+r*std::min(r1::degrees(speed/t.turnRadius),30.)*dt*(p.speed<0?-1.:1.));
            if(pitchIn>0&&p.speed>=t.rotate)p.pitch=std::min(t.maxPitch*.6,p.pitch+t.pitchRate*dt);
            else p.pitch=std::max(0.,p.pitch-t.pitchRate*dt);
            p.roll-=p.roll*(1-std::exp(-4.*dt));
            if(p.pitch>2.5&&p.speed>=t.rotate) {
                // Off the ground with the climb the rotation already gives,
                // or the next hump of the terrain would set it back down.
                p.airborne=true;p.climb=std::max(1.5,p.speed*std::sin(p.pitch*rad));p.alt+=.2;
                text("stream-status","Take-off.");
                saida::Log::info("[World aircraft] lift-off at ",p.speed," m/s");
            }
            moveAircraft(dt,p.speed);
            if(!p.airborne)p.alt=surfaceUnder(p.lon,p.lat,p.alt+2.);
            return;
        }
        const double bank=r*t.maxBank;
        p.roll+=std::clamp(bank-p.roll,-t.rollRate*dt,t.rollRate*dt);
        if(pitchIn!=0)p.pitch+=pitchIn*t.pitchRate*dt;
        else p.pitch-=std::clamp(p.pitch,-.3*t.pitchRate*dt,.3*t.pitchRate*dt);
        p.pitch=std::clamp(p.pitch,-t.maxPitch,t.maxPitch);
        // Climbing costs speed and diving gives it -- half of what gravity
        // would, which is the arcade in it.
        p.speed=std::max(0.,p.speed+(t.accel*p.thrust-drag-kGravity*std::sin(p.pitch*rad)*.5)*dt);
        // A coordinated turn, half again as quick as a real one.
        const double v=std::max(p.speed,t.stall*.6);
        p.yaw=wrap(p.yaw+r1::degrees(kGravity*std::tan(p.roll*rad)/v)*1.5*dt);
        double vertical=p.speed*std::sin(p.pitch*rad);
        if(p.speed<t.stall) {
            // The wing gives way: the aircraft sinks and its nose drops.
            const double k=(t.stall-p.speed)/t.stall;
            vertical-=14.*k;
            p.pitch-=18.*k*dt;
        }
        p.climb+=(vertical-p.climb)*(1-std::exp(-3.*dt));
        moveAircraft(dt,p.speed*std::cos(p.pitch*rad));
        p.alt+=p.climb*dt;
        bool onRoof=false,wet=false;
        const double ground=surfaceUnder(p.lon,p.lat,p.alt,&onRoof,&wet);
        if(p.alt<=ground) {
            p.alt=ground;p.airborne=false;
            const bool hard=p.climb<-7.||std::abs(p.roll)>25.||p.pitch<-8.;
            if(hard){p.speed=0;p.lever=0;p.thrust=0;}
            text("stream-status",hard?"Hard landing — the aircraft stops."
                                 :wet?"Water landing.":onRoof?"Landed on the roof.":"Landing.");
            saida::Log::info("[World aircraft] ",hard?"hard landing":"landed"," at ",p.climb," m/s vertical, ",
                             p.speed," m/s",wet?" on water":onRoof?" on a roof":"");
            p.climb=0;p.roll=0;p.pitch=std::max(0.,p.pitch);
        }
    }
    // A helicopter. The rotor takes three seconds to come up to speed. Space
    // and Shift climb and descend and it holds its height with neither;
    // W/S fly it forward and back, A/D turn it on the spot. It sits down on
    // the ground, on water or on a roof, wherever it is lowered.
    void flyHelicopter(double dt,double f,double r,double up) {
        Aircraft& h=plane;const auto& t=*h.type;
        h.rotor=std::min(1.,h.rotor+dt/3.);
        const bool lift=h.rotor>.85;
        double want=lift?t.climb*up:(h.airborne?-3.:0.);
        if(!h.airborne&&want<0)want=0;
        h.climb+=(want-h.climb)*(1-std::exp(-3.*dt));
        const double target=h.airborne?(f>=0?f*t.top:f*t.top*.3):0.;
        h.speed+=std::clamp(target-h.speed,-t.accel*dt,t.accel*dt);
        if(!h.airborne)h.speed*=std::exp(-6.*dt);
        if(lift)h.yaw=wrap(h.yaw+r*t.rollRate*dt*(h.airborne?1.:.35));
        const double pitchTarget=h.airborne?-t.maxPitch*std::clamp(target/t.top,-.4,1.):0.;
        const double rollTarget=h.airborne?r*t.maxBank*std::clamp(.3+std::abs(h.speed)/15.,0.,1.):0.;
        h.pitch+=(pitchTarget-h.pitch)*(1-std::exp(-2.5*dt));
        h.roll+=(rollTarget-h.roll)*(1-std::exp(-2.5*dt));
        moveAircraft(dt,h.speed);
        h.alt+=h.climb*dt;
        bool onRoof=false,wet=false;
        const double ground=surfaceUnder(h.lon,h.lat,h.alt+.5,&onRoof,&wet);
        if(h.alt<=ground) {
            if(h.airborne) {
                text("stream-status",onRoof?"Landed on the roof.":wet?"Landed on the water.":"Landed.");
                saida::Log::info("[World aircraft] helicopter set down",onRoof?" on a roof":wet?" on water":"",
                                 " at ",h.lon,", ",h.lat);
            }
            h.alt=ground;h.airborne=false;h.climb=std::max(0.,h.climb);
        } else if(h.alt>ground+.3&&!h.airborne) {
            h.airborne=true;
            text("stream-status","Take-off.");
            saida::Log::info("[World aircraft] helicopter lifted off");
        }
        h.mainSpin=std::fmod(h.mainSpin+h.rotor*h.rotor*28.*dt,kTwoPi);
        h.tailSpin=std::fmod(h.tailSpin+h.rotor*h.rotor*44.*dt,kTwoPi);
    }
    glm::quat aircraftRotation(const Aircraft& a) const {
        return glm::angleAxis(float(-a.yaw*rad),glm::vec3(0,1,0))
              *glm::angleAxis(float(a.pitch*rad),glm::vec3(1,0,0))
              *glm::angleAxis(float(-a.roll*rad),glm::vec3(0,0,1));
    }
    // Turned about the centre of gravity, so a bank rolls the aircraft about
    // itself rather than about its wheels.
    void placeAircraft(double dt) {
        auto place=[&](Aircraft& a){
            if(!a.node||!a.type)return;
            const glm::quat q=aircraftRotation(a);
            const glm::vec3 cg=glm::vec3(origin.local(ecef(a.lon,a.lat,a.alt+a.type->cg)));
            a.node->transform().position=cg-q*glm::vec3(0,float(a.type->cg),0);
            a.node->transform().rotation=q;
            const bool down=!a.airborne||a.alt-groundAt(a.lon,a.lat,a.alt)<30.;
            for(auto* g:a.gear)g->setVisible(down);
            for(auto* m:a.mainRotors)m->transform().rotation=glm::angleAxis(float(a.mainSpin),glm::vec3(0,1,0));
            for(auto* m:a.tailRotors)m->transform().rotation=glm::angleAxis(float(a.tailSpin),glm::vec3(1,0,0));
        };
        if(piloting)place(plane);
        for(auto& a:leftAircraft) {
            // A helicopter left behind winds down rather than stopping dead.
            a.rotor=std::max(0.,a.rotor-dt/8.);
            a.mainSpin=std::fmod(a.mainSpin+a.rotor*a.rotor*28.*dt,kTwoPi);
            a.tailSpin=std::fmod(a.tailSpin+a.rotor*a.rotor*44.*dt,kTwoPi);
            dropLeftAircraft(a,dt);
            place(a);
        }
    }

    // ── ships at sea ────────────────────────────────────────────────────────
    //
    // The sea service predicts the ships around the player -- six years of
    // real AIS, the day, the weather, and whatever live AIS it last heard
    // (gen/sea.cpp) -- on its own thread. The game sails them.
    // A ship keeps its own state once it is out: a later prediction that no
    // longer lists it does not pull it from under the player's eyes, it sails
    // on and is dropped beyond kSeaKeep; a new one appears only beyond
    // kSeaAppear, so nothing pops into view. Each can be boarded.
    bool gameTime(double& out) {
        if(!sunScript)return false;
        json result;
        if(sunScript->callExport("gameTime",json::array(),result)!=saida::ScriptCallStatus::Succeeded
           ||!result.is_number())return false;
        out=result.get<double>();
        return true;
    }
    bool localConditions() const {
        return conditions.is_object()&&conditions.contains("lon")&&conditions.contains("lat")
            &&r1::weatherLocal(conditions.value("lon",1000.),conditions.value("lat",1000.),lon,lat);
    }
    // Keep observations separate: returning to real weather restores the
    // local forecast, while a forced atmosphere applies at every coordinate.
    using Weather=r1::WeatherState;
    Weather observedWeather;
    Weather effectiveWeather() const {
        auto sample=r1::resolveWeather(observedWeather,localConditions(),forcedWeather,
            inspectAt?std::optional<std::array<double,3>>(inspectWeather):std::nullopt);
        if(inspectTemperature){sample.temperature=*inspectTemperature;sample.temperatureKnown=true;}
        return sample;
    }
    bool fogFar=false;  // what the fog was last told about the horizon
    static double number(const json& j,const char* key,double fallback) {
        auto it=j.find(key);return it!=j.end()&&it->is_number()?it->get<double>():fallback;
    }
    void readConditions() {
        auto latest=sky->latest();
        if(!latest||latest==conditionsDoc)return;
        conditionsDoc=latest;conditions=*latest;
        const auto w=conditions.value("weather",json());
        observedWeather=Weather{};
        if(w.is_object()) {
            observedWeather.known=true;
            observedWeather.temperature=number(w,"temperature",0.);
            observedWeather.temperatureKnown=w.contains("temperature")&&w["temperature"].is_number()&&
                std::isfinite(observedWeather.temperature);
            observedWeather.cover=std::clamp(number(w,"cloudCover",0.)/100.,0.,1.);
            observedWeather.rain=r1::liquidRainMmPerHour(w);
            observedWeather.visibility=number(w,"visibility",0.);
            observedWeather.windSpeed=std::max(0.,number(w,"windSpeed",0.));
            observedWeather.windFrom=number(w,"windFrom",0.);
            observedWeather.snowfall=std::max(0.,number(w,"snowfall",0.));
            observedWeather.snowDepth=std::max(0.,number(w,"snowDepth",0.));
            observedWeather.code=int(number(w,"code",-1.));
        }
        applyWeather();
    }
    // The measured visibility is the fog (sun_cycle.js). From 24 km up it is
    // clear air: the weather models stop there (24.14 km, fifteen miles), and
    // the photographed horizons of clear days read 150 km and more, so a
    // reading at the models' ceiling is the clear default, not 24 km of haze.
    static constexpr double kVisibilityCeiling=24000.;
    bool applyWeather() {
        if(!sunScript)return false;
        fogFar=farPack.node!=nullptr;
        const auto weather=effectiveWeather();
        double seen=weather.visibility>0&&(inspectAt||weather.visibility<kVisibilityCeiling)?weather.visibility:0.;
        if(toasterMode) {
            constexpr double kToasterVisibility=650.;
            seen=seen>0.?std::min(seen,kToasterVisibility):kToasterVisibility;
        }
        const double rain=weather.rain;
        const double cloud=r1::rainCloudCover(weather.cover,weather.code,rain);
        json result;
        // From the menu, before any tile is resident, the ground is not known
        // yet: the observer's altitude stands in, and tellSun() sends the real
        // ground at the spawn. Asking `height` there threw "Missing terrain".
        const double ground=tile(lon,lat)?height(lon,lat):alt;
        const bool accepted=sunScript->callExport("setWeather",json::array({cloud,rain,seen,ground}),result)
            ==saida::ScriptCallStatus::Succeeded&&result.is_boolean()&&result.get<bool>();
        if(inspectAt&&accepted)saida::Log::info("[World inspection] cloud=",cloud," rain_mm_h=",rain," visibility_m=",seen);
        return accepted;
    }

    saida::ParticleSystemNode* rainfall=nullptr;
    r1::RainState rainState;
    double rainShelterAge=1e9;
    glm::vec3 rainShelterEye{0.f};
    bool rainSheltered=false;
    glm::vec3 weatherWind() const {
        const auto weather=effectiveWeather();
        const double to=(weather.windFrom+180.)*rad;
        const glm::dvec3 local(std::sin(to)*weather.windSpeed,0.,-std::cos(to)*weather.windSpeed);
        // Bearings belong to the observer's tangent frame, including at poles;
        // render coordinates belong to the last floating origin instead.
        return glm::vec3(glm::transpose(origin.basis)*Frame(lon,lat,alt).basis*local);
    }
    void clearRain() {
        if(rainfall)rainfall->setEnabled(false);
        rainState.reset();rainShelterAge=1e9;rainSheltered=false;
        auto& settings=engine.sceneTree().world().settings();
        settings.rainIntensity=0.f;settings.wetness=0.f;settings.stormCloudCover=0.f;
    }
    void updateRain(double dt) {
        const auto weather=effectiveWeather();
        const bool active=playing&&!pending&&weather.known;
        const double rate=weather.rain;
        rainState.update(dt,rate,active);
        auto& settings=engine.sceneTree().world().settings();
        settings.rainIntensity=float(rainState.intensity);
        settings.wetness=float(rainState.wetness);
        const double storm=active&&rate>0.?r1::rainCloudCover(weather.cover,weather.code,rate):0.;
        settings.stormCloudCover+=float((storm-settings.stormCloudCover)*(1.-std::exp(-std::max(0.,dt)/2.)));
        if(storm==0.&&settings.stormCloudCover<.0001f)settings.stormCloudCover=0.f;
        if(!active){clearRain();return;}
        const glm::vec3 up=glm::normalize(glm::vec3(glm::transpose(origin.basis)*
            (ecef(lon,lat,alt+1.)-ecef(lon,lat,alt))));
        settings.rainUpDirection=up;
        const glm::vec3 eye=camera->transform().position;
        rainShelterAge+=dt;
        if(rate>0.&&(rainShelterAge>.15||glm::length(eye-rainShelterEye)>1.f)) {
            rainShelterAge=0.;rainShelterEye=eye;
            auto* physics=engine.sceneTree().world().physics();
            rainSheltered=physics&&physics->raycast(eye+up*.1f,up,120.f,obstacleFilter()).hit;
        }
        if(rate<=0.||rainSheltered||menu) {
            if(rainfall)rainfall->setEnabled(false);
            return;
        }
        if(daylightAge>.5&&sunScript) {
            json result;daylightAge=0.;
            if(sunScript->callExport("daylight",json::array(),result)==saida::ScriptCallStatus::Succeeded&&result.is_number())
                daylightNow=std::clamp(result.get<double>(),0.,1.);
        }
        if(!rainfall) {
            auto node=std::make_unique<saida::ParticleSystemNode>();node->setName("rainfall");
            node->effectClass=saida::ParticleSystemNode::EffectClass::Rain;node->applyEffectPreset();
            rainfall=static_cast<saida::ParticleSystemNode*>(engine.sceneTree().world().addChild(std::move(node)));
            saida::Log::info("[World weather] liquid rain, ",rate," mm/h");
        }
        const glm::vec3 wind=weatherWind();
        const glm::vec3 drift=wind*.55f;
        rainfall->setEnabled(true);
        rainfall->maxParticles=1800;rainfall->lifetime=1.1f;
        rainfall->shape=saida::ParticleSystemNode::Shape::Box;rainfall->boxExtents={14.f,2.f,14.f};
        rainfall->radius=14.f;rainfall->spawnRate=float(std::clamp(90.+rate*140.,90.,1500.));
        rainfall->startSize=.035f;rainfall->stretch=22.f;rainfall->endSizeScale=1.f;
        rainfall->startColor={.75f,.81f,.88f,.38f};rainfall->endColor={.75f,.81f,.88f,.08f};
        rainfall->emissive=float(.035+.7*daylightNow);rainfall->drag=0.f;
        rainfall->gravity=-up*1.5f;
        rainfall->useInitialVelocity=true;rainfall->initialVelocity=-up*12.f+drift;
        rainfall->transform().position=eye+up*9.f-drift*.65f;
    }
    // Streets and buildings that arrive after the player (a provisional
    // tile cooked again) can land on him: he is moved to the nearest free
    // ground, and told why.
    void keepStanding() {
        checkStanding=false;
        if(!playing||pending||driving||sailing||piloting||swimming||!tile(lon,lat))return;
        // The finer survey (IGN) may have replaced the quick one under him.
        // A breakwater the feet stand on is not in the grid: keep what holds him.
        if(jumpOffset<=0) {
            const double ground=height(lon,lat,alt);
            alt=feet->isOnFloor()&&alt>ground?alt:ground;
        }
        if(!footBlocked(lon,lat,alt))return;
        double x,y;
        if(!freeSpot(lon,lat,40.,x,y))return;
        saida::Log::info("[World] a building arrived where the player stood: moved to ",x,", ",y);
        lon=x;lat=y;alt=height(lon,lat);jumpOffset=jumpVelocity=0;
        text("stream-status","The buildings have arrived: here you are in the street.");
    }
    // ── snow in the air ─────────────────────────────────────────────────────
    //
    // Falling snow when the forecast says it snows, drifting snow when the
    // wind lifts what lies on the ground (on the pack, or wherever the model
    // says snow lies), both carried by the measured wind. They are drawn
    // around the camera and nowhere else, and dimmed with the daylight: a
    // particle is not lit by the scene.
    saida::ParticleSystemNode* snowfall=nullptr;
    saida::ParticleSystemNode* drift=nullptr;
    double daylightNow=1,daylightAge=1e9;
    // Snow on the ground: when the model says it lies (3 cm or more, Open-
    // Meteo's snow depth for the place), the ground and the roofs of every
    // resident tile wear the photographed snow, and take their own surfaces
    // back when it melts. The streets are left clear -- they are ploughed --
    // and so are the ground classes that are already snow, ice or water.
    saida::Material* snowMaterial(bool doubleSided) {
        const r1::Swatch& s=r1::palette().snow;
        return material(r1::surfaceMaterial(s.name,s.color,s.roughness,std::string("snow"),doubleSided));
    }
    static bool snowable(const std::string& name) {
        const bool ground=name.rfind("Ground \xE2\x80\x94 ",0)==0,roof=name.rfind("Roofs \xE2\x80\x94 ",0)==0;
        if(!ground&&!roof)return false;
        for(const char* keep:{"Water","Snow","Glacier","Frosted","sea ice","Melt pond","Young grey"})
            if(name.find(keep)!=std::string::npos)return false;
        return true;
    }
    void updateSnowCover() {
        const auto weather=effectiveWeather();
        const bool want=weather.known&&weather.snowDepth>=.03;
        for(auto& [key,t]:loaded) {
            if(!t.geography)continue;
            if(t.snowed&&!want) {
                for(auto& [node,own]:t.bare)node->setMaterial(own);
                t.bare.clear();t.snowed=false;t.snowSeen=0;
                continue;
            }
            if(!want)continue;
            if(!t.snowed){t.snowed=true;t.snowSeen=0;}
            const auto& children=t.geography->children();
            for(;t.snowSeen<children.size();++t.snowSeen) {
                auto* mesh=dynamic_cast<saida::MeshNode*>(children[t.snowSeen].get());
                if(!mesh||!snowable(mesh->name())||!mesh->material())continue;
                t.bare.push_back({mesh,mesh->material()});
                mesh->setMaterial(snowMaterial(mesh->material()->desc().doubleSided));
            }
        }
    }
    saida::ParticleSystemNode* emitter(const char* name) {
        auto n=std::make_unique<saida::ParticleSystemNode>();
        n->setName(name);
        n->effectClass=saida::ParticleSystemNode::EffectClass::Snow;
        n->applyEffectPreset();
        return static_cast<saida::ParticleSystemNode*>(engine.sceneTree().world().addChild(std::move(n)));
    }
    void updateSnow(double dt) {
        const auto weather=effectiveWeather();
        const bool here=playing&&!pending&&weather.known;
        const bool falling=here&&((weather.code>=71&&weather.code<=77)||weather.code==85||weather.code==86||weather.snowfall>.02);
        const bool lying=onSeaIce(lon,lat)||weather.snowDepth>.05;
        const bool blowing=here&&lying&&weather.windSpeed>6.;
        daylightAge+=dt;
        if((falling||blowing)&&daylightAge>.5&&sunScript) {
            json result;daylightAge=0;
            if(sunScript->callExport("daylight",json::array(),result)==saida::ScriptCallStatus::Succeeded&&result.is_number())
                daylightNow=std::clamp(result.get<double>(),0.,1.);
        }
        // The wind blows towards where it does not come from; x east, z south.
        const glm::vec3 wind=weatherWind();
        const glm::vec3 eye=camera->transform().position;
        const float light=float(.04+.9*daylightNow);
        if(falling) {
            if(!snowfall){snowfall=emitter("snowfall");saida::Log::info("[World weather] snow falling, ",weather.snowfall," cm/h");}
            snowfall->setEnabled(true);
            snowfall->maxParticles=2400;snowfall->lifetime=9.f;snowfall->radius=22.f;
            snowfall->spawnRate=float(std::clamp(160.+420.*weather.snowfall,160.,700.));
            snowfall->drag=.35f;
            // Terminal drift = acceleration / drag: the flakes move with the wind.
            snowfall->gravity=wind*snowfall->drag+glm::vec3(0,-.45f,0);
            snowfall->emissive=light;
            snowfall->transform().position=eye+glm::vec3(0,9.f,0)-wind*4.f;
        } else if(snowfall)snowfall->setEnabled(false);
        if(blowing) {
            if(!drift){drift=emitter("drifting snow");saida::Log::info("[World weather] snow drifting, wind ",weather.windSpeed," m/s");}
            drift->setEnabled(true);
            drift->maxParticles=1600;drift->lifetime=1.8f;drift->radius=14.f;
            drift->spawnRate=float(std::clamp((weather.windSpeed-6.)*140.,40.,900.));
            drift->startSize=.035f;drift->startSpeed=.3f;drift->stretch=3.f;drift->drag=1.5f;
            drift->startColor=glm::vec4(.95f,.97f,1.f,.45f);drift->endColor=glm::vec4(.95f,.97f,1.f,0.f);
            drift->gravity=wind*drift->drag+glm::vec3(0,-.04f,0);
            drift->emissive=light;
            const glm::vec3 feet=player->transform().position;
            drift->transform().position=glm::vec3(eye.x,feet.y+.3f,eye.z)-wind*.8f;
        } else if(drift)drift->setEnabled(false);
    }
    // The offset the HUD clock reads: the place's zone when Open-Meteo gave
    // one, otherwise the longitude's whole hours, labelled as an estimate.
    int utcOffset(bool& known) {
        known=localConditions()&&conditions.value("timeSource","")=="zone";
        return known?conditions.value("utcOffsetSeconds",0):int(std::round(lon/15.))*3600;
    }
    std::string localClock() {
        double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
        bool known=false;const int offset=utcOffset(known);
        const std::time_t local=std::time_t(unixSeconds)+offset;
        std::tm parts{};gmtime_s(&parts,&local);
        std::ostringstream out;out<<std::put_time(&parts,"%H:%M");
        return out.str()+(known?" · local time":" · estimated time zone")+(forcedMinutes?" (forced)":"");
    }
    // "Forcer l'heure à" (Options). The hour is the one the HUD clock shows,
    // so the instant depends on the zone under the player: it is recomputed
    // every frame, and the Sun is told only when it moves -- a new zone, a
    // new local day, or the option itself. With no forced hour the script
    // follows the real clock, which is its own default.
    void applyForcedTime() {
        std::optional<double> wanted;
        if(forcedMinutes) {
            bool known=false;const int offset=utcOffset(known);
            wanted=r1::forcedInstant(double(std::time(nullptr)),offset,*forcedMinutes);
        }
        if(wanted==forcedSent||!findSun())return;
        json result;
        const bool accepted=sunScript->callExport("setForcedTime",json::array({wanted?json(*wanted):json(nullptr)}),result)
                ==saida::ScriptCallStatus::Succeeded&&result.is_boolean()&&result.get<bool>();
        if(!accepted) {
            if(!forcedSaid) {
                saida::Log::error("[World options] scripts/sun_cycle.js refused setForcedTime(",
                                  wanted?number(*wanted,0):std::string("null"),")");
                sayForcedTime("The Sun refused the forced time: see game.log.");
                forcedSaid=true;
            }
            return;
        }
        forcedSent=wanted;forcedSaid=false;
    }
    fs::path optionsFile() const {return game/"cache/options.json";}
    // A test run neither reads nor rewrites the player's options: the smoke
    // test and the gallery light the world by the clock or by --at.
    bool testRun() const {return smoke||!worldCapture.pngPath.empty();}
    void loadOptions() {
        std::ifstream input(optionsFile());
        if(!input)return;
        if(testRun()){saida::Log::info("[World options] cache/options.json ignored by a test run");return;}
        try {
            const json options=json::parse(input);
            if(!options.is_object())throw std::runtime_error("options must be an object");
            if(const auto hour=options.find("forcedTime");hour!=options.end()) {
                if(!hour->is_string())saida::Log::error("[World options] cache/options.json: forcedTime must be a string, ignored");
                else {
                    const auto typed=hour->get<std::string>();
                    if(!typed.empty()) {
                        forcedMinutes=r1::parseClockTime(typed);
                        if(forcedMinutes)saida::Log::info("[World options] time forced to ",r1::formatClockTime(*forcedMinutes)," local");
                        else saida::Log::error("[World options] cache/options.json: forcedTime '",typed,"' is not HH:MM, ignored");
                    }
                }
            }
            if(const auto atmosphere=options.find("forcedWeather");atmosphere!=options.end()) {
                const auto mode=atmosphere->is_string()?r1::parseWeatherOverride(atmosphere->get<std::string>()):std::nullopt;
                if(mode) {
                    forcedWeather=*mode;
                    saida::Log::info("[World options] weather mode ",r1::weatherOverrideKey(forcedWeather)," everywhere");
                } else saida::Log::error("[World options] cache/options.json: invalid forcedWeather, ignored");
            }
            if(const auto toaster=options.find("toasterMode");toaster!=options.end()) {
                if(toaster->is_boolean()) {
                    toasterMode=toaster->get<bool>();
                    saida::Log::info("[World options] toasterMode ",toasterMode?"enabled":"disabled");
                } else saida::Log::error("[World options] cache/options.json: invalid toasterMode, ignored");
            }
        } catch(const std::exception& e) {
            saida::Log::error("[World options] cache/options.json unreadable, ignored: ",e.what());
        }
    }
    void saveOptions() {
        if(testRun())return;
        json options=json::object();
        // Updating either option must preserve the other and any future keys.
        std::ifstream input(optionsFile());
        if(input) {
            try {
                auto saved=json::parse(input);
                if(saved.is_object())options=std::move(saved);
                else saida::Log::warn("[World options] replacing invalid non-object options");
            } catch(const std::exception& e) {
                saida::Log::warn("[World options] replacing unreadable options: ",e.what());
            }
            input.close();
        }
        if(forcedMinutes)options["forcedTime"]=r1::formatClockTime(*forcedMinutes);
        else options.erase("forcedTime");
        options["forcedWeather"]=r1::weatherOverrideKey(forcedWeather);
        options["toasterMode"]=toasterMode;
        std::error_code ignored;fs::create_directories(optionsFile().parent_path(),ignored);
        std::ofstream output(optionsFile());
        output<<options.dump(2)<<"\n";
        output.flush();
        if(!output) {
            saida::Log::error("[World options] could not write ",optionsFile().string());
            sayForcedTime("Option applied, but it could not be saved: see game.log.");
            sayForcedWeather("Option applied, but it could not be saved: see game.log.");
            sayToasterMode("Option applied, but it could not be saved: see game.log.");
        }
    }
    void sayForcedTime(const std::string& problem={}) {
        if(!problem.empty()){text("options-status",problem);return;}
        text("options-status",forcedMinutes
            ?"Everywhere in the world it is "+r1::formatClockTime(*forcedMinutes)+" local time. Time no longer passes."
            :"Real time: every place is at its current hour.");
    }
    void sayForcedWeather(const std::string& problem={}) {
        for(const auto mode:{r1::WeatherOverride::Real,r1::WeatherOverride::Clear,
                             r1::WeatherOverride::LightRain,r1::WeatherOverride::Storm}) {
            const std::string id=std::string("weather-")+r1::weatherOverrideKey(mode);
            if(auto* button=ui->findElementById(id))button->SetClass("selected",mode==forcedWeather);
        }
        ui->notifyJsMutation();
        text("weather-options-status",!problem.empty()?problem:
            forcedWeather==r1::WeatherOverride::Real?"Real weather: every place follows its local forecast.":
            std::string("Everywhere in the world: ")+r1::weatherOverrideLabel(forcedWeather)+". This choice stays after you travel.");
    }
    void sayToasterMode(const std::string& problem={}) {
        if(auto* off=ui->findElementById("toaster-off"))off->SetClass("selected",!toasterMode);
        if(auto* on=ui->findElementById("toaster-on"))on->SetClass("selected",toasterMode);
        ui->notifyJsMutation();
        text("toaster-options-status",!problem.empty()?problem:
            toasterMode?"Toaster mode on: 720p, mid/far tree LODs, closer fog."
                       :"Standard quality: normal graphics and draw distance.");
    }
    void applyToasterResolution() {
        if(toasterMode) {
            int w=0,h=0;
            engine.window().size(w,h);
            if(w>0&&h>0&&(w!=1280||h!=720)){standardWidth=w;standardHeight=h;}
            engine.window().setSize(1280,720);
        } else {
            int targetW=standardWidth>0?standardWidth:1600;
            int targetH=standardHeight>0?standardHeight:900;
            if(targetW==1280&&targetH==720){targetW=1600;targetH=900;}
            engine.window().setSize(targetW,targetH);
        }
    }
    void applyToasterLods() {
        for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles){
            for(Plant& plant:t.vegetation){
                if(plant.lod)plant.lod->setFinestLevel(toasterMode?1:(plant.close?0:1));
            }
        }
    }
    void setToasterMode(bool enabled) {
        toasterMode=enabled;
        saida::Log::info("[World options] toaster mode ",toasterMode?"enabled":"disabled");
        applyToasterResolution();
        applyToasterLods();
        applyWeather();
        sayToasterMode();
        saveOptions();
    }
    void forceWeather(r1::WeatherOverride mode) {
        forcedWeather=mode;
        saida::Log::info("[World options] weather mode ",r1::weatherOverrideKey(mode)," everywhere");
        sayForcedWeather();
        saveOptions();
        if(findSun()&&!applyWeather()) {
            saida::Log::error("[World options] scripts/sun_cycle.js refused forced weather");
            sayForcedWeather("The sky refused the chosen weather: see game.log.");
        }
        updateSnowCover();
        updateSnow(0.);
        updateRain(0.);
        text("local-conditions",weatherLabel());
    }
    void showOptions(bool show) {
        optionsOpen=show;
        globeDragging=false;
        style("options-screen","display",show?"block":"none");
        style("menu","display",show?"none":"block");
        if(auto* e=ui->findElementById(show?"options-back":"options"))e->Focus();
        if(!show)return;
        field("forced-time",forcedMinutes?r1::formatClockTime(*forcedMinutes):std::string());
        sayForcedTime();
        sayForcedWeather();
        sayToasterMode();
    }
    void forceTime(std::optional<int> minutes) {
        forcedMinutes=minutes;forcedSaid=false;
        saida::Log::info("[World options] ",minutes?"time forced to "+r1::formatClockTime(*minutes)+" local":std::string("real time"));
        sayForcedTime();
        saveOptions();
        applyForcedTime();
    }
    void applyTypedTime() {
        const std::string typed=value("forced-time");
        if(const auto minutes=r1::parseClockTime(typed)){forceTime(minutes);field("forced-time",r1::formatClockTime(*minutes));return;}
        saida::Log::info("[World options] refused forced time '",typed,"': not HH:MM between 00:00 and 23:59");
        sayForcedTime("Invalid time \""+typed+"\": type HH:MM, from 00:00 to 23:59 (for example 14:00).");
    }
    std::string weatherLabel() {
        if(forcedWeather!=r1::WeatherOverride::Real&&!inspectAt)
            return std::string("forced weather: ")+r1::weatherOverrideLabel(forcedWeather)+" · everywhere";
        if(!localConditions())return "Local weather unavailable";
        const auto weather=conditions.value("weather",json());
        if(!weather.is_object())return "Local weather unavailable";
        const int code=weather.value("code",-1);
        const char* state=code==0?"clear":code<=3?"cloudy":code<=48?"fog"
                          :code<=67?"rain":code<=77?"snow":code<=82?"showers":"storm";
        return std::string("forecast: ")+state+" · "+number(weather.value("temperature",0.),0)+" °C"
             +" · "+number(weather.value("cloudCover",0.),0)+" % cloud";
    }
    double metresFrom(double x,double y) const {
        const glm::dvec3 d=origin.local(ecef(x,y,0.))-origin.local(ecef(lon,lat,0.));
        return std::hypot(d.x,d.z);
    }
    saida::Node* hullPrototype(const std::string& model,const json& doc) {
        auto& p=hullPrototypes[model];
        if(!p&&doc.is_object()) {
            try {p=prototypes->addChild(plantNode(doc));}
            catch(const std::exception& e){saida::Log::warn("[World sea] hull ",model," failed: ",e.what());}
        }
        return p;
    }
    void readSea() {
        auto latest=sea->latest();
        if(!latest||latest==seaDoc)return;
        seaDoc=latest;
        const json& doc=*latest;
        const json source=doc.value("source",json::object());
        const json hulls=doc.value("hulls",json::object());
        std::set<std::string> have;
        for(auto& s:seaShips)have.insert(s.id);
        size_t added=0;
        for(const auto& s:doc.value("ships",json::array())) {
            if(seaShips.size()>=kSeaShips)break;
            const std::string id=s.value("id","");
            if(id.empty()||have.count(id)||seaTaken.count(id))continue;
            const double x=s.value("lon",0.),y=s.value("lat",0.);
            if(!seaFirst&&metresFrom(x,y)<kSeaAppear)continue;
            if(tile(x,y)&&!navigable(x,y))continue;       // the cell's land, where the map knows it
            const std::string model=s.value("model","");
            auto* proto=hullPrototype(model,hulls.value(model,json()));
            if(!proto)continue;
            SeaShip ship;ship.id=id;
            ship.phase=double(std::hash<std::string>{}(id)%1000)*.0063;
            Vessel& v=ship.v;
            v.node=engine.sceneTree().world().addChild(clonePlant(*proto));
            v.kind=s.value("kind","commercial");v.length=s.value("length",20.);v.beam=s.value("beam",5.);
            v.top=s.value("top",8.);v.accel=s.value("accel",.5);v.turn=s.value("turn",10.);
            v.lon=x;v.lat=y;v.yaw=wrap(s.value("heading",0.));v.speed=s.value("speed",0.);
            v.alt=tile(x,y)?waterLevel(x,y):0.;
            if(std::getenv("R1WORLD_SEA_DEBUG")) {
                const glm::dvec3 at=origin.local(ecef(v.lon,v.lat,v.alt));
                saida::Log::info("[World sea] ship ",id," ",model," at ",at.x,",",at.y,",",at.z," alt ",v.alt);
            }
            seaShips.push_back(std::move(ship));++added;
        }
        seaFirst=false;
        const bool online=source.value("online",false);
        const std::string weather=source.value("weather",std::string("?"));
        const std::string synced=source.contains("synced")&&source["synced"].is_string()
            ?source["synced"].get<std::string>():std::string("never");
        const std::string said=weather+(online?" online":" offline");
        if(said!=seaSaid||added) {
            saida::Log::info("[World sea] ",seaShips.size()," ships (+",added,"), weather ",weather,", ",
                             online?"online":"offline",", last synced ",synced,
                             ", live ",source.value("live",0));
            seaSaid=said;
        }
    }
    void updateSea(double dt) {
        seaAsk+=dt;seaRead+=dt;
        if(seaAsk>5.) {
            seaAsk=0;double t=0;
            if(gameTime(t)){sea->ask(lon,lat,t);sky->ask(lon,lat);}
        }
        if(seaRead>1.){seaRead=0;readSea();}
        for(size_t i=0;i<seaShips.size();) {
            Vessel& v=seaShips[i].v;
            if(v.speed>0) {
                // Look a hull length and a half ahead; where the map says land,
                // come round to the first open bearing, stopping if there is none.
                auto clear=[&](double heading){
                    const double s=std::sin(heading*rad),c=std::cos(heading*rad);
                    const double lead=v.length*.75+v.speed*8.;
                    auto bow=advance(v.lon,v.lat,s*lead,c*lead);
                    return !tile(bow.x,bow.y)||navigable(bow.x,bow.y);
                };
                if(!clear(v.yaw)) {
                    double turn=0;
                    for(double d:{25.,-25.,50.,-50.,90.,-90.,135.,-135.,180.})
                        if(clear(v.yaw+d)){turn=d;break;}
                    if(turn==0)v.speed=0;
                    else v.yaw=wrap(v.yaw+std::clamp(turn,-v.turn*dt,v.turn*dt));
                }
                const double s=std::sin(v.yaw*rad),c=std::cos(v.yaw*rad);
                const auto next=advance(v.lon,v.lat,s*v.speed*dt,c*v.speed*dt);
                if(tile(next.x,next.y)&&!navigable(next.x,next.y))v.speed=0;
                else {
                    v.lon=next.x;v.lat=next.y;
                    v.alt=tile(v.lon,v.lat)?waterLevel(v.lon,v.lat):0.;
                }
            }
            if(metresFrom(v.lon,v.lat)>kSeaKeep) {
                if(v.node)v.node->queueFree();
                seaShips.erase(seaShips.begin()+long(i));
                continue;
            }
            ++i;
        }
    }

    // ── traffic ─────────────────────────────────────────────────────────────
    //
    // The simulation is `engine/plugins/traffic`, which knows nothing about
    // this project: no geodesy, no tiles, no scene. Everything below is the
    // half that only R1World can do — read the lane graph the generator cooked,
    // give each tile its own flow, put a scene node on each agent, and hand
    // the player's car back as an obstacle.
    //
    // Cars are tile-local by construction. A flow drives in its tile's tangent
    // frame and its nodes are children of the tile's node, so the floating
    // origin moves them for free (placeTiles) and evicting the tile evicts its
    // traffic without a single pointer to update.
    void loadPaints() {
        // Albedos, from the Atlas (assets/world/atlas.json), where a test holds
        // them to rule 2 -- a colour the tests cannot see is a rule not held.
        for(const auto& albedo:r1::palette().carPaints)paints.push_back(glm::vec3(albedo[0],albedo[1],albedo[2]));
        if(paints.empty()){
            saida::Log::warn("[World traffic] the Atlas lists no car paint; the fleet will be one colour");
            paints.push_back(glm::vec3(.30f,.30f,.31f));
        }
    }
    // Disabled scene ownership keeps shared prototype resources resident.
    const VehicleModel& vehicleSpec(const saida::Node& node) const {
        for(const auto& spec:fleet)if(node.name()=="vehicle-"+spec.name)return spec;
        return fleet.front(); // The entry scene starts with the city car.
    }
    static void vehicleLod(saida::Node& node) {
        auto* body=r1::VehicleRuntime::body(node);
        auto& visual=body?static_cast<saida::Node&>(*body):node;
        auto* lod=visual.getBehaviour<saida::LODGroupBehaviour>();
        if(!lod)lod=visual.addBehaviour<saida::LODGroupBehaviour>();
        // Full source detail is for close cars. Switch by projected size,
        // keeping long vehicles detailed farther away, with engine hysteresis.
        const bool bike=node.name()=="vehicle-moto_city"||node.name()=="vehicle-moto_cross";
        lod->setLevels({{"Near",bike?.045f:.16f},{"Far",0.f}});
    }
    static uint32_t motorcycleMix(uint32_t x) {
        x^=x>>16;x*=0x7feb352du;x^=x>>15;x*=0x846ca68bu;x^=x>>16;return x;
    }
    void buildMotorcycleHelmetGeometry() {
        saida::MeshData shell,visor;
        constexpr int rings=8,segments=16;
        for(int ring=0;ring<=rings;++ring) {
            const float theta=float(ring)*float(kTwoPi*.25/rings);
            for(int segment=0;segment<=segments;++segment) {
                const float phi=float(segment)*float(kTwoPi/segments);
                const float x=.145f*std::sin(theta)*std::cos(phi);
                const float z=.165f*std::sin(theta)*std::sin(phi);
                const float y=.015f+.185f*std::cos(theta);
                glm::vec3 normal(x/(.145f*.145f),(y-.015f)/(.185f*.185f),z/(.165f*.165f));
                saida::Vertex v{};v.pos={x,y,z};v.normal=glm::normalize(normal);v.color=glm::vec3(1.f);
                shell.vertices.push_back(v);
            }
        }
        for(int ring=0;ring<rings;++ring)for(int segment=0;segment<segments;++segment) {
            const uint32_t a=uint32_t(ring*(segments+1)+segment),b=a+1;
            const uint32_t c=a+uint32_t(segments+1),d=c+1;
            shell.indices.insert(shell.indices.end(),{a,c,b,b,c,d});
        }
        constexpr int visorSegments=8;
        for(int x=0;x<=visorSegments;++x) {
            const float u=-1.f+2.f*float(x)/visorSegments;
            const float z=.142f+.014f*(1.f-u*u);
            for(float y:{.092f,.005f}) {
                saida::Vertex v{};v.pos={u*.112f,y,z};v.normal={0,0,1};v.color=glm::vec3(1.f);
                visor.vertices.push_back(v);
            }
        }
        for(int x=0;x<visorSegments;++x) {
            const uint32_t a=uint32_t(x*2);visor.indices.insert(visor.indices.end(),{a,a+2,a+1,a+1,a+2,a+3});
        }
        motorcycleHelmetShell=std::make_unique<saida::Mesh>(engine.resources().geometry(),shell.vertices,shell.indices);
        motorcycleHelmetVisor=std::make_unique<saida::Mesh>(engine.resources().geometry(),visor.vertices,visor.indices);
    }
    void addMotorcycleHelmet(saida::Node& rider,uint32_t seed) {
        if(!motorcycleHelmetShell||!motorcycleHelmetVisor)return;
        static constexpr std::array<glm::vec4,8> colors{{
            {.055f,.065f,.075f,1.f},{.82f,.84f,.82f,1.f},{.68f,.055f,.035f,1.f},
            {.045f,.12f,.30f,1.f},{.92f,.48f,.025f,1.f},{.12f,.22f,.11f,1.f},
            {.85f,.78f,.62f,1.f},{.20f,.20f,.22f,1.f}
        }};
        saida::MaterialDesc shellDesc;shellDesc.baseColor=colors[motorcycleMix(seed)%colors.size()];
        shellDesc.roughness=.32f;shellDesc.metallic=.04f;shellDesc.doubleSided=true;
        saida::MaterialDesc visorDesc;visorDesc.baseColor={.025f,.035f,.045f,1.f};
        visorDesc.roughness=.19f;visorDesc.metallic=.22f;visorDesc.doubleSided=true;
        auto helmet=std::make_unique<saida::Node>("Motorcycle helmet");
        saida::Transform offset;offset.position={0.f,.015f,0.f};
        helmet->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 Head",offset);
        auto nearLevel=std::make_unique<saida::Node>("Near");
        nearLevel->addChild(std::make_unique<saida::MeshNode>("Helmet shell",motorcycleHelmetShell.get(),
                                                         engine.resources().getMaterial(shellDesc)));
        nearLevel->addChild(std::make_unique<saida::MeshNode>("Helmet visor",motorcycleHelmetVisor.get(),
                                                         engine.resources().getMaterial(visorDesc)));
        auto farLevel=std::make_unique<saida::Node>("Far");
        farLevel->addChild(std::make_unique<saida::MeshNode>("Helmet shell",motorcycleHelmetShell.get(),
                                                        engine.resources().getMaterial(shellDesc)));
        helmet->addChild(std::move(nearLevel));helmet->addChild(std::move(farLevel));
        helmet->addBehaviour<saida::LODGroupBehaviour>()->setLevels({{"Near",.045f},{"Far",0.f}});
        rider.addChild(std::move(helmet));
    }
    std::unique_ptr<saida::Node> makeMotorcycleRider(const std::string& vehicle,HumanKind& kind,
                                                      uint32_t seed,bool helmet) {
        const bool motocross=vehicle=="moto_cross";
        auto rider=std::make_unique<saida::Node>("Rider");
        rider->transform().rotation=glm::quat(0,0,1,0);
        rider->transform().position=r1::rocketboxRidingSeat(*kind.rig,motocross,
            float(kVehicleScale),float(humanScale),float(kind.seatDrop));
        rider->transform().scale=glm::vec3(float(humanScale));
        auto* animator=rider->addBehaviour<saida::Animator>();
        animator->setRig(kind.rig);
        for(const auto& [name,clip]:kind.clips)animator->addClip(name,clip);
        animator->play("sit",true,0.f);animator->setPoseRate(6.f,saida::Animator::PoseRateMode::Hold);
        r1::holdRocketboxRidingBase(*animator);
        animator->addModifier<saida::GazeModifier>(*kind.rig,r1::rocketboxRidingLean())->lookAt({0.f,-.4f,1.2f});
        animator->addModifier<saida::GazeModifier>(*kind.rig,r1::rocketboxRidingLook())->lookAt({0.f,1.5f,100.f});
        auto* left=animator->addModifier<saida::TwoBoneIKModifier>(*kind.rig,r1::rocketboxArm(false));
        auto* right=animator->addModifier<saida::TwoBoneIKModifier>(*kind.rig,r1::rocketboxArm(true));
        for(int h=0;h<2;++h) {
            auto* arm=h?right:left;
            const auto grip=motorcycleContact(*rider,vehicle,h,true);
            const auto palm=r1::rocketboxBarGrip(*kind.rig,h==1);
            arm->setTipRotation(palm.rotation);arm->reach(grip-palm.rotation*palm.position);
            arm->setWeightNow(1.f);arm->setPole({grip.x*1.7f,grip.y-.15f,grip.z-.25f});
            auto* leg=animator->addModifier<saida::TwoBoneIKModifier>(*kind.rig,
                saida::TwoBoneIKModifier::Settings{h?"Bip01 R Thigh":"Bip01 L Thigh",h?"Bip01 R Calf":"Bip01 L Calf",h?"Bip01 R Foot":"Bip01 L Foot",22.f});
            const auto foot=motorcycleContact(*rider,vehicle,h,false);
            leg->reach(foot);leg->setWeightNow(1.f);leg->setPole({foot.x*2.f,foot.y+.35f,foot.z+.45f});
            leg->setTipRotation(r1::rocketboxFootRotation(*kind.rig,h==1));
        }
        animator->addModifier<r1::RocketboxRidingHands>(*kind.rig)->hold(true);
        for(const char* level:{"Near","Far"}) {
            auto holder=std::make_unique<saida::Node>(level);
            auto found=std::find_if(kind.levels.begin(),kind.levels.end(),
                [&](const auto& l){return l.first==level;});
            if(found!=kind.levels.end())for(const auto& meshSource:found->second) {
                auto mesh=std::make_unique<saida::MeshNode>(meshSource->name(),meshSource->mesh(),meshSource->material());
                mesh->transform()=meshSource->transform();holder->addChild(std::move(mesh));
            }
            rider->addChild(std::move(holder));
        }
        rider->addBehaviour<saida::LODGroupBehaviour>()->setLevels({{"Near",.045f},{"Far",0.f}});
        if(helmet)addMotorcycleHelmet(*rider,seed);
        return rider;
    }
    void customizeMotorcycleRider(saida::Node& vehicle,uint32_t seed,double latitude) {
        const std::string name=vehicleSpec(vehicle).name;
        if(!r1::motorcycle(name)||motorcycleRiderKinds.empty())return;
        const auto& kinds=(!motorcycleFemaleRiderKinds.empty()&&r1::femaleMotorcycleRider(seed))
            ?motorcycleFemaleRiderKinds:motorcycleRiderKinds;
        const size_t variant=kinds[motorcycleMix(seed)%kinds.size()];
        HumanKind* kind=humanKind(variant);
        if(!kind||!kind->rig)return;
        const bool helmet=motorcycleMix(seed^0x9e3779b9u)%100u<(latitude>=0.?80u:30u);
        if(auto* old=vehicle.findByPath("Rider"))vehicle.detachChild(old);
        vehicle.addChild(makeMotorcycleRider(name,*kind,seed,helmet));
    }
    void buildTrafficPrototype() {
        std::ifstream input(game/"assets/models/vehicles/fleet.json");
        if(!input)throw std::runtime_error("Missing road vehicle fleet manifest");
        json doc;input>>doc;
        // Stable 80/20 sex selection, independent of the number of available outfits.
        motorcycleRiderKinds.clear();motorcycleFemaleRiderKinds.clear();
        for(size_t i=0;i<crowdKinds.size();++i) {
            // Two female variants provide the requested distribution without
            // forcing the entire pedestrian wardrobe resident at game startup.
            if(crowdAppearances[i].sex=='f'&&motorcycleFemaleRiderKinds.size()>=2)continue;
            prototypeForStartup(crowdKinds[i].model);
            if(humanKind(i))(crowdAppearances[i].sex=='f'?motorcycleFemaleRiderKinds:motorcycleRiderKinds).push_back(i);
        }
        if(motorcycleRiderKinds.empty()) {
            prototypeForStartup(crowdKinds.front().model);
            if(humanKind(0))motorcycleRiderKinds.push_back(0);
        }
        buildMotorcycleHelmetGeometry();
        for(const auto& entry:doc.at("vehicles")) {
            VehicleModel spec;
            spec.name=entry.at("name");spec.length=entry.at("length");spec.width=entry.at("width");
            spec.height=entry.at("height");spec.wheelbase=entry.at("wheelbase");spec.wheelRadius=entry.at("wheelRadius");
            for(double* d:{&spec.length,&spec.width,&spec.height,&spec.wheelbase,&spec.wheelRadius})*d*=kVehicleScale;
            auto root=std::make_unique<saida::Node>("vehicle-"+spec.name);
            for(const auto& level:std::vector<std::pair<std::string,std::string>>{{"near","Near"},{"far","Far"}}) {
                auto* source=prototypeForStartup(entry.at(level.first).at("path").get<std::string>());
                if(!source)throw std::runtime_error("Vehicle asset failed: "+spec.name);
                auto child=clonePlant(*source);child->setName(level.second);
                child->transform().rotation=glm::quat(0,0,1,0); // Authored forward +Z.
                child->transform().scale=glm::vec3(float(kVehicleScale));
                root->addChild(std::move(child));
            }
            spec.prototype=prototypes->addChild(std::move(root));fleet.push_back(spec);
        }
        if(fleet.empty())throw std::runtime_error("The road vehicle fleet is empty");
        while(!car->children().empty())car->removeChild(car->children().front().get());
        for(const auto& child:fleet.front().prototype->children())car->addChild(clonePlant(*child));
        car->setName("vehicle-city");vehicleLod(*car);collectWheels(*car);
        saida::Log::info("[World traffic] fleet=",fleet.size()," shared vertices=",doc.at("totalVertices").get<size_t>());
    }
    size_t vehicleKind(uint32_t seed,const saida::traffic::Lane& lane,double motorcycleChance=.05) const {
        // Stable per-agent selection: 5% globally, 50% in Africa, India and
        // Latin America. The remaining share keeps the former car mix intact.
        const uint32_t raw=(seed^(seed>>16))%100;
        const uint32_t bikeCut=uint32_t(std::clamp(motorcycleChance,0.,1.)*100.);
        std::string name=raw<bikeCut?((raw&1u)?"moto_cross":"moto_city"):"";
        const uint32_t roll=raw>=bikeCut?uint32_t((raw-bikeCut)*100/std::max(1u,100-bikeCut)):0;
        if(name.empty())name=roll<34?"city":roll<55?"sedan":roll<74?"suv":roll<86?"offroad":roll<94?"sport":roll<98?"truck":"bus";
        if((name=="truck"||name=="bus")&&lane.speed<11.f)name="city";
        for(size_t i=0;i<fleet.size();++i)if(fleet[i].name==name)return i;
        return 0;
    }
    double motorcycleChance(double lon,double lat) const {
        const auto country=countryCrowd.at(lon,lat);
        const auto& region=country.subregion;
        const bool africa=region.find("Africa")!=std::string::npos;
        const bool latinAmerica=country.code=="MX"||region=="Central America"||region=="South America"||region=="Caribbean";
        return africa||country.code=="IN"||latinAmerica?.50:.05;
    }
    double trafficTime_=0;
    bool trafficValidation_=std::getenv("R1WORLD_TRAFFIC_VALIDATE")!=nullptr;
    double trafficValidationTime_=0,trafficValidationReport_=0;
    size_t trafficValidationMoving_=0,trafficValidationLive_=0;
    size_t trafficValidationSteadySamples_=0,trafficValidationSteadyMoving_=0;
    bool trafficValidationStuck_=false;
    double trafficValidationSeconds_=std::getenv("R1WORLD_TRAFFIC_SECONDS")?std::clamp(std::atof(std::getenv("R1WORLD_TRAFFIC_SECONDS")),60.,300.):60.;
    static constexpr double kTrafficValidationReportSeconds=5.;
    static constexpr double kTrafficValidationWarmupSeconds=30.;
    static constexpr size_t kTrafficValidationMinimumLive=20,kTrafficValidationMinimumMoving=10;
    void validateTraffic(double dt) {
        if(!playing||pending)return;
        trafficValidationTime_+=dt;trafficValidationReport_+=dt;
        if(trafficValidationReport_<kTrafficValidationReportSeconds)return;
        trafficValidationReport_=0;
        size_t live=0,moving=0,physical=0,grounded=0,incidents=0,requested=0,damaged=0;
        std::array<size_t,8> reasons{};size_t deadEnds=0;
        for(auto& [key,region]:loaded)for(size_t slot=0;slot<region.flow.agents().size();++slot) {
            const auto& agent=region.flow.agents()[slot];if(!agent.alive)continue;
            ++live;moving+=agent.speed>1.f;incidents+=agent.incident;requested+=agent.targetSpeed>1.f;
            if(agent.targetSpeed<.1f)++reasons[size_t(agent.stopReason)];
            deadEnds+=agent.nextLane==UINT32_MAX;
            if(agent.waiting>10.f) {
                const auto pose=region.flow.pose(agent);
                const auto& lane=region.graph.lanes[agent.lane];
                const auto observer=region.frame.local(ecef(lon,lat,alt));
                if(agent.external&&agent.waiting>60.f&&std::hypot(pose.position.x-observer.x,pose.position.y-observer.z)<60.)
                    trafficValidationStuck_=true;
                saida::Log::info("[World traffic waiter] tile=",key," id=",agent.identity," lane=",agent.lane," next=",agent.nextLane,
                    " s=",agent.s," remaining=",region.graph.laneLength(agent.lane)-agent.s," wait=",agent.waiting,
                    " speed=",agent.speed," target=",agent.targetSpeed," reason=",size_t(agent.stopReason)," physical=",agent.external,
                    " signal=",lane.signal," group=",unsigned(lane.signalGroup)," light=",lane.signal>=0?int(region.graph.signals[lane.signal].light(lane.signalGroup,trafficTime_)):-1,
                    " x=",pose.position.x," z=",pose.position.y," yaw=",pose.yaw);
            }
            if(slot<region.cars.size()&&region.cars[slot]&&r1::VehicleRuntime::physical(*region.cars[slot])) {
                ++physical;const auto* drive=r1::VehicleRuntime::driver(*region.cars[slot]);
                grounded+=drive->grounded();damaged+=drive->damage>.1f;
            }
        }
        trafficValidationLive_=std::max(trafficValidationLive_,live);
        trafficValidationMoving_=std::max(trafficValidationMoving_,moving);
        if(trafficValidationTime_>=kTrafficValidationWarmupSeconds){++trafficValidationSteadySamples_;trafficValidationSteadyMoving_+=moving;}
        saida::Log::info("[World traffic validation] t=",trafficValidationTime_," live=",live," moving=",moving,
            " requested=",requested," physical=",physical," grounded=",grounded," incidents=",incidents," damaged=",damaged);
        saida::Log::info("[World traffic validation] stops following=",reasons[1]," junction=",reasons[2],
            " blockedExit=",reasons[3]," signal=",reasons[4]," stopSign=",reasons[5]," deadEnds=",deadEnds);
        if(trafficValidationTime_>=trafficValidationSeconds_) {
            // A snapshot can coincide with all-red clearance. Measure sustained
            // motion after warmup instead of requiring a particular final phase.
            const double steadyMoving=trafficValidationSteadySamples_?double(trafficValidationSteadyMoving_)/trafficValidationSteadySamples_:0.;
            testFailed=trafficValidationLive_<kTrafficValidationMinimumLive||steadyMoving<kTrafficValidationMinimumMoving||trafficValidationStuck_;
            saida::Log::info("[World traffic validation] ",testFailed?"FAIL":"PASS"," peak live=",trafficValidationLive_," peak moving=",trafficValidationMoving_," steady moving=",steadyMoving," local persistent stop=",trafficValidationStuck_);
            engine.sceneTree().quit();
        }
    }
    std::array<saida::AssetID,3> signalMeshes_{};
    void prepareSignalMeshes() {
        if(signalMeshes_[0])return;
        auto& resources=engine.resources();
        signalMeshes_[0]=resources.queueMemoryMesh(saida::prepareMesh(r1::traffic_signal::pole()),"r1world/signal/pole");
        signalMeshes_[1]=resources.queueMemoryMesh(saida::prepareMesh(r1::traffic_signal::housing()),"r1world/signal/housing");
        signalMeshes_[2]=resources.queueMemoryMesh(saida::prepareMesh(r1::traffic_signal::lens()),"r1world/signal/lens");
    }
    bool frameTrafficSignal() {
        saida::Node* closest=nullptr;float distance=std::numeric_limits<float>::max();
        const auto observer=origin.local(ecef(lon,lat,alt));
        for(auto& [key,region]:loaded)for(const auto& child:region.node->children()) {
            if(child->name()!="Traffic signal")continue;
            const float candidate=glm::distance(glm::vec3(observer),glm::vec3(child->worldTransform()[3]));
            if(candidate<distance){distance=candidate;closest=child.get();}
        }
        if(!closest)return false;
        const auto matrix=closest->worldTransform();
        const auto position=glm::vec3(matrix*glm::vec4(1.4f,2.3f,-3.8f,1));
        const auto target=glm::vec3(matrix*glm::vec4(0,1.9f,r1::traffic_signal::kHeadOffset,1));
        player->setVisible(false);camera->transform().position=position;
        camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
        return true;
    }
    saida::Material* signalMaterial(uint32_t colour,bool lit) {
        static const std::array<glm::vec3,3> colours{{{1,.03f,.01f},{1,.5f,.01f},{.03f,1,.1f}}};
        constexpr float kLensGlassAlbedo=.08f,kLensEmissiveStrength=2.f;
        saida::MaterialDesc desc;
        desc.baseColor=glm::vec4(colours[colour]*kLensGlassAlbedo,1);
        desc.emissiveColor=glm::vec4(lit?colours[colour]*kLensEmissiveStrength:glm::vec3(0),1);
        desc.roughness=.35f;
        return engine.resources().getMaterial(desc);
    }
    void buildTrafficSignals(Loaded& tile,const json& doc) {
        if(!doc.contains("controls"))return;
        using namespace r1::traffic_signal;
        constexpr float kKerbClearance=.45f;
        constexpr double kMaximumRoadsideReach=10.;
        r1::RoadSurface roadSurface;
        if(auto surface=tile.data.find("roadSurface");surface!=tile.data.end())for(const auto& contour:*surface) {
            r1::Ring ring;
            for(const auto& point:contour)ring.push_back({point[0],point[1]});
            roadSurface.contours.push_back(std::move(ring));
        }
        prepareSignalMeshes();
        auto& resources=engine.resources();
        saida::MaterialDesc pole;pole.baseColor={.24f,.25f,.26f,1};pole.roughness=.4f;pole.metallic=.7f;
        auto* poleMaterial=engine.resources().getMaterial(pole);
        saida::MaterialDesc casing;casing.baseColor={.018f,.02f,.023f,1};casing.roughness=.5f;
        auto* housingMaterial=resources.getMaterial(casing);
        auto approachDirection=[&](const json& control) {
            return control.contains("stopDirection")?
                saida::traffic::Vec2{control["stopDirection"][0].get<float>(),control["stopDirection"][1].get<float>()}:
                tile.graph.direction(control.at("lane").get<uint32_t>());
        };
        std::map<std::pair<uint32_t,uint8_t>,const json*> approaches;
        for(const auto& control:doc.at("controls")) {
            if(control.at("kind")!="traffic_signals")continue;
            if(!control.value("stopLineDrawn",false)) {
                saida::Log::warn("[World signals] no drawable stop line for lane ",control.at("lane"),"; post omitted");
                continue;
            }
            const auto laneIndex=control.at("lane").get<uint32_t>();
            if(laneIndex>=tile.graph.lanes.size())continue;
            const auto& lane=tile.graph.lanes[laneIndex];if(lane.signal<0)continue;
            const auto key=std::make_pair(uint32_t(lane.signal),lane.signalGroup);
            auto [found,inserted]=approaches.emplace(key,&control);
            if(!inserted) {
                const auto& existing=*found->second;
                const auto direction=approachDirection(control);
                const float side=doc.value("leftHand",false)?-1.f:1.f;
                const auto& junction=tile.signalPositions[size_t(lane.signal)];
                auto lateral=[&](const json& candidate) {
                    const auto& p=candidate.at("stopPosition");
                    return (p[0].get<float>()-junction.x)*(-direction.y*side)+
                           (p[2].get<float>()-junction.y)*(direction.x*side);
                };
                if(lateral(control)>lateral(existing))found->second=&control;
            }
        }
        for(const auto& [key,control]:approaches) {
            const auto laneIndex=control->at("lane").get<uint32_t>();
            const auto& lane=tile.graph.lanes[laneIndex];
            const auto direction=approachDirection(*control);
            constexpr float kJunctionMatchTolerance=.1f,kCrossingDirectionCosine=.7f;
            const saida::traffic::Vec2 junction{tile.signalPositions[size_t(lane.signal)].x,tile.signalPositions[size_t(lane.signal)].y};
            float crossingHalfWidth=0.f;
            for(uint32_t crossing=0;crossing<tile.graph.lanes.size();++crossing) {
                const auto& other=tile.graph.lanes[crossing];
                if(std::abs(saida::traffic::dot(direction,tile.graph.direction(crossing)))>kCrossingDirectionCosine)continue;
                if(saida::traffic::length(tile.graph.nodes[other.from]-junction)>kJunctionMatchTolerance&&
                   saida::traffic::length(tile.graph.nodes[other.to]-junction)>kJunctionMatchTolerance)continue;
                crossingHalfWidth=std::max(crossingHalfWidth,std::abs(other.offset)+other.width*.5f);
            }
            const float junctionDistance=saida::traffic::dot(junction-tile.graph.nodes[lane.to],direction);
            const float setback=std::isfinite(lane.stopLineDistance)?lane.stopLineDistance:
                std::max(tile.flow.driver.stopLineDistance,crossingHalfWidth+kKerbClearance-junctionDistance);
            const auto centre=control->contains("stopPosition")?
                saida::traffic::Vec2{control->at("stopPosition")[0].get<float>(),control->at("stopPosition")[2].get<float>()}:
                tile.flow.positionOf(laneIndex,tile.graph.laneLength(laneIndex)-setback);
            const float side=doc.value("leftHand",false)?-1.f:1.f;
            const auto location=roadSurface.roadside({centre.x,centre.y},{-direction.y*side,direction.x*side},kKerbClearance,kMaximumRoadsideReach);
            if(!location) {
                saida::Log::warn("[World signals] no clear roadside for lane ",laneIndex,"; post omitted");
                continue;
            }
            const auto geo=geodeticOf(tile,{float(location->x),control->at("position")[1].get<float>(),float(location->y)});
            if(!this->tile(geo.x,geo.y)) {
                saida::Log::warn("[World signals] roadside terrain unavailable for lane ",laneIndex,"; post omitted");
                continue;
            }
            if(this->tile(geo.x,geo.y)!=&tile)continue; // one owning tile, even at streamed boundaries
            // The foundation sits on terrain; pavement may cover its foot.
            // A universal pavement lift would leave verge-mounted posts floating.
            const float ground=float(tile.frame.local(ecef(geo.x,geo.y,height(geo.x,geo.y))).y);
            auto* assembly=tile.node->createChild<saida::Node>("Traffic signal");
            assembly->transform().position={float(location->x),ground,float(location->y)};
            assembly->transform().rotation=glm::angleAxis(std::atan2(direction.x,direction.y),glm::vec3(0,1,0));
            assembly->createChild<saida::MeshNode>("Galvanized pole and brackets",resources.getMesh(signalMeshes_[0]),poleMaterial);
            for(int head=0;head<2;++head) {
                const float scale=head==0?1.f:kRepeaterScale,height=head==0?kHeadHeight:kRepeaterHeight;
                auto* housing=assembly->createChild<saida::MeshNode>(head==0?"Signal housing and visors":"Repeater housing and visors",resources.getMesh(signalMeshes_[1]),housingMaterial);
                housing->transform().position={0,height,kHeadOffset};housing->transform().scale=glm::vec3(scale);
                for(uint32_t colour=0;colour<3;++colour) {
                    auto* lamp=assembly->createChild<saida::MeshNode>("Signal lens",resources.getMesh(signalMeshes_[2]),signalMaterial(colour,false));
                    lamp->transform().position={0,height+(1.f-float(colour))*kLensPitch*scale,kHeadOffset};
                    lamp->transform().scale=glm::vec3(scale);
                    tile.signalLamps.push_back({lamp,uint32_t(lane.signal),lane.signalGroup,colour,false});
                }
            }
        }
    }
    void readGraph(Loaded& tile) {
        auto found=tile.data.find("traffic");
        if(found==tile.data.end())return;   // cooked before traffic existed
        const auto& doc=*found;
        for(auto& node:doc.at("nodes")) {
            tile.graph.nodes.push_back({float(node[0]),float(node[2])});
            tile.groundUp.push_back(float(node[1]));
        }
        for(const auto& entry:doc.at("lanes")) {
            using namespace r1::traffic_format;
            saida::traffic::Lane lane{uint32_t(entry[From]),uint32_t(entry[To]),float(entry[Speed]),float(entry[Weight])};
            if(entry.size()>AllowedTurns) {
                lane.offset=entry[Offset];lane.width=entry[Width];lane.road=entry[Road];lane.number=entry[Number];lane.priority=entry[Priority];
                lane.signal=entry[Signal];lane.signalGroup=entry[Group];lane.stop=entry[Stop];lane.yield=entry[Yield];lane.allowedTurns=entry[AllowedTurns];
            }
            if(entry.size()>Layer)lane.layer=entry[Layer];
            if(entry.size()>StopDistance)lane.stopLineDistance=entry[StopDistance];
            tile.graph.lanes.push_back(lane);
        }
        if(doc.contains("signals"))for(const auto& entry:doc.at("signals")) {
            saida::traffic::Signal signal;signal.key=entry.at("key");signal.offset=entry.value("offset",0.);
            tile.graph.signals.push_back(signal);
            const auto& p=entry.at("position");tile.signalPositions.push_back({p[0].get<float>(),p[2].get<float>()});
        }
        if(doc.contains("restrictions"))for(const auto& entry:doc.at("restrictions"))
            tile.graph.restrictions.push_back({entry[0],entry[1],entry[2],entry[3]});
        if(tile.graph.lanes.empty())return;
        tile.graph.rightTurnCrossSign=1;
        tile.graph.build();
        saida::traffic::Rules rules;
        rules.leftHand=doc.value("leftHand",false);
        rules.lateralAxisSign=-1.f;
        // The despawn radius is the streamer's, not a taste: a car further
        // than the tile ring is a car in a tile that is about to be evicted.
        rules.despawn=260.f;
        // The simulator measures centre-to-centre gaps. Reserve a bus length
        // plus clearance so two long vehicles cannot overlap in a queue.
        rules.minGap=saida::traffic::DriverSettings{}.defaultLength+saida::traffic::DriverSettings{}.bumperGap;
        rules.junctionGuard=14.f;
        // Deterministic per tile, so the same street is the same street on
        // every visit and a capture can be compared with itself.
        tile.flow.reset(&tile.graph,rules,uint32_t(std::hash<std::string>{}(tile.data.at("key").get<std::string>()))|1u);
        tile.motorcycleChance=motorcycleChance(tile.data.value("lon",0.),tile.data.value("lat",0.));
        tile.flow.setSpawnSize([this,&tile](uint32_t seed,const saida::traffic::Lane& lane) {
            const auto& kind=fleet[vehicleKind(seed,lane,tile.motorcycleChance)];
            return saida::traffic::VehicleSize{float(kind.length),float(kind.width)};
        });
        tile.wanted=doc.value("cars",0);
        buildTrafficSignals(tile,doc);
    }
    glm::vec3 paintFor(uint32_t seed) const {
        return paints[seed%paints.size()];
    }
    void paintCar(saida::Node& node,glm::vec3 albedo) {
        node.traverse([&](saida::Node& n,const glm::mat4&){
            if(!n.material()||n.name().rfind("paint-",0)!=0)return;
            auto desc=n.material()->desc();
            desc.baseColor=glm::vec4(albedo,desc.baseColor.a);
            constexpr float kVehicleReflection=1.f;
            desc.environmentReflection=std::max(desc.environmentReflection,kVehicleReflection);
            if(auto* mesh=dynamic_cast<saida::MeshNode*>(&n))
                mesh->setMaterial(engine.resources().getMaterial(desc));
        });
    }
    // The camera's position in one tile's tangent frame. It is the observer the
    // traffic spawns around, because it is the thing that sees.
    glm::dvec3 cameraTileLocal(const Loaded& tile) const {
        const glm::vec3 p=camera->transform().position;
        return tile.frame.local(origin.origin+origin.basis*glm::dvec3(p.x,p.y,p.z));
    }
    bool trafficVisible(const Loaded& tile,const saida::traffic::Pose& pose) const {
        const auto eye=cameraTileLocal(tile);
        const glm::dvec3 delta(pose.position.x-eye.x,0,pose.position.y-eye.z);
        constexpr double kVisibilityDistance=300.;
        constexpr double kVisibilityCosine=.15;
        const double distance=glm::length(delta);
        if(distance>kVisibilityDistance)return false;
        if(distance<1.)return true;
        const auto forward=glm::transpose(tile.frame.basis)*origin.basis*glm::dvec3(camera->transform().rotation*glm::vec3(0,0,-1));
        return glm::dot(delta,forward)/distance>kVisibilityCosine;
    }
    saida::traffic::Pose physicalTrafficPose(Loaded& tile,size_t slot) {
        auto& root=*tile.cars[slot];r1::VehicleRuntime::synchronize(root);
        const auto position=root.transform().position;
        const auto forward=root.transform().rotation*glm::vec3(0,0,-1);
        auto* rigid=r1::VehicleRuntime::body(root);
        const auto velocity=glm::transpose(glm::mat3(tile.node->worldTransform()))*rigid->physicsWorld()->linearVelocity(rigid->bodyId());
        return {{position.x,position.z},std::atan2(forward.x,forward.z),std::max(0.f,glm::dot(velocity,forward))};
    }
    saida::traffic::Obstacle vehicleTrafficObstacle(saida::Node& car,const Loaded& tile) const {
        glm::vec3 position;glm::quat rotation;
        if(r1::VehicleRuntime::physical(car)) {
            auto* body=r1::VehicleRuntime::body(car);
            body->physicsWorld()->getBodyTransform(body->bodyId(),position,rotation);
        } else {position=glm::vec3(car.worldTransform()[3]);rotation=glm::quat_cast(glm::mat3(car.worldTransform()));}
        const auto inverse=glm::inverse(tile.node->worldTransform());
        const auto local=glm::vec3(inverse*glm::vec4(position,1));
        const auto direction=glm::mat3(inverse)*(rotation*glm::vec3(0,0,-1));
        const auto& spec=vehicleSpec(car);
        saida::traffic::Obstacle obstacle;obstacle.active=car.enabled();obstacle.position={local.x,local.z};
        obstacle.halfLength=float(spec.length*.5);obstacle.halfWidth=float(spec.width*.5);obstacle.yaw=std::atan2(direction.x,direction.z);
        return obstacle;
    }
    void updateTraffic(float delta) {
        SAIDA_PROFILE_SCOPE("World/Traffic");
        if(!debugTraffic||fleet.empty())return;
        const float trafficDelta=std::clamp(delta,0.f,.25f);
        trafficTime_+=trafficDelta;
        auto promoteParked=[&](saida::Node* node) {
            if(!node||!node->enabled()||r1::VehicleRuntime::physical(*node))return;
            const auto eye=origin.local(ecef(lon,lat,alt));
            if(glm::length(glm::dvec3(node->transform().position)-eye)<r1::VehicleRuntime::kPromotionDistance)
                if(auto* physics=engine.sceneTree().world().physics())r1::VehicleRuntime::activate(*node,*physics,glm::vec3(0));
        };
        if(carParked&&!carSinking)promoteParked(car);
        for(auto& entry:parked)promoteParked(entry.node);
        // §5 again, from the other side: the whole neighbourhood shares one
        // fleet budget, and the tiles nearest the player spend it first --
        // `nearby` returns them in that order. Far tiles keep their graph and
        // simply show nothing, which costs one integer.
        size_t remaining=kTrafficCars;
        for(auto tile:ring) {
            auto found=loaded.find(tile.key());
            if(found==loaded.end())continue;
            Loaded& l=found->second;
            if(l.graph.lanes.empty())continue;
            const glm::dvec3 eye=cameraTileLocal(l);
            const size_t localCapacity=l.flow.capacityNear({float(eye.x),float(eye.z)});
            const size_t share=std::min({l.wanted,localCapacity,remaining});
            remaining-=share;
            l.flow.setPopulation(uint32_t(share));
            l.flow.setVisibility([this,&l](const saida::traffic::Pose& pose){return playing&&!pending&&trafficVisible(l,pose);});
            l.flow.setTime(trafficTime_);
            for(auto& lamp:l.signalLamps) {
                const auto light=l.graph.signals[lamp.program].light(lamp.group,trafficTime_);
                const bool lit=uint32_t(light)==lamp.colour;
                if(lit!=lamp.lit){lamp.node->setMaterial(signalMaterial(lamp.colour,lit));lamp.lit=lit;}
            }
            for(size_t i=0;i<l.cars.size();++i)if(l.cars[i]&&trafficSlotLive(l,i)&&r1::VehicleRuntime::physical(*l.cars[i])) {
                const auto pose=physicalTrafficPose(l,i);
                const auto* deform=r1::VehicleRuntime::body(*l.cars[i])->getBehaviour<saida::DeformableMeshBehaviour>();
                l.flow.setPhysicalPose(i,pose,deform&&deform->absorbedEnergy()>=deform->maximumImpactEnergy*r1::VehicleRuntime::kDisablingDamageFraction);
            }
            std::vector<saida::traffic::Obstacle> roadObstacles;
            // A transferred body can still occupy the old tile's road. Traffic
            // on either side must see its footprint while ownership changes.
            for(auto& [otherKey,otherRegion]:loaded)if(&otherRegion!=&l)
                for(size_t other=0;other<otherRegion.cars.size();++other)
                    if(otherRegion.cars[other]&&trafficSlotLive(otherRegion,other))
                        roadObstacles.push_back(vehicleTrafficObstacle(*otherRegion.cars[other],l));
            if(carParked&&car&&!driving)roadObstacles.push_back(vehicleTrafficObstacle(*car,l));
            for(auto& parkedCar:parked)roadObstacles.push_back(vehicleTrafficObstacle(*parkedCar.node,l));
            for(const auto& walker:l.crowd.walkers())if(walker.alive&&walker.link>=0&&walker.along>.75&&l.walks.links[walker.link].crossing)
                roadObstacles.push_back({{float(walker.x),float(walker.z)},float(r1::Crowd::kBodyRadius),true});
            l.flow.setObstacles(std::move(roadObstacles));
            // The observer is the camera, not the player. It stands eight and a
            // half metres behind him in a car, so "sixteen metres behind the
            // player" was eight metres behind the *camera* -- in shot, which is
            // half of why cars appeared in view.
            saida::traffic::Obstacle player;
            if(driving)player=vehicleTrafficObstacle(*car,l);
            // Which way the camera is looking, in this tile's frame, so cars
            // may appear close behind it and never close in front.
            const glm::vec3 ahead=camera->transform().rotation*glm::vec3(0,0,-1);
            const glm::dmat3 intoTile=glm::transpose(l.frame.basis)*origin.basis;
            const glm::dvec3 look=intoTile*glm::dvec3(ahead.x,ahead.y,ahead.z);
            const double flat=std::hypot(look.x,look.z);
            const saida::traffic::Vec2 facing=flat>1e-6
                ?saida::traffic::Vec2{float(look.x/flat),float(look.z/flat)}
                :saida::traffic::Vec2{0.f,0.f};
            l.flow.update(trafficDelta,{float(eye.x),float(eye.z)},facing,player);
            syncTrafficNodes(l,trafficDelta);
        }
        transferTraffic();
    }
    static void resizeTrafficSlots(Loaded& tile,size_t count) {
        if(tile.cars.size()>=count)return;
        tile.cars.resize(count,nullptr);tile.carKinds.resize(count,0);tile.carWheels.resize(count);
        tile.carSpin.resize(count,0);tile.carIdentities.resize(count,0);
    }
    bool transferTrafficCar(Loaded& source,size_t index,Loaded& destination) {
        auto* root=source.cars[index];
        r1::VehicleRuntime::synchronize(*root);
        root->updateTransforms(source.node->worldTransform(),true);
        const auto into=glm::inverse(destination.node->worldTransform())*root->worldTransform();
        const auto forward=glm::mat3(into)*glm::vec3(0,0,-1);
        const auto agent=source.flow.agents()[index];
        const saida::traffic::Pose actual{{into[3].x,into[3].z},std::atan2(forward.x,forward.z),agent.speed};
        const auto slot=destination.flow.adopt(agent,actual,source.graph.lanes[agent.lane].road);
        if(!slot)return false;
        auto owned=source.node->detachChild(root);
        if(!owned){destination.flow.retire(*slot);return false;}
        source.flow.retire(index);
        root->transform().position=glm::vec3(into[3]);
        root->transform().rotation=glm::normalize(glm::quat_cast(glm::mat3(into)));
        resizeTrafficSlots(destination,*slot+1);
        if(destination.cars[*slot])destination.cars[*slot]->queueFree();
        destination.cars[*slot]=destination.node->addChild(std::move(owned));
        destination.carKinds[*slot]=source.carKinds[index];destination.carIdentities[*slot]=source.carIdentities[index];
        destination.carWheels[*slot]=std::move(source.carWheels[index]);destination.carSpin[*slot]=source.carSpin[index];
        source.cars[index]=nullptr;source.carIdentities[index]=0;
        return true;
    }
    void preserveTrafficCar(Loaded& source,size_t index) {
        auto* root=source.cars[index];
        r1::VehicleRuntime::synchronize(*root);
        root->updateTransforms(source.node->worldTransform(),true);
        const auto world=root->worldTransform();
        const auto point=origin.origin+origin.basis*glm::dvec3(world[3]);
        const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
        auto owned=source.node->detachChild(root);if(!owned)return;
        source.flow.retire(index);source.cars[index]=nullptr;source.carIdentities[index]=0;
        root=engine.sceneTree().world().addChild(std::move(owned));
        root->transform().position=glm::vec3(world[3]);root->transform().rotation=glm::quat_cast(glm::mat3(world));
        if(auto* drive=r1::VehicleRuntime::driver(*root))drive->parked=true;
        const auto forward=root->transform().rotation*glm::vec3(0,0,-1);
        parked.push_back({root,geo.x,geo.y,geo.z-r1::VehicleRuntime::kBodyClearance,std::atan2(double(forward.x),-double(forward.z))/rad});
    }
    void transferTraffic() {
        for(auto& [key,source]:loaded)for(size_t i=0;i<source.cars.size();++i) {
            auto* root=source.cars[i];if(!root||!trafficSlotLive(source,i))continue;
            const auto geo=geodeticOf(source,root->transform().position);
            auto* destination=tile(geo.x,geo.y);
            const auto& agent=source.flow.agents()[i];
            const float remaining=source.graph.laneLength(agent.lane)-agent.s;
            constexpr float kBoundaryProbeDistance=2.f;
            const auto terminal=source.flow.positionOf(agent.lane,source.graph.laneLength(agent.lane))+source.graph.direction(agent.lane)*kBoundaryProbeDistance;
            const auto ahead=geodeticOf(source,{terminal.x,root->transform().position.y,terminal.y});
            auto* forwardTile=tile(ahead.x,ahead.y);
            // Early handoff leaves the centre in the old tile for a few frames.
            // Ownership follows the road's exit; its entry is never a return
            // transfer just because the body has not crossed the seam yet.
            if(destination&&destination!=&source&&destination!=forwardTile)continue;
            if((!destination||destination==&source)&&agent.nextLane==UINT32_MAX&&remaining<source.flow.driver.transferTolerance) {
                destination=forwardTile;
            }
            if(!destination||destination==&source||destination->graph.lanes.empty())continue;
            transferTrafficCar(source,i,*destination);
        }
    }
    bool trafficSlotLive(const Loaded& tile,size_t slot) const {
        const auto& agents=tile.flow.agents();
        return debugTraffic&&slot<agents.size()&&agents[slot].alive;
    }
    // Reuse each slot's graph; dormant slots retain resources without rendering.
    void syncTrafficNodes(Loaded& tile,float delta) {
        const auto& agents=tile.flow.agents();
        resizeTrafficSlots(tile,agents.size());
        for(size_t i=0;i<tile.cars.size();++i) {
            const bool live=trafficSlotLive(tile,i);
            if(live&&tile.cars[i]&&tile.carIdentities[i]!=agents[i].identity&&
               (r1::motorcycle(fleet[tile.carKinds[i]].name)||
                tile.carKinds[i]!=vehicleKind(agents[i].vehicleSeed,tile.graph.lanes[agents[i].lane],tile.motorcycleChance))) {
                tile.cars[i]->setEnabled(false);tile.cars[i]->queueFree();tile.cars[i]=nullptr;
            }
            if(!tile.cars[i]) {
                if(!live)continue;                 // no node until a slot is used
                const auto& agent=agents[i];
                tile.carKinds[i]=vehicleKind(agent.vehicleSeed,tile.graph.lanes[agent.lane],tile.motorcycleChance);
                auto node=clonePlant(*fleet[tile.carKinds[i]].prototype);
                customizeMotorcycleRider(*node,agent.vehicleSeed,tile.data.value("lat",0.));
                vehicleLod(*node);vehicleCollider(*node,fleet[tile.carKinds[i]]);
                paintCar(*node,paintFor(agent.seed));
                tile.carWheels[i].clear();tile.carSpin[i]=0.;
                std::function<void(saida::Node&)> wheels=[&](saida::Node& n){
                    if(n.name().rfind("wheel-",0)==0){tile.carWheels[i].push_back(&n);return;}
                    for(auto& c:n.children())wheels(*c);
                };
                wheels(*node);
                tile.cars[i]=tile.node->addChild(std::move(node));
            }
            if(!live) {
                tile.cars[i]->setEnabled(false);
                continue;
            }
            tile.cars[i]->setEnabled(true);
            const auto& agent=agents[i];
            if(tile.carIdentities[i]!=agent.identity) {
                r1::VehicleRuntime::reset(*tile.cars[i]);tile.carIdentities[i]=agent.identity;
                const auto& dimensions=fleet[tile.carKinds[i]];
                tile.flow.setVehicleDimensions(i,float(dimensions.length),float(dimensions.width));
                const auto profile=r1::vehicleDynamics(dimensions.name);
                const auto* vehicleDrive=r1::VehicleRuntime::driver(*tile.cars[i]);
                tile.flow.setVehicleLimits(i,{profile.acceleration,profile.braking,profile.grip,profile.maximumSpeed,
                    float(dimensions.wheelbase),glm::radians(vehicleDrive->steeringAngle)});
                paintCar(*tile.cars[i],paintFor(uint32_t(agent.identity)));
            }
            // Agent seeds advance at junctions. Keep model and paint stable
            // for the pooled node instead of changing a car in full view.
            tile.carSpin[i]+=agent.speed*delta/fleet[tile.carKinds[i]].wheelRadius;
            for(auto* wheel:tile.carWheels[i])wheel->transform().rotation=
                glm::angleAxis(float(r1::motorcycle(fleet[tile.carKinds[i]].name)?tile.carSpin[i]:-tile.carSpin[i]),glm::vec3(1,0,0));
            const auto pose=tile.flow.pose(agent);
            const auto& lane=tile.graph.lanes[agent.lane];
            const float span=tile.graph.laneLength(agent.lane);
            const float t=span>0.f?agent.s/span:0.f;
            const float up=tile.groundUp[lane.from]*(1.f-t)+tile.groundUp[lane.to]*t;
            const saida::traffic::Vec2 dir{std::sin(pose.yaw),std::cos(pose.yaw)};
            // Engine z points south, so north is -z and the bearing the rest of
            // this file speaks is atan2(east, north).
            const double bearing=std::atan2(double(dir.x),-double(dir.y));
            auto& root=*tile.cars[i];
            const auto observer=tile.frame.local(ecef(lon,lat,alt));
            const float distance=std::hypot(float(observer.x)-pose.position.x,float(observer.z)-pose.position.y);
            if(!r1::VehicleRuntime::physical(root)) {
                root.transform().position=glm::vec3(pose.position.x,up+r1::VehicleRuntime::kBodyClearance,pose.position.y);
                const float rise=(tile.groundUp[lane.to]-tile.groundUp[lane.from])/std::max(.05f,span);
                root.transform().rotation=glm::angleAxis(float(-bearing),glm::vec3(0,1,0))*glm::angleAxis(std::atan(rise),glm::vec3(1,0,0));
                const float promotion=r1::VehicleRuntime::kPromotionDistance+std::abs(float(carSpeed))*r1::VehicleRuntime::kPredictionTime;
                if(distance<promotion)if(auto* physics=engine.sceneTree().world().physics())
                    r1::VehicleRuntime::activate(root,*physics,glm::mat3(tile.node->worldTransform())*
                        (root.transform().rotation*glm::vec3(0,0,-1))*agent.speed);
            }
            if(r1::VehicleRuntime::physical(root)) {
                const auto command=tile.flow.command(i,pose);
                auto* drive=r1::VehicleRuntime::driver(root);drive->parked=false;drive->command(command.throttle,command.brake,command.steer);
                const auto lanePosition=tile.flow.positionOf(agent.lane,agent.s);
                const auto laneDirection=tile.graph.direction(agent.lane);
                const bool aligned=saida::traffic::length(pose.position-lanePosition)<.5f&&
                    saida::traffic::dot(dir,laneDirection)>.98f;
                if(distance>r1::VehicleRuntime::kDemotionDistance&&!agent.incident&&!trafficVisible(tile,pose)&&
                   aligned&&r1::VehicleRuntime::stableForTraffic(root)) {
                    r1::VehicleRuntime::deactivate(root);tile.flow.releasePhysicalPose(i);
                }
            }
        }
    }
    size_t trafficWanted() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=tile.wanted;
        return n;
    }
    size_t trafficLive() const {
        size_t n=0;
        for(const auto& [key,tile]:loaded)n+=tile.flow.live();
        return n;
    }

    // A photographed surface's texture, registered once per path and colour space.
    saida::AssetID texture(const std::string& path,bool srgb) {
        if(path.empty())return saida::kAssetInvalid;
        auto [it,inserted]=textures.try_emplace(path+(srgb?"#srgb":"#linear"),saida::kAssetInvalid);
        if(inserted)it->second=engine.resources().getOrRegister((game/path).string(),saida::AssetType::Texture,srgb);
        return it->second;
    }
    saida::Material* material(const r1::Material& m) {
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
    // The tile's node, empty but for its sea when it is all ocean: its own
    // ground, streets, works and buildings are uploaded a few parts a frame
    // by `uploadParts`, straight from what the worker prepared.
    std::unique_ptr<saida::Node> buildTile(const std::string& key,const r1::ServedTile& served) {
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
    // The player and the car push the grass aside where they pass.
    static constexpr float kWalkerBend=0.45f,kCarBend=1.6f;
    void bendGrass() {
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
    // The tile's grass blades (gen/grass), on its ground as drawn, in the
    // tile's own frame: the engine makes them around the camera.
    static std::unique_ptr<saida::GrassNode> grassNode(const r1::CookedTile& cooked) {
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
    // A mount used to upload a whole tile in one frame -- 30 to 60 ms, the
    // hitch every new tile was felt as. Now the ground goes first, a few
    // milliseconds of parts a frame, and the rest follows over the next frames.
    // Physics follows each uploaded mesh; arrivals wait for its first sync.
    void uploadParts() {
        const auto start=std::chrono::steady_clock::now();
        const auto priority=orderedNearby((pending||warming)?pickLon:lon,(pending||warming)?pickLat:lat);
        // A tile's replacement uploads right after it: the old one is shown
        // until then, so it is the one standing in for it on screen.
        std::vector<std::pair<const std::string*,Loaded*>> order;
        for(const auto tile:priority)for(auto* tiles:{&loaded,&incoming}) {
            auto found=tiles->find(tile.key());
            if(found!=tiles->end())order.push_back({&found->first,&found->second});
        }
        for(auto [name,tile]:order) {
            const auto& key=*name;auto& t=*tile;
            if(!t.geography)continue;
            const auto& parts=std::any_cast<const PreparedTile&>(t.served->prepared).parts;
            while(t.nextPart<parts.size()) {
                const PartUpload& part=parts[t.nextPart++];
                auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(part.prepared));
                if(!mesh) {
                    // Said, never silent: a tile missing a part looks like OSM missing it.
                    saida::Log::warn("[World streaming] the geometry arena refused ",part.name," of ",key);
                    continue;
                }
                t.partMeshes.push_back(mesh);
                meshCollider(*t.geography,std::make_unique<saida::MeshNode>(part.name,mesh,
                    material(t.served->cooked.parts[part.material].material)));
                if(msSince(start)>=kPartUploadMs)return;
            }
        }
    }
    static const std::vector<PartUpload>& buildingParts(const Loaded& t,size_t index,int level) {
        const auto& source=std::any_cast<const PreparedTile&>(t.served->prepared).buildings[index];
        return level==0&&t.reducedDensity?source.reducedNear:source.levels[size_t(level)];
    }
    static const std::vector<r1::MeshPart>& buildingMaterials(const Loaded& t,size_t index,int level) {
        const auto& source=t.served->cooked.buildings[index];
        return level==0&&t.reducedDensity?source.reducedNear:source.levels[size_t(level)];
    }
    static size_t residentVertices(const json& data) { return data.value("residentVertices",data.at("vertices").get<size_t>()); }
    static size_t residentIndices(const json& data) { return data.value("residentIndices",data.value("indices",size_t(0))); }
    static std::pair<size_t,size_t> buildingExtraGeometry(const Loaded& t) {
        size_t vertices=0,indices=0;
        for(size_t i=0;i<t.buildings.size();++i)for(int level=0;level<2;++level)
            if(t.buildings[i].levels[size_t(level)].started)for(const auto& part:buildingParts(t,i,level)) {
                vertices+=part.prepared->geometry.vertices.size();indices+=part.prepared->geometry.indices.size();
            }
        return {vertices,indices};
    }
    static bool buildingsReady(const Loaded& t) {
        for(size_t i=0;i<t.buildings.size();++i) {
            const auto& state=t.buildings[i].levels[2];
            if(!state.started||state.nextPart<buildingParts(t,i,2).size())return false;
            for(auto* mesh:state.meshes)if(!mesh->loaded())return false;
        }
        return true;
    }
    static bool nearBuildingReady(const Loaded& t,size_t footprint) {
        if(footprint>=t.visualForFootprint.size()||t.visualForFootprint[footprint]<0)return false;
        const auto& b=t.buildings[size_t(t.visualForFootprint[footprint])];
        return b.lod->activeLevel()==0&&b.levels[0].started&&
            std::all_of(b.levels[0].meshes.begin(),b.levels[0].meshes.end(),[](auto* mesh){return mesh->loaded();});
    }
    // Only requested representations occupy the arena. The tiny Far stays as
    // a fallback while a closer level uploads; physical shells never change.
    void updateBuildingLods() {
        SAIDA_PROFILE_SCOPE("World/BuildingLods");
        // An abandoned upload is protected by the resource cache until its
        // job finishes. Sweep once more then, so it cannot retain arena space.
        if(buildingTrimPending&&engine.resources().assetLoadsSettled()) {trim();buildingTrimPending=false;}
        struct Candidate {Loaded* tile;size_t index;double distance;};
        std::vector<Candidate> candidates;
        size_t reservedV=farVertices()+farPack.vertices+nextFarPack.vertices,reservedI=0;
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
                    bool ready=state.started&&state.nextPart==parts.size();
                    for(auto* mesh:state.meshes)ready=ready&&mesh->loaded();
                    b.lod->setLevelReady(level,ready);
                    if(state.started&&state.nextPart==parts.size()&&!ready&&!state.failed&&
                       engine.resources().assetLoadsSettled()) {
                        state.failed=true;
                        saida::Log::error("[World building LOD] ",source.id," level=",level,
                            " GPU upload failed; retaining available representation");
                        if(level==2)text("stream-status","A building could not be loaded.");
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
                const auto& part=parts[state.nextPart++];
                auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(part.prepared));
                if(!mesh)throw std::runtime_error("Building visual mesh registration refused");
                auto node=std::make_unique<saida::MeshNode>(part.name,mesh,material(materials[part.material].material));
                node->castShadows()=level!=2;
                state.root->addChild(std::move(node));state.meshes.push_back(mesh);
                if(msSince(started)>=kPartUploadMs)return;
            }
        }
    }
    void placeTiles() {
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
    void trim() {
        engine.sceneTree().applyDeferred();
        engine.resources().trimUnused(engine.sceneTree().world().resourceUsage());
    }
    // Only two nearby interiors occupy the shared arena. A room containing
    // the player is pinned until they leave; hysteresis prevents door thrashing.
    void updateInteriors(double dt) {
        SAIDA_PROFILE_SCOPE("World/Interiors");
        struct Candidate { Loaded* tile; LiveInterior* room; double distance; };
        std::vector<Candidate> candidates;bool released=false;size_t active=0;
        const auto playerEcef=ecef(lon,lat,alt);
        for(auto& [key,t]:loaded) {
          const auto q=t.frame.local(playerEcef);const r1::P2 at{q.x,q.z};
          for(auto& room:t.interiors) {
            const double distance=room.contains(at)?0:r1::dist(at,room.plan.door);
            if(room.node&&!room.ready) {
                bool ready=true;
                room.node->traverse([&](saida::Node& node,const glm::mat4&) {
                    if(auto* mesh=node.mesh())ready=ready&&mesh->loaded();
                });
                if(ready&&nearBuildingReady(t,room.plan.footprint)) {
                    room.ready=true;room.node->setEnabled(true);
                    saida::Log::info("[World interiors] ready ",room.plan.name);
                } else if(!ready&&engine.resources().assetLoadsSettled()) {
                    room.node->queueFree();room.node=nullptr;room.leaves[0]=room.leaves[1]=nullptr;
                    room.refused=true;released=true;
                    saida::Log::error("[World interiors] geometry upload failed: ",room.plan.name);
                    text("stream-status","This interior could not be loaded.");
                    if(smoke){testFailed=true;engine.sceneTree().quit();return;}
                }
            }
            if(!room.ready&&distance<kClosedDoorNear&&!room.closedDoor) {
                const auto& p=room.plan;
                room.closedDoor=boxCollider(*t.node,"Unloaded interior door",{float(p.width),2.5f,.10f},{0,1.25f,0});
                room.closedDoor->transform().position={float(p.door.x),float(p.floor),float(p.door.y)};
                room.closedDoor->transform().rotation=glm::angleAxis(float(-std::atan2(p.along.y,p.along.x)),glm::vec3(0,1,0));
            }
            if(room.closedDoor&&((room.node&&room.ready)||distance>kClosedDoorFar)) {room.closedDoor->queueFree();room.closedDoor=nullptr;}
            if(distance>kInteriorRelease)room.refused=false; // Retry a capacity refusal on a later visit.
            if(room.node&&distance>kInteriorRelease) {
                room.node->queueFree();room.node=nullptr;room.leaves[0]=room.leaves[1]=nullptr;
                room.layout={};room.opening=0;room.ready=false;released=true;
            }
            if(room.node)++active;
            if(!t.reducedDensity&&!room.node&&!room.refused&&distance<kInteriorLoad&&
               nearBuildingReady(t,room.plan.footprint))candidates.push_back({&t,&room,distance});
            if(room.node&&room.ready) {
                auto p=room.plan.local(at);
                // Sensor on both sides, with a hold time and a wide safety zone.
                const bool sensor=!driving&&!sailing&&!piloting&&std::abs(p.x)<room.plan.width/2+1.5&&std::abs(p.y)<3.5;
                room.hold=sensor?1.5:std::max(0.,room.hold-dt);
                room.opening=r1::slideDoor(room.opening,room.hold>0,dt);
                for(int side=0;side<2;++side) {
                    const double sign=side?1.:-1.;const auto& plan=room.plan;
                    const double angle=room.opening*r1::kPi*.5;
                    double u=plan.doorStyle=="sliding"?sign*plan.width*(.25+.5*room.opening):sign*plan.width*(.5-.25*std::cos(angle));
                    auto at=plan.point(u,plan.doorStyle=="sliding"?-.025:plan.width*.25*std::sin(angle));
                    room.leaves[side]->transform().position={float(at.x),float(room.plan.floor),float(at.y)};
                    room.leaves[side]->transform().rotation=glm::angleAxis(float(-std::atan2(plan.along.y,plan.along.x)+(plan.doorStyle=="swing"?sign*angle:0.)),glm::vec3(0,1,0));
                }
            }
          }
        }
        if(released)trim();
        std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){return a.distance!=b.distance?a.distance<b.distance:a.room->plan.id<b.room->plan.id;});
        if(active>=kInteriorRooms&&!candidates.empty()) {
            LiveInterior* farthest=nullptr;double farDistance=candidates.front().distance+5;
            for(auto& [key,t]:loaded)for(auto& room:t.interiors)if(room.node) {
                auto q=t.frame.local(ecef(lon,lat,alt));r1::P2 at{q.x,q.z};
                if(room.contains(at))continue;
                const double d=r1::dist(at,room.plan.door);
                if(d>farDistance){farDistance=d;farthest=&room;}
            }
            if(farthest) {
                farthest->node->queueFree();farthest->node=nullptr;farthest->leaves[0]=farthest->leaves[1]=nullptr;
                farthest->layout={};farthest->opening=0;farthest->ready=false;--active;trim();
            }
        }
        if(active>=kInteriorRooms||candidates.empty())return;
        auto& room=*candidates.front().room;auto& t=*candidates.front().tile;const auto& p=room.plan;
        if(p.recipe!="home"&&p.recipe!="warehouse") {
            const auto letters=r1::facadeLettering(p.name,3.,.75);
            if(letters&&!modelsReady(*letters))return;
        }
        SAIDA_PROFILE_SCOPE("World/BuildInterior");
        room.layout=r1::layoutInterior(p);auto parts=r1::buildInteriorShell(p);auto door=r1::buildInteriorDoor(p);
        std::map<std::string,std::vector<r1::MeshPart>> furnishings;
        for(const auto& fixture:room.layout.fixtures) {
            const auto key=r1::interiorFixtureKey(fixture);
            if(!furnishings.count(key))furnishings.emplace(key,r1::buildInteriorFixture(fixture));
        }
        size_t vertices=0,indices=0;for(auto& part:parts){vertices+=part.mesh.vertexCount();indices+=part.mesh.indices.size();}
        for(const auto& [key,prototype]:furnishings)for(const auto& part:prototype){vertices+=part.mesh.vertexCount();indices+=part.mesh.indices.size();}
        for(auto& part:door){vertices+=2*part.mesh.vertexCount();indices+=2*part.mesh.indices.size();}
        const auto used=engine.resources().geometryUsage();const auto cap=engine.resources().geometryCapacity();
        if(vertices>24000||used.vertices+vertices+4096>cap.vertices||used.indices+indices+12000>cap.indices) {
            room.refused=true;saida::Log::warn("[World interiors] arena refused ",p.name," vertices=",vertices);
            text("stream-status","Interior unavailable: geometry budget reached.");return;
        }
        auto root=std::make_unique<saida::Node>("Interior "+std::to_string(p.id));
        auto upload=[&](saida::Node& parent,const std::vector<r1::MeshPart>& list) {
            for(size_t i=0;i<list.size();++i)if(!list[i].mesh.empty()) {
                auto up=uploadOf(list[i],i);auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(
                    saida::prepareMesh({std::move(up.vertices),std::move(up.indices)})));
                if(!mesh)throw std::runtime_error("interior mesh allocation refused");
                auto node=std::make_unique<saida::MeshNode>(up.name,mesh,material(list[i].material));
                // The existing opaque roof and exterior walls cast the room's
                // sun shadow. Thin linings and ceiling rails must not cast a
                // second, nearly coincident shadow onto that same surface.
                node->castShadows()=up.name.find("door")!=std::string::npos;
                if(i<3&&up.name!="Interior surface joints")meshCollider(parent,std::move(node));
                else parent.addChild(std::move(node));
            }
        };
        try {
            upload(*root,parts);
            const double yaw=-std::atan2(p.along.y,p.along.x);
            // Stable resource keys let the engine share each prototype across
            // every instance and both active rooms. No furniture mesh is baked
            // repeatedly into a tile or regenerated for every shelf.
            struct SharedPart { saida::Mesh* mesh; saida::Material* paint; bool shadow; };
            std::map<std::string,std::vector<SharedPart>> prototypes;
            for(const auto& [key,prototype]:furnishings)for(size_t i=0;i<prototype.size();++i) {
                if(prototype[i].mesh.empty())continue;
                auto up=uploadOf(prototype[i],i);
                auto* mesh=engine.resources().getMesh(engine.resources().queueMemoryMesh(
                    saida::prepareMesh({std::move(up.vertices),std::move(up.indices)}),
                    "generated/interior-prototypes/v25/"+key+"/"+std::to_string(i)));
                if(!mesh)throw std::runtime_error("furniture prototype allocation refused");
                prototypes[key].push_back({mesh,material(prototype[i].material),i==0||i==2});
            }
            size_t fixtureIndex=0;
            for(const auto& fixture:room.layout.fixtures) {
                if(fixture.height>.05) {
                    auto* body=boxCollider(*root,fixture.kind+" collider "+std::to_string(fixtureIndex),
                        {float(fixture.size.x),float(fixture.height),float(fixture.size.y)},
                        {0,float(fixture.height*.5),0});
                    const auto at=p.point(fixture.at.x,fixture.at.y);
                    body->transform().position={float(at.x),float(p.floor),float(at.y)};
                    body->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));
                }
                ++fixtureIndex;
                if(fixture.kind=="vehicle") {
                    if(fleet.empty())throw std::runtime_error("garage requires the road vehicle fleet");
                    auto instance=clonePlant(*fleet[fixture.variant%std::min<size_t>(5,fleet.size())].prototype);vehicleLod(*instance);
                    const auto at=p.point(fixture.at.x,fixture.at.y);
                    instance->transform().position={float(at.x),float(p.floor+.04),float(at.y)};
                    instance->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));root->addChild(std::move(instance));continue;
                }
                for(const auto& [mesh,paint,shadow]:prototypes.at(r1::interiorFixtureKey(fixture))) {
                    auto instance=std::make_unique<saida::MeshNode>(fixture.kind,mesh,paint);
                    instance->castShadows()=shadow;
                    auto at=p.point(fixture.at.x,fixture.at.y);
                    instance->transform().position={float(at.x),float(p.floor),float(at.y)};
                    instance->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));
                    root->addChild(std::move(instance));
                }
            }
            for(const auto at:p.exteriorVehicles) {
                if(fleet.empty())throw std::runtime_error("garage requires the road vehicle fleet");
                const auto& spec=fleet[size_t(uint64_t(p.id)%std::min<size_t>(5,fleet.size()))];
                auto instance=clonePlant(*spec.prototype);vehicleLod(*instance);vehicleCollider(*instance,spec);
                instance->transform().position={float(at.x),float(at.y+.04),float(at.z)};
                instance->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));root->addChild(std::move(instance));
            }
            for(int side=0;side<2;++side) {
                auto leaf=std::make_unique<saida::Node>(p.doorStyle=="sliding"?"Automatic sliding leaf":"Automatic swing leaf");upload(*leaf,door);
                auto at=p.point((side?1:-1)*p.width*.25,-.025);
                leaf->transform().position={float(at.x),float(p.floor),float(at.y)};
                leaf->transform().rotation=glm::angleAxis(float(yaw),glm::vec3(0,1,0));
                room.leaves[side]=root->addChild(std::move(leaf));
            }
            auto a=p.ring[p.edge],b=p.ring[(p.edge+1)%p.ring.size()];
            const double fasciaHeight=std::clamp(p.ceiling-p.floor-2.8,.3,1.1);
            const double letterHeight=std::min(.75,fasciaHeight-.12);
            if(p.recipe!="home"&&p.recipe!="warehouse")if(auto sign=lettering(p.name,r1::retailInterior(p.recipe)?r1::dist(a,b)-.8:3.,letterHeight,{.65f,.65f,.61f})) {
                const auto center=r1::retailInterior(p.recipe)?r1::P2{(a.x+b.x)/2,(a.y+b.y)/2}:p.door;
                sign->transform().position={float(center.x-p.inward.x*.29),
                    float(p.floor+(r1::retailInterior(p.recipe)?2.8+(fasciaHeight-letterHeight)/2:2.22)),float(center.y-p.inward.y*.29)};
                // Glyphs face +Z, the shop's exterior is -inward. Reverse
                // their baseline relative to the polygon's CCW edge.
                sign->transform().rotation=glm::angleAxis(float(yaw+r1::kPi),glm::vec3(0,1,0));
                root->addChild(std::move(sign));
            } else saida::Log::warn("[World interiors] font cannot spell building name: ",p.name);
            // A few ceiling lights, scoped to this streamed room.
            // A mall's supermarket is lit where it is, not only at the door:
            // over its checkouts and around its mapped position.
            std::vector<r1::P2> lit;
            for(double v:{3.,11.,20.})lit.push_back(p.point(0,v));
            if(!r1::retailInterior(p.recipe))for(size_t i=0;i<room.layout.rooms.size()&&i<8;++i)lit.push_back(r1::centroid(room.layout.rooms[i].ring));
            if(p.anchor) {
                const r1::P2 a=*p.anchor,d=p.door;const double l=r1::dist(a,d);
                const r1::P2 toward{(a.x-d.x)/l,(a.y-d.y)/l},across{-toward.y,toward.x};
                for(double s:{l/2+4,l/2+16})for(double w:{-9.,9.})
                    lit.push_back({d.x+toward.x*s+across.x*w,d.y+toward.y*s+across.y*w});
                lit.push_back(a);
            }
            for(const auto at:lit) {
                if(!r1::pointInPolygon(at,p.ring))continue;
                auto light=std::make_unique<saida::LightNode>("Interior ceiling light",saida::LightType::Point);
                light->transform().position={float(at.x),float(p.ceiling-.25),float(at.y)};
                light->color={1.f,.93f,.82f};light->intensity=r1::retailInterior(p.recipe)?3.f:.8f;
                light->range=r1::retailInterior(p.recipe)?12.f:8.f;light->castShadows=false;
                root->addChild(std::move(light));
            }
            root->setEnabled(false);
            room.ready=false;room.node=t.node->addChild(std::move(root));
            saida::Log::info("[World interiors] queued ",p.name," recipe=",p.recipe," vertices=",vertices,
                " furniture_instances=",room.layout.fixtures.size()," prototypes=",furnishings.size());
        } catch(const std::exception& e) {
            root.reset();trim();
            room.refused=true;room.leaves[0]=room.leaves[1]=nullptr;
            saida::Log::error("[World interiors] ",p.name,": ",e.what());
            text("stream-status","This interior could not be loaded.");
        }
    }
    int retailTestPhase=0;int64_t retailTestId=0;double retailTestTime=0,retailTestV=-3;const char* retailTestWait="";
    bool retailTest() const { return smoke&&(std::getenv("R1WORLD_RETAIL_SMOKE")||std::getenv("R1WORLD_INTERIOR_SMOKE")); }
    // R1WORLD_RETAIL_SHOT=<png> with R1WORLD_RETAIL_SHOT_AT=outside|inside|
    // anchor photographs the store the traversal chose, facing into it (or,
    // for "anchor", from behind a mall's checkouts toward its supermarket),
    // then ends the run: the test says the door opens, only a picture says
    // it looks like a shop (rule 1 of CLAUDE.md).
    bool retailShot(const char* moment,const r1::InteriorPlan& plan,r1::P2 facing={0,0}) {
        const char* path=std::getenv("R1WORLD_RETAIL_SHOT");
        const char* at=std::getenv("R1WORLD_RETAIL_SHOT_AT");
        if(!path||!at||std::string(at)!=moment)return false;
        if(facing.x==0&&facing.y==0)facing=plan.inward;
        yaw=std::atan2(facing.x,-facing.y)/rad;
        if(captureQueued)return true;
        saida::CaptureRequest shot;
        shot.pngPath=path;shot.frame=30;shot.fixedStep=1.f/60.f;shot.settleTimeoutFrames=900;
        captureQueued=true;
        saida::Log::info("[World retail E2E] photographing ",plan.name," ",moment," to ",path);
        engine.captureFrameThenExit(shot);
        return true;
    }
    void runRetailTest(double dt) {
        smokeStarted=false;retailTestTime+=dt;
        if(captureQueued)return;
        auto fail=[&](const char* why){saida::Log::error("[World retail E2E] FAIL ",why);testFailed=true;engine.sceneTree().quit();};
        if(retailTestTime>45){saida::Log::error("[World retail E2E] phase=",retailTestPhase," v=",retailTestV," waiting=",retailTestWait);fail("interior traversal timed out");return;}
        Loaded* owner=nullptr;LiveInterior* found=nullptr;
        const std::string recipe=std::getenv("R1WORLD_INTERIOR_SMOKE")?std::getenv("R1WORLD_INTERIOR_SMOKE"):"";
        for(auto& [key,t]:loaded)for(auto& r:t.interiors)
            if((retailTestId&&r.plan.id==retailTestId)||(!retailTestId&&r.node&&(recipe.empty()?r1::retailInterior(r.plan.recipe):r.plan.recipe==recipe))) {owner=&t;found=&r;break;}
        if(!found||(found->node&&!found->ready))return;
        auto& r=*found;retailTestId=r.plan.id;
        double walkDepth=0;
        for(double v=.5;v<=12;v+=.5) {
            if(!r1::pointInPolygon(r.plan.point(0,v+.4),r.plan.ring))break;
            walkDepth=v;
        }
        if(walkDepth<1.){fail("interior has no walkable entrance aisle");return;}
        auto position=[&](double u,double v){auto p=r.plan.point(u,v);return r1::Anchor::at(owner->data.at("lon"),owner->data.at("lat"),0).toGeodetic(p.x,r.plan.floor,p.y);};
        // A shop near a tile edge has its forecourt in the neighbour, which
        // may still be streaming: wait for it rather than walk off the world.
        auto resident=[&](double v){auto p=position(0,v);retailTestWait="a tile at the shop to stream in";return tile(p.x,p.y)!=nullptr;};
        auto move=[&](double v,bool check){auto p=position(0,v);
            if(check) {
                auto from=owner->frame.local(ecef(lon,lat,alt));
                auto target=owner->frame.local(ecef(p.x,p.y,p.z));
                auto direction=glm::transpose(origin.basis)*owner->frame.basis*(target-from);
                const auto end=moveFeet(direction.x,-direction.z,std::max(.001,dt));
                if(glm::length(ecef(end.x,end.y,alt)-ecef(p.x,p.y,alt))>.18) {
                    saida::Log::error("[World retail E2E] stalled v=",v," altitude=",alt," jump=",jumpOffset);
                    for(const auto& contact:feet->contacts())if(contact.node)
                        saida::Log::error("[World retail E2E] contact=",contact.node->name()," normal=",contact.normal.x,",",contact.normal.y,",",contact.normal.z);
                    return false;
                }
                lon=end.x;lat=end.y;alt=end.z;
            } else {lon=p.x;lat=p.y;}
            if(!check)alt=height(lon,lat,alt);return true;};
        if(retailTestPhase==0) {if(!r.node||!resident(-18)||!resident(walkDepth))return;
            const std::string shotAt=std::getenv("R1WORLD_RETAIL_SHOT_AT")?std::getenv("R1WORLD_RETAIL_SHOT_AT"):"";
            if(shotAt=="room") {
                const r1::InteriorRoom* room=nullptr;
                const char* requestedRoom=std::getenv("R1WORLD_INTERIOR_ROOM");
                for(const auto& candidate:r.layout.rooms) {
                    if(!room)room=&candidate;
                    if(requestedRoom) {if(candidate.use==requestedRoom){room=&candidate;break;}continue;}
                    if((r.plan.recipe=="school"&&candidate.use=="classroom")||
                       (r.plan.recipe=="home"&&candidate.use=="living")||
                       (r.plan.recipe=="garage"&&candidate.use=="garage")){room=&candidate;break;}
                }
                if(!room){fail("no furnished room to photograph");return;}
                auto center=r.plan.local(r1::centroid(room->ring));double front=1e30,back=-1e30;
                for(auto p:room->ring){auto q=r.plan.local(p);front=std::min(front,q.y);back=std::max(back,q.y);}
                auto anchor=r1::Anchor::at(owner->data.at("lon"),owner->data.at("lat"),0);
                auto cameraPoint=r.plan.point(center.x,front+.5),targetPoint=r.plan.point(center.x,back-.5);
                auto cameraGeo=anchor.toGeodetic(cameraPoint.x,r.plan.floor+1.65,cameraPoint.y);
                auto targetGeo=anchor.toGeodetic(targetPoint.x,r.plan.floor+1.2,targetPoint.y);
                auto eye=origin.local(ecef(cameraGeo.x,cameraGeo.y,cameraGeo.z));
                auto aim=origin.local(ecef(targetGeo.x,targetGeo.y,targetGeo.z));
                captureView.set=true;for(int i=0;i<3;++i){captureView.position[i]=eye[i];captureView.target[i]=aim[i];}
                retailShot("room",r.plan);worldCapture.pngPath=std::getenv("R1WORLD_RETAIL_SHOT");return;
            }
            if(shotAt=="outside"){move(-18,false);retailShot("outside",r.plan);return;}
            if(shotAt=="anchor") {
                if(!r.plan.anchor){fail("no supermarket mapped inside this store");return;}
                const r1::P2 a=*r.plan.anchor,d=r.plan.door;const double l=r1::dist(a,d);
                const r1::P2 toward{(a.x-d.x)/l,(a.y-d.y)/l};
                // Just behind the checkout line, on the store's side.
                const auto spot=r1::Anchor::at(owner->data.at("lon"),owner->data.at("lat"),0)
                    .toGeodetic(d.x+toward.x*(l/2+6),r.plan.floor,d.y+toward.y*(l/2+6));
                lon=spot.x;lat=spot.y;alt=height(lon,lat,alt);
                retailShot("anchor",r.plan,toward);return;
            }
            move(-3,false);retailTestPhase=1;return;}
        if(retailTestPhase==1) {
            if(retailTestV<2&&(r.opening<.99||!feet->physicsWorld()))return;
            retailTestV=std::min(walkDepth,retailTestV+std::min(dt,.1)*5.);
            if(!move(retailTestV,true)){fail("open doorway or central aisle blocked");return;}
            if(retailTestV>=walkDepth){saida::Log::info("[World retail E2E] entered ",r.plan.name," recipe=",r.plan.recipe," and walked ",walkDepth," m inside");
                if(retailShot("inside",r.plan)){retailTestV=walkDepth;return;}
                retailTestPhase=2;}
            return;
        }
        if(retailTestPhase==2) {
            retailTestV=std::max(-2.,retailTestV-std::min(dt,.1)*5.);
            if(retailTestV<3&&r.opening<.99)return;
            if(!move(retailTestV,true)){fail("return through automatic door blocked");return;}
            if(retailTestV<=-2) {move(-6,false);retailTestPhase=3;}
            return;
        }
        if(retailTestPhase==3) {
            if(r.opening>.001)return;
            // An entrance can be only 30 cm from a facade corner. Probe the
            // middle of its longer opaque span, rather than beyond that edge.
            const double left=r.plan.local(r.plan.ring[r.plan.edge]).x;
            const double right=r.plan.local(r.plan.ring[(r.plan.edge+1)%r.plan.ring.size()]).x;
            const double half=r.plan.width/2;
            const double wallU=(-half-left>right-half)?(left-half)/2:(right+half)/2;
            auto door=position(0,0),wall=position(wallU,0);
            if(!blocked(door.x,door.y,door.z)){fail("closed door not solid");return;}
            if(!blocked(wall.x,wall.y,wall.z)) {
                saida::Log::error("[World retail E2E] wall u=",wallU," edge=",r.plan.edge," floor=",r.plan.floor);
                std::vector<r1::MeshPart> snapshot;
                const auto& prepared=std::any_cast<const PreparedTile&>(owner->served->prepared);
                for(const auto& part:prepared.parts)if(part.prepared)
                    snapshot.push_back(generatorPart(part.name,part.prepared->geometry.vertices,part.prepared->geometry.indices));
                for(const auto& shell:prepared.buildingCollisions) {
                    std::vector<saida::Vertex> vertices(shell->positions.size());
                    for(size_t i=0;i<vertices.size();++i){vertices[i].pos=shell->positions[i];vertices[i].normal={0,1,0};}
                    snapshot.push_back(generatorPart("building collision",vertices,shell->indices));
                }
                const auto bytes=r1::writeGlb(snapshot);
                std::ofstream geometry(game/"generated/interior-smoke-failure.glb",std::ios::binary);
                geometry.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
                std::ofstream planFile(game/"generated/interior-smoke-failure.json");planFile<<r.plan.json().dump();
                fail("facade not solid");return;
            }
            // Query presence alone does not prove that the character solver
            // stops at a mesh. Try crossing both closed door and facade.
            auto cannotCross=[&](double u) {
                const glm::dvec3 saved(lon,lat,alt);auto from=position(u,-.65),target=position(u,.65);
                lon=from.x;lat=from.y;alt=from.z;
                auto delta=origin.local(ecef(target.x,target.y,alt))-origin.local(ecef(lon,lat,alt));
                const auto end=moveFeet(delta.x,-delta.z,.1);
                const auto local=owner->frame.local(ecef(end.x,end.y,end.z));
                const bool stopped=r.plan.local({local.x,local.z}).y<.15;
                lon=saved.x;lat=saved.y;alt=saved.z;
                feet->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+.06)));
                return stopped;
            };
            if(!cannotCross(0)||!cannotCross(wallU)){fail("character crossed closed door or facade");return;}
            auto* physics=engine.sceneTree().world().physics();
            for(size_t i=0;i<r.layout.fixtures.size();++i) {
                const auto& fixture=r.layout.fixtures[i];if(fixture.height<=.05)continue;
                auto* body=dynamic_cast<saida::CollisionObjectNode*>(r.node->findByPath(fixture.kind+" collider "+std::to_string(i)));
                if(!body||!body->physicsWorld()){fail("furniture collider not synchronized");return;}
                const auto at=r.plan.point(fixture.at.x,fixture.at.y);
                const glm::vec3 center(owner->node->worldTransform()*glm::vec4(float(at.x),float(r.plan.floor+fixture.height*.5),float(at.y),1));
                const auto hits=physics->overlapSphere(center,.1f,obstacleFilter());
                if(std::find(hits.begin(),hits.end(),body->bodyId())==hits.end()) {
                    saida::Log::error("[World retail E2E] fixture=",fixture.kind," index=",i," collider id=",body->bodyId().GetIndexAndSequenceNumber(),
                        " expected=",center.x,",",center.y,",",center.z);
                    fail("furniture's drawn center has no matching engine collider");return;
                }
            }
            if(!resident(-95))return;
            move(-95,false);retailTestPhase=4;return;
        }
        if(retailTestPhase==4) {
            if(r.node){fail("interior not released outside radius");return;}
            move(-3,false);retailTestPhase=5;return;
        }
        if(retailTestPhase==5&&r.node&&r.opening>.99) {
            saida::Log::info("[World retail E2E] PASS engine character entry, aisle, exit, door/wall/furniture colliders, eviction and regeneration");
            engine.sceneTree().quit();
        }
    }
    std::unique_ptr<saida::Node> clonePlant(saida::Node& source) {
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
    // A clone of a prototype with its empty nodes removed: every mesh it holds
    // comes back as one node carrying the transform it would have inherited.
    //
    // Uniform scale is required to compose nested TRS without introducing shear.
    static bool uniformScale(const glm::vec3& s) {
        return std::abs(s.x-s.y)<1e-6f&&std::abs(s.y-s.z)<1e-6f;
    }
    bool flattenable(saida::Node& node) const {
        if(!uniformScale(node.transform().scale))return false;
        for(auto& child:node.children())if(!flattenable(*child))return false;
        return true;
    }
    void collectMeshes(saida::Node& source,const saida::Transform& above,
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
    // Each shared model parsed once: instances share its mesh and material
    // pointers, avoiding thousands of repeated GLB and texture decodes.
    std::map<std::string,saida::AssetHandle> prototypeLoads;
    saida::Node* prototype(const std::string& path) {
        auto& found=naturePrototypes[path];
        if(!found) {
            auto& handle=prototypeLoads[path];
            if(!handle)handle=saida::GLTFLoader::request((game/path).string(),engine.resources());
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
    bool modelsReady(const json& doc) {
        bool ready=true;
        const auto path=doc.value("importedFrom",std::string());
        if(!path.empty()&&!prototype(path))ready=false;
        for(const auto& child:doc.value("children",json::array()))if(!modelsReady(child))ready=false;
        return ready;
    }
    saida::Node* prototypeForStartup(const std::string& path) {
        // Before the frame loop only: CPU decoding still uses the worker,
        // while GPU publication waits for the normal resource pump.
        for(;;) {
            if(auto* node=prototype(path))return node;
            engine.resources().assetLoader().pump();
            std::this_thread::yield();
        }
    }
    // Warm shared models through the engine worker while the map is on screen.
    std::vector<std::string> warmList;
    void warm() {
        if(warmList.empty())return;
        const std::string path=warmList.back();
        if(prototype(path))warmList.pop_back();
    }
    std::unique_ptr<saida::Node> plantNode(json doc) {
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
            if(node->findByPath("Mid"))lod->setLevels({{"Near", .1f}, {"Mid", .09f}, {"Far", 0.f}});
            else lod->setLevels({{"Near", .06f}, {"Far", 0.f}});
            // A level dissolves into the next over half a second instead of
            // swapping in one frame (Saida's screen-door cross-fade).
            lod->setCrossFade(.5f);
        }
        return node;
    }
    // `reach` is §5's detail band, 1 on foot and less at the wheel: the
    // radius a plant is drawn in shrinks with the speed it is passed at, which
    // is the cheapest of the plan's levers and the only one that costs nothing
    // when standing still.
    void updateNature(double reach) {
        SAIDA_PROFILE_SCOPE("World/Nature");
        for(auto* tiles:{&loaded,&incoming})for(auto& [key,t]:*tiles){
            const auto pos=t.frame.local(ecef(lon,lat,alt));
            for(Plant& plant:t.vegetation){
                if(plant.tree&&!debugTrees)continue;
                const auto p=plant.node->transform().position;
                double d=std::hypot(p.x-pos.x,p.z-pos.z);
                const double maxDist=plant.grass?(toasterMode?35.:65.):(toasterMode?280.:550.);
                plant.node->setVisible(d<maxDist*reach);
                if(plant.lod) {
                    // Entered inside the radius, left a tenth beyond it, so
                    // a tree at the edge does not flicker between two models.
                    plant.close=d<kTreeNearRadius*(plant.close?1.1:1.);
                    plant.lod->setFinestLevel(toasterMode?1:(plant.close?0:1));
                }
            }
        }
    }
    // ── landmarks seen from afar ─────────────────────────────────────────────
    //
    // A landmark's full model belongs to its tile, and its tile is resident
    // only within a few hundred metres (gen/landmarks.cpp). Beyond that the
    // shipped list (assets/world/landmarks/landmarks.json) gives two lighter
    // levels and the ground they stand on: level 1 across a district, level 2
    // across a city, nothing past the haze. A far model is shown exactly when
    // the near one is not -- its tile is not resident, or has not streamed
    // it yet -- so the two are never drawn together and never both missing.
    struct FarLevel { std::string path; double until=0; size_t vertices=0; };
    struct FarLandmark {
        std::string slug,tile; double lon=0,lat=0,alt=0;
        std::vector<FarLevel> levels;
        saida::Node* node=nullptr; int shown=-1,requested=-1;
        saida::AssetHandle loading;
        // Where the near model sits in its tile's prop list, found once per mount.
        const saida::Node* seenTile=nullptr; long nearIndex=-1;
    };
    std::vector<FarLandmark> farLandmarks;
    // Levels 1 and 2 of every landmark, from the list shipped with its models
    // (assets/world/landmarks/landmarks.json, read by the generator).
    void readFarLandmarks() {
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
    bool nearModel(FarLandmark& f) {
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
    void placeFar(FarLandmark& f) {
        const auto position=glm::vec3(origin.local(ecef(f.lon,f.lat,f.alt)));
        const Frame own(f.lon,f.lat,f.alt);
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*f.node,position,rotation);
    }
    size_t farVertices() const {
        size_t n=0;
        for(const auto& f:farLandmarks)if(f.node&&f.shown>=0)n+=f.levels[size_t(f.shown)].vertices;
        return n;
    }
    void updateFar() {
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
    // ── the pack to the horizon ─────────────────────────────────────────────
    //
    // The streamed tiles end some 600 m out; on the sea ice the horizon is
    // 4.7 km away at eye height, and on a clear Arctic day the air hides
    // nothing short of it. Past the tiles the pack goes on (gen/seaice.cpp's
    // far pack): snow or water as the observation says, curved with the
    // Earth, rebuilt when the player has walked 4 km from its centre or the
    // tile under him was cooked from a newer reading.
    struct FarPack {
        saida::Node* node=nullptr; double lon=0,lat=0; size_t vertices=0;
        std::shared_ptr<const r1::SeaIce> ice;
        bool refused=false;
    } farPack,nextFarPack;
    void placeFarPack() {
      for(auto* pack:{&farPack,&nextFarPack}) {
        if(!pack->node)continue;
        const auto position=glm::vec3(origin.local(ecef(pack->lon,pack->lat,0.)));
        const Frame own(pack->lon,pack->lat,0.);
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*pack->node,position,rotation);
      }
    }
    void updateFarPack() {
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
    // ── the relief to the horizon ─────────────────────────────────────────────
    //
    // Past the streamed tiles the engine draws nested rings of terrain
    // (Saida's TerrainRingsNode): nine rings of 128 cells, 16 m to 4 km, out to
    // 262 km. Fine rings read Terrain Tiles; from 256 m they read the installed
    // relief without network. Both fall back to local observations, sampled
    // on a thread of their own (gen/far_relief); a ring is sampled again when
    // the player has moved two of its cells. The resident tiles are holes in
    // the rings, so the two never overlap and never leave a gap. The images
    // are kept on disk: a place visited sees its horizon offline.
    static constexpr double kFarBaseSpacing=16.0;
    static constexpr int kFarLevels=9;
    static constexpr double kFarReanchor=20000.0;  // metres before the rings' tangent frame moves
    static constexpr auto kFarRetry=std::chrono::seconds(60);
    struct FarJob {
        std::mutex lock;
        std::vector<std::pair<int,r1::FarLevel>> done;
        std::vector<std::string> said;
        bool busy=false;
    };
    struct FarRelief {
        saida::TerrainRingsNode* node=nullptr;
        double lon=0,lat=0; r1::Anchor anchor;
        std::shared_ptr<r1::FarLayers> layers;
        uint64_t layersSeen=0,generation=0;
        std::array<glm::dvec2,saida::TerrainRingsNode::kMaxLevels> origins{};
        std::array<bool,saida::TerrainRingsNode::kMaxLevels> asked{};
        std::array<std::chrono::steady_clock::time_point,saida::TerrainRingsNode::kMaxLevels> retryAt{};
        std::shared_ptr<FarJob> job;
        double reach=0;
    } farRelief;
    void placeFarRelief() {
        if(!farRelief.node)return;
        const auto position=glm::vec3(origin.local(ecef(farRelief.lon,farRelief.lat,0.)));
        const Frame own(farRelief.lon,farRelief.lat,0.);
        const auto rotation=glm::quat_cast(glm::mat3(glm::transpose(origin.basis)*own.basis));
        engine.sceneTree().world().rebaseSubtreeTo(*farRelief.node,position,rotation);
    }
    void updateFarRelief() {
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
        if(!farRelief.node->visible())farRelief.node->setVisible(true);
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
    // The trunk of a tree that has just been mounted. The compound is rebuilt
    // at the next physics sync, once however many trees joined it this frame.
    void addTrunk(Loaded& l,const json& at) {
        if(!l.trunks) {
            auto body=std::make_unique<saida::StaticBodyNode>();body->setName("Tree trunks");
            l.trunks=static_cast<saida::StaticBodyNode*>(l.node->addChild(std::move(body)));
            l.trunks->setEnabled(debugTrees);
        }
        auto shape=std::make_unique<saida::CollisionShapeNode>();
        shape->shapeType=saida::CollisionShapeType::Box;shape->halfExtents={.23f,2.f,.23f};
        shape->offset={at[0].get<float>(),at[1].get<float>()+2.f,at[2].get<float>()};
        l.trunks->addChild(std::move(shape));
        l.trunks->markDirty();
    }
    // `roadside`: only what is read from the road -- the signs, which the
    // tile lists before its furniture -- for a driver above 15 km/h.
    void streamProps(bool roadside=false) {
        SAIDA_PROFILE_SCOPE("World/StreamProps");
        const auto start=std::chrono::steady_clock::now();
        // Scene/GPU APIs stay on the render thread. Yield between objects;
        // CPU model requests yield; node instantiation retains a soft budget.
        // A replacement gets its props while hidden, so its trees and signs
        // are there the frame it is shown (World::promoteIncoming).
        for(auto t:ring)for(auto* tiles:{&loaded,&incoming}) {
            auto it=tiles->find(t.key());if(it==tiles->end())continue;
            auto& l=it->second;
            try { mountLettering(l); }
            catch(const std::exception& e) {
                l.nextLettering=l.data.value("lettering",json::array()).size();
                saida::Log::error("[World lettering] ",t.key(),": ",e.what());
                text("stream-status","A sign could not be loaded.");
                if(smoke){testFailed=true;engine.sceneTree().quit();return;}
            }
            if(msSince(start)>=kPropImportMs)return;
            while(l.nextProp<l.props.size()) {
                if(roadside) {
                    const auto& doc=l.props[l.nextProp];
                    if(!doc.contains("groups")||std::find(doc["groups"].begin(),doc["groups"].end(),"roadside")==doc["groups"].end())break;
                }
                try {
                    auto doc=l.props[l.nextProp];
                    bool plant=doc.contains("groups")&&std::find(doc["groups"].begin(),doc["groups"].end(),"vegetation")!=doc["groups"].end();
                    if(!modelsReady(doc))return;
                    auto n=plantNode(doc);
                    if(!n)throw std::runtime_error("Object import failed");
                    if(auto* water=dynamic_cast<saida::WaterNode*>(n.get())) {
                        l.waters.push_back(water);water->setEnabled(debugWater);
                    }
                    if(plant) {
                        Plant entry;
                        entry.node=n.get();
                        entry.grass=n->isInGroup("grass");
                        entry.tree=n->isInGroup("tree");
                        if(entry.tree)n->setEnabled(debugTrees);
                        entry.lod=n->getBehaviour<saida::LODGroupBehaviour>();
                        if(toasterMode&&entry.lod)entry.lod->setFinestLevel(1);
                        l.vegetation.push_back(entry);
                        n->setVisible(false);
                    }
                    if(doc.contains("groups")&&std::find(doc["groups"].begin(),doc["groups"].end(),"tree")!=doc["groups"].end())
                        addTrunk(l,doc.at("transform").at("position"));
                    if(n->isInGroup("landmark")) {
                        auto body=std::make_unique<saida::StaticBodyNode>();body->setName(n->name()+" collider");
                        auto shape=std::make_unique<saida::CollisionShapeNode>();shape->shapeType=saida::CollisionShapeType::Mesh;
                        body->addChild(std::move(shape));body->addChild(std::move(n));l.node->addChild(std::move(body));
                    } else l.node->addChild(std::move(n));
                } catch(const std::exception& e) {
                    saida::Log::warn("[World streaming] prop failed in ",t.key(),": ",e.what());
                    if(smoke){testFailed=true;engine.sceneTree().quit();return;}
                }
                ++l.nextProp;
                if(msSince(start)>=kPropImportMs)return;
            }
        }
    }
    // The nodes the parts go up under (World::uploadParts).
    void shape(Loaded& tile) {
        tile.geography=tile.node->findByPath("Geography");
        tile.grass=dynamic_cast<saida::GrassNode*>(tile.node->findByPath("Grass"));
        for(const auto& child:tile.node->children())
            if(auto* water=dynamic_cast<saida::WaterNode*>(child.get()))tile.waters.push_back(water);
        const auto& prepared=std::any_cast<const PreparedTile&>(tile.served->prepared);
        if(tile.geography)for(size_t i=0;i<prepared.buildingCollisions.size();++i) {
            auto body=std::make_unique<saida::StaticBodyNode>();
            body->setName("Building shell collider "+std::to_string(i));
            auto collision=std::make_unique<saida::CollisionShapeNode>();
            collision->setTriangleMeshData(prepared.buildingCollisions[i]);
            body->addChild(std::move(collision));tile.geography->addChild(std::move(body));
        }
        tile.reducedDensity=densityPolicy.reduced(tile.served->cooked.tile)||
            tile.data.at("vertices").get<size_t>()>r1::DensityPolicy::kThreshold;
        const char* names[]={"Near","Mid","Far"};
        tile.buildings.reserve(tile.served->cooked.buildings.size());
        tile.visualForFootprint.assign(tile.data.at("footprints").size(),-1);
        for(const auto& source:tile.served->cooked.buildings) {
            Loaded::Building b;
            b.node=tile.node->createChild<saida::Node>("Building "+std::to_string(source.id));
            for(size_t level=0;level<3;++level) {
                b.levels[level].root=b.node->createChild<saida::Node>(names[level]);
                b.levels[level].root->setVisible(false);
            }
            b.lod=b.node->addBehaviour<saida::LODGroupBehaviour>();
            b.lod->setLevels({{"Near",.08f},{"Mid",.008f},{"Far",0.f}});
            b.lod->setBounds({{float(source.low.x),float(source.low.y),float(source.low.z)},
                              {float(source.high.x),float(source.high.y),float(source.high.z)}});
            b.lod->setCrossFade(.3f);
            for(int level=0;level<3;++level)b.lod->setLevelReady(level,false);
            if(source.footprint<tile.visualForFootprint.size())tile.visualForFootprint[source.footprint]=int(tile.buildings.size());
            tile.buildings.push_back(std::move(b));
        }
    }
    // Everything a tile in `loaded` does beyond being drawn: its density, its
    // traffic and crowd, its aircraft. After the emplace, never before: the
    // flow holds a pointer to the graph, and a graph built in a temporary
    // would be moved out from under it.
    void activate(const std::string& key,Loaded& tile) {
        const r1::Tile gen=tile.served->cooked.tile;
        // The tile's own geometry, the one measure the budget is about.
        // Props are scene nodes on shared models (CLAUDE.md rule 5):
        // counting every visible tree's Near mesh put a dozen trees over
        // 120 000 and sent every tile's trees to cards for good.
        densityPolicy.observe(gen,tile.data.at("vertices").get<size_t>());
        tile.reducedDensity=densityPolicy.reduced(gen);
        if(tile.reducedDensity)saida::Log::info("[World density] ",key," crowd=1/4 interiors=blocked");
        ++minimapRevision;
        checkStanding=true;
        tile.footprints=std::any_cast<const PreparedTile&>(tile.served->prepared).footprints;
        unpack(tile);
        readGraph(tile);
        readCrowd(tile);
        mountAircraft(tile);
        placeTiles();
        reparkIfCovered(key);
    }
    // A replacement is shown once every part of its ground is draw-ready and
    // solid, in the frame its predecessor is taken down: the street is never
    // missing in between. Replacements go up together -- a tile's ground is
    // joined to its neighbours' (gen/seams), so one swapped alone would meet
    // the old version of the next one with a step -- unless the first ready
    // has waited a second for the others. One that cannot get ready says why
    // and is shown as it is rather than leaving the old version up for good.
    static constexpr double kReplaceWaitMs=10000.,kBatchWaitMs=1000.;
    bool replacementReady(const std::string& key,const Loaded& next) {
        const auto& parts=std::any_cast<const PreparedTile&>(next.served->prepared).parts;
        bool ready=(!next.geography||next.nextPart>=parts.size())&&buildingsReady(next);
        for(auto* mesh:next.partMeshes)ready=ready&&mesh->loaded();
        // As many of its props as the shown version has, or all of them.
        const auto old=loaded.find(key);
        const size_t letters=next.data.value("lettering",json::array()).size();
        if(old!=loaded.end())
            ready=ready&&next.nextProp>=std::min(old->second.nextProp,next.props.size())&&
                  next.nextLettering>=std::min(old->second.nextLettering,letters);
        auto* physics=engine.sceneTree().world().physics();
        if(ready&&next.geography&&(!physics||physics->hasRoomForBody()))
            for(const auto& child:next.geography->children())
                if(auto* body=dynamic_cast<saida::CollisionObjectNode*>(child.get()))
                    ready=ready&&!body->bodyId().IsInvalid();
        return ready;
    }
    bool promoteIncoming() {
        std::vector<std::string> due;bool holding=false;double longest=0;
        for(const auto& [key,next]:incoming) {
            const double waited=msSince(next.builtAt);
            if(replacementReady(key,next)){due.push_back(key);longest=std::max(longest,waited);}
            else if(waited>=kReplaceWaitMs) {
                // A timed-out replacement is due too: otherwise a younger
                // unready tile holds the batch forever and repeats this warning.
                longest=std::max(longest,waited);
                const auto& parts=std::any_cast<const PreparedTile&>(next.served->prepared).parts;
                saida::Log::warn("[World streaming] ",key," replacement not draw-ready after ",
                                 int(waited)," ms (parts ",next.nextPart,"/",parts.size(),", props ",
                                 next.nextProp,"/",next.props.size(),"): shown as it is");
                due.push_back(key);
            } else holding=true;
        }
        if(due.empty()||(holding&&longest<kBatchWaitMs))return false;
        for(const auto& key:due) {
            const double waited=msSince(incoming.at(key).builtAt);
            auto old=loaded.extract(key);
            auto handle=incoming.extract(key);
            handle.mapped().node->setVisible(true);
            Loaded& tile=loaded.insert(std::move(handle)).position->second;
            activate(key,tile);
            if(!old.empty()) {
                auto& source=old.mapped();
                for(size_t i=0;i<source.cars.size();++i)if(source.cars[i]&&trafficSlotLive(source,i))
                    if(!transferTrafficCar(source,i,tile))preserveTrafficCar(source,i);
                source.node->queueFree();
            }
            saida::Log::info("[World streaming] replaced ",key," after ",int(waited)," ms cook_ms=",
                             tile.served->cooked.cookMs);
        }
        return true;
    }
    void stream() {
        SAIDA_PROFILE_SCOPE("World/StreamTiles");
        double x=(pending||warming)?pickLon:lon,y=(pending||warming)?pickLat:lat;
        auto want=orderedNearby(x,y);std::set<std::string> wanted;for(auto t:want)wanted.insert(t.key());
        std::set<std::string> keep=wanted;
        // Retain the current tile during an in-progress teleport, so the player
        // does not stand on nothing while the destination cooks. It is kept out
        // of `wanted` on purpose: it is leaving, and charging the destination's
        // budget for it is what made a Paris -> Amsterdam teleport refuse its
        // own ninth tile. The arena has the room -- 841 k for the densest
        // neighbourhood, 107 k for the tile being left, 9 k for every prop
        // model on the planet, inside 1 048 576.
        if(playing)keep.insert(tileAt(lon,lat).key());
        if(playing&&!pending&&!warming)for(const auto& [key,region]:loaded) {
            bool visible=false;
            for(const auto& agent:region.flow.agents())if(agent.alive&&trafficVisible(region,region.flow.pose(agent))){visible=true;break;}
            if(visible)keep.insert(key);
        }
        bool removed=false;
        auto stale=[&](const Loaded& t){
            const auto current=service->find(t.served->cooked.tile);
            return current&&current->serial!=t.served->serial;
        };
        for(auto* tiles:{&loaded,&incoming})for(auto it=tiles->begin();it!=tiles->end();) {
            // A replacement cooked over again is dropped and started anew below.
            if(!keep.count(it->first)||(tiles==&incoming&&stale(it->second)))
                {it->second.node->queueFree();it=tiles->erase(it);removed=true;}else ++it;
        }
        if(promoteIncoming())removed=true;
        size_t count=0,indices=0;
        for(auto* tiles:{&loaded,&incoming})for(auto& [k,t]:*tiles) {
            if(wanted.count(k)||(!pending&&!warming&&keep.count(k))) {count+=residentVertices(t.data);indices+=residentIndices(t.data);}
            // Closer representations share the same arena. Include even the
            // departing tile's details until its Far transition releases them.
            const auto [extraV,extraI]=buildingExtraGeometry(t);count+=extraV;indices+=extraI;
        }
        // The far landmarks share the arena with the tiles (gen/landmarks.cpp).
        count+=farVertices()+farPack.vertices+nextFarPack.vertices;
        bool started=false;
        for(auto it=loaded.begin();it!=loaded.end();) {
            // A tile the service cooked again -- offline first, then from real
            // observations -- is replaced. Its new version goes up hidden while
            // the old one is still drawn: taking the old one down first left
            // its streets missing for the frames the new parts took to upload.
            if(!stale(it->second)||incoming.count(it->first)){++it;continue;}
            const auto current=service->find(it->second.served->cooked.tile);
            const json& data=current->cooked.manifest;
            const size_t vertices=residentVertices(data),tileIndices=residentIndices(data);
            if(count+vertices<=residentVertexBudget&&indices+tileIndices<=residentIndexBudget) {
                try {
                    auto* ptr=engine.sceneTree().world().addChild(buildTile(it->first,*current));
                    ptr->setVisible(false);
                    shape(incoming.try_emplace(it->first,ptr,current).first->second);
                    count+=vertices;indices+=tileIndices;
                    started=true;++it;
                } catch(const std::exception& e) {
                    // The mount below meets the same refusal and reports it.
                    saida::Log::warn("[World streaming] replacement of ",it->first," refused: ",e.what());
                    it->second.node->queueFree();it=loaded.erase(it);removed=true;
                }
            } else {
                // Both versions do not fit side by side: the old one goes first
                // and the tile is missing until the new one is up. Said, since
                // it is seen.
                saida::Log::info("[World streaming] ",it->first," replaced in place: ",count," + ",vertices,
                                 " of ",residentVertexBudget," resident vertices, ",indices," + ",tileIndices,
                                 " of ",residentIndexBudget," indices");
                if(wanted.count(it->first)) {
                    const auto& old=it->second.data;
                    count-=residentVertices(old);indices-=residentIndices(old);
                }
                const auto [extraV,extraI]=buildingExtraGeometry(it->second);count-=extraV;indices-=extraI;
                it->second.node->queueFree();it=loaded.erase(it);removed=true;
            }
        }
        if(removed){++minimapRevision;trim();}
        if(started)placeTiles();
        const auto tick=std::chrono::steady_clock::now();
        for(auto t:want) {
            if(loaded.count(t.key()))continue;
            auto served=service->find(t.gen());
            if(!served)continue;
            const json& data=served->cooked.manifest;
            try {
                const size_t vertices=residentVertices(data),tileIndices=residentIndices(data);
                if(count+vertices>residentVertexBudget||indices+tileIndices>residentIndexBudget) {
                    text("stream-status","Detail limit reached in this area.");
                    if(refused.empty()) {
                        refused=t.key();
                        // Loud on purpose. A skipped tile used to be a UI string
                        // and nothing else, so a neighbourhood that did not fit
                        // looked exactly like a slow download -- the worst shape
                        // a failure can take (CLAUDE.md rule 3).
                        saida::Log::warn("[World] ",t.key()," does not fit: ",count,
                                         " + ",vertices," of ",residentVertexBudget," resident vertices, ",
                                         indices," + ",tileIndices," of ",residentIndexBudget," indices");
                    }
                    continue;
                }
                const auto mountStarted=std::chrono::steady_clock::now();
                auto* ptr=engine.sceneTree().world().addChild(buildTile(t.key(),*served));
                auto [entry,inserted]=loaded.try_emplace(t.key(),ptr,served);
                shape(entry->second);
                // Counted at once: several tiles mount in one tick, and each
                // must see the ones mounted before it.
                count+=vertices;indices+=tileIndices;
                activate(t.key(),entry->second);
                lastMountMs=msSince(mountStarted);
                const auto arena=engine.resources().geometryUsage();
                saida::Log::info("[World streaming] mounted ",t.key()," mount_ms=",lastMountMs,
                                 " arena=",arena.vertices,"v/",arena.indices,"i (largest free ",
                                 arena.largestFreeVertices,"v/",arena.largestFreeIndices,"i)",
                                 " cook_ms=",served->cooked.cookMs," since_go_ms=",
                                 msSince(goStarted)," private_mb=",int(privateMegabytes()));
            }catch(const std::exception& e){
                text("status",std::string("Tile unavailable: ")+e.what());
                saida::Log::error("[World streaming] mount failed for ",t.key(),": ",e.what());
                if(smoke){testFailed=true;engine.sceneTree().quit();return;}
            }
            // Mounting is cheap now that the parts go up over the next frames;
            // several tiles fit in one tick, as long as the tick stays short.
            if(msSince(tick)>=kMountTickMs)break;
        }
        bool surroundingReady=std::all_of(want.begin(),want.end(),[&](Tile t){return loaded.count(t.key())>0;});
        if(pending && refused==tileAt(pickLon,pickLat).key() && !smoke) {
            // Not transient: the arena is a fixed size and this neighbourhood
            // does not fit in it. Saying so and handing the map back is a worse
            // outcome than spawning and a far better one than a counter that
            // never moves -- and it is the truth (§3 I5).
            pending=false;warming=false;showMap(true);
            text("status","This neighbourhood exceeds the available geometry memory. Pick a nearby spot.");
            return;
        }
        auto ready=[&](double x,double y){auto* t=tile(x,y);if(!t)return false;
            if(t->geography&&t->nextPart<std::any_cast<const PreparedTile&>(t->served->prepared).parts.size())return false;
            if(!buildingsReady(*t))return false;
            for(auto* mesh:t->partMeshes)if(!mesh->loaded())return false;
            return !t->geography||(!t->geography->children().empty()&&
                !static_cast<saida::CollisionObjectNode*>(t->geography->children().back().get())->bodyId().IsInvalid());};
        if(pending && ready(pickLon,pickLat)) {
            const bool waterSpawn=deepWater(pickLon,pickLat);
            double x0=pickLon,y0=pickLat;bool found=waterSpawn||!footBlocked(x0,y0);
            for(int i=1;!found&&i<=160;++i) {
                double a=i*2.39996323,d=2.*std::sqrt(double(i));auto q=advance(pickLon,pickLat,d*cos(a),d*sin(a));
                if(ready(q.x,q.y)&&!blocked(q.x,q.y)){x0=q.x;y0=q.y;found=true;}
            }
            if(!found) {
                // A safe point can lie just across the tile boundary. Only
                // that exceptional spawn waits for the rest of the search area.
                if(!surroundingReady && refused.empty())return;
                pending=false;
                text("status","This point is inside a building. Pick a nearby street.");
                // Third time this shape of bug appears in this file, so it is
                // written down rather than rediscovered: a spawn that is
                // *refused* used to leave the E2E waiting for one that would
                // never come, and report it three minutes later as a data
                // timeout. The refusal is the answer; it has to be said out
                // loud, and the test has to fail on the reason.
                saida::Log::warn("[World] no standable ground within 25 m of ",
                                 pickLon,", ",pickLat," (buildings)");
                if(smoke) {
                    saida::Log::error("[World E2E] FAIL spawn refused: inside a building");
                    testFailed=true;engine.sceneTree().quit();
                }
                return;
            }
            lon=x0;lat=y0;alt=waterSpawn?waterLevel(lon,lat):height(lon,lat);
            conditions=json::object();conditionsDoc.reset();observedWeather=Weather{};clearRain();
            origin=Frame(lon,lat,alt);placeTiles();moveSun();
            jumpOffset=jumpVelocity=0;followDistance=kOnFootFollow;wasJump=false;
            // Teleporting leaves the current vehicle. A water arrival starts
            // swimming at the selected coordinate, including in the open sea.
            // Carry the vehicle's view into the walking heading, just as a
            // normal exit does: the camera and movement must share one yaw.
            yaw=wrap(yaw+lookYaw);lookYaw=0;lookIdle=0;
            dismountPlayerFromBike();
            driving=false;swimming=waterSpawn;swimTime=0;swimLean=0;swimHeading=yaw;
            clearBoats();
            clearAircraft();
            player->setEnabled(true);
            player->transform().rotation=glm::angleAxis(float(-yaw*rad),glm::vec3(0,1,0));
            parkCar();
            playing=true;pending=false;warming=false;showMap(false);request(lon,lat);
            saida::Log::info("[World] spawned ",lon,", ",lat," altitude=",alt,
                             waterSpawn?" swimming":onWater(lon,lat)?" on foot, wading in "+
                                 std::to_string(waterLevel(lon,lat)-alt)+" m of water":" on foot",
                             tile(lon,lat)->data.value("provisional",false)?" (ground first, OSM on its way)"
                             :tile(lon,lat)->data.value("offlineApproximation",false)
                                 ?" (simplified offline terrain)":"");
            saida::Log::info("[World streaming] go_to_play_ms=",
                msSince(goStarted),
                " resident=",loaded.size()," target=",want.size());
            if(smoke){
                if(smokeArrivalHeading) {
                    if(std::abs(wrap(yaw-*smokeArrivalHeading))>1e-6||lookYaw!=0||lookIdle!=0) {
                        saida::Log::error("[World E2E] FAIL teleport retained a separate vehicle view heading");
                        testFailed=true;engine.sceneTree().quit();return;
                    }
                    smokeArrivalHeading.reset();
                    saida::Log::info("[World E2E] PASS vehicle side view becomes the walking heading after teleport");
                }
                // A test driver must choose a free direction, like a human:
                // walking straight into the nearest real wall is not a failure.
                double best=0;
                for(int direction=0;direction<32;++direction){
                    double heading=direction*11.25,clear=clearAhead(lon,lat,alt,heading,.5,12.);
                    if(clear>best){best=clear;yaw=heading;}
                }
                if(!tellSun()){saida::Log::error("[World E2E] FAIL the sun cycle did not take the observer");testFailed=true;engine.sceneTree().quit();return;}
                saida::Log::info("[World E2E] sun follows the observer");
                smokeWaterSpawn=waterSpawn&&!smokeSail&&worldCapture.pngPath.empty();
                smokeStarted=!smokeSail&&!smokeFly&&!waterSpawn;
                smokeSailWait=smokeSail;smokeSailTime=0;
                smokeFlyWait=smokeFly;smokeFlyTime=0;smokeFlyPhase=0;
                smokeWalk=0;smokeStart=ecef(lon,lat,alt);smokeRan=smokeJumped=false;
                saida::Log::info("[World E2E] spawn complete with ",loaded.size()," tiles resident");
            }
        }
        if(pending) {
            const auto status=service->status();
            if(status.outOfMemory)text("status","Memory full: loading slowed, retrying automatically. Closing other applications helps.");
            else if(status.memoryPaused)text("status","Loading is waiting for free memory.");
            else if(!status.error.empty())text("status","Loading error, retrying automatically. "+status.error);
            else if(status.offline)text("status","A source is not responding: showing the data already received.");
            else text("status",kPreparing);
        }
    }
public:
    World(saida::Engine& e,fs::path g,bool test,saida::CaptureRequest capture,double startLon,double startLat,bool hop,double hopX,double hopY,saida::runtime::CaptureViewpoint view,bool sail=false,bool fly=false):engine(e),game(g),smokeSail(sail),smokeFly(fly),pickLon(startLon),pickLat(startLat),mapCenterLon(startLon),mapCenterLat(startLat),smoke(test),hopLon(hopX),hopLat(hopY),hopWanted(hop),worldCapture(capture),captureView(view) {
        performanceSmoke=smoke&&std::getenv("R1WORLD_DEBUG_SMOKE");
        if(!perfSmoke.empty()&&(!smoke||(perfSmoke!="walk"&&perfSmoke!="drive")))
            throw std::runtime_error("R1WORLD_PERF_SMOKE requires --smoke and walk or drive");
        if(qualitySwitchSmoke&&(!smoke||perfSmoke!="walk"))
            throw std::runtime_error("R1WORLD_QUALITY_SWITCH_SMOKE requires --smoke and R1WORLD_PERF_SMOKE=walk");
        residentVertexBudget=size_t(double(e.resources().geometryCapacity().vertices)*kTileGeometryShare);
        residentIndexBudget=size_t(double(e.resources().geometryCapacity().indices)*kTileGeometryShare);
        ui=dynamic_cast<saida::WebCanvasNode*>(e.sceneTree().firstInGroup("world-ui"));
        minimapUi=dynamic_cast<saida::WebCanvasNode*>(e.sceneTree().firstInGroup("minimap-ui"));
        camera=dynamic_cast<saida::CameraNode*>(e.sceneTree().firstInGroup("camera"));
        if(!ui||!minimapUi||!camera)throw std::runtime_error("Earth scene is missing its camera, map or minimap");
        minimapUi->setEnabled(false);
        {
            std::ifstream input(game/"assets/world/timezone-countries.json");
            if(!input)throw std::runtime_error("Missing assets/world/timezone-countries.json");
            const json places=json::parse(input);
            zoneCountries=places.at("zones").get<std::map<std::string,std::string>>();
            countryNames=places.at("countries").get<std::map<std::string,std::string>>();
        }
        loadOptions();
        engine.window().size(standardWidth,standardHeight);
        if(toasterMode)applyToasterResolution();
        player=e.sceneTree().firstInGroup("player");
        if(!player)throw std::runtime_error("Earth scene is missing the player");
        player->findBehavioursInChildren(animators);
        if(animators.empty())throw std::runtime_error("Player model has no Animator");
        loadHumans();
        // The body is drawn at the people's scale; its height is the manifest's.
        for(auto& child:player->children())child->transform().scale=glm::vec3(float(humanScale));
        unifyPlayerBody();
        for(auto* a:animators) {
            for(auto clip:{"idle","run","sprint","jump"})
                if(!a->clips().count(clip))throw std::runtime_error(std::string("Player animation missing: ")+clip);
            a->play("idle");
            if(!a->rig())throw std::runtime_error("Player model has no skeleton");
            playerImpacts.push_back(a->addModifier<saida::ImpactModifier>(*a->rig(),impactSettings()));
            if(!playerImpacts.back()->valid())throw std::runtime_error("Player skeleton lacks the spine the stagger bends");
        }
        player->setEnabled(false);
        saida::Log::info("[World player] model ready, animators=",animators.size());
        setupCombat();
        car=e.sceneTree().firstInGroup("vehicle");
        if(!car)throw std::runtime_error("Earth scene is missing the player's car");
        car->setEnabled(false);
        for(auto& child:car->children())child->transform().scale=glm::vec3(float(kVehicleScale));
        // The kit names its four wheels, and the names are the contract: a
        // re-export that renamed them would leave a car sliding on frozen
        // wheels, which is a silent regression. It is said once, out loud.
        collectWheels(*car);
        saida::Log::info("[World car] model ready, wheels=",frontWheels.size()+rearWheels.size());
        // His feet among people: the engine's character body, a capsule as
        // wide as theirs and as tall as him, its position where he stands.
        feet=e.sceneTree().world().createChild<saida::CharacterBodyNode>();
        feet->setName("player-feet");
        {
            auto capsule=std::make_unique<saida::CollisionShapeNode>();
            capsule->shapeType=saida::CollisionShapeType::Capsule;
            capsule->radius=float(r1::Crowd::kBodyRadius);
            capsule->height=float(playerHeight);
            capsule->axis=1;
            capsule->offset=glm::vec3(0.f,float(playerHeight*.5),0.f);
            feet->addChild(std::move(capsule));
        }
        feet->setEnabled(false);
        prototypes=e.sceneTree().world().createChild<saida::Node>("Shared prototypes");
        prototypes->setEnabled(false);
        loadPaints();
        buildTrafficPrototype();vehicleCollider(*car,vehicleSpec(*car));
        buildAircraftPrototypes();
        saida::Log::info("[World traffic] ready, ",paints.size()," paints");
        // Textures resident at once. A city neighbourhood shows about fifteen
        // photographed materials (ground, streets, walls, roofs; see
        // assets/textures/surfaces.json), some 90 MB, on top of what 256 MB already held.
        // The reference MX450 has 2 GiB of VRAM (plan §3 I4). This 512 MiB
        // resource budget leaves space for the roughly 50 MiB geometry arena,
        // render targets and other GPU allocations; total residency is profiled.
        e.resources().setGpuBudget(512ull*1024*1024);
        r1::WorldService::Options options;
        options.gameRoot=game.string();
        // Nine visible tiles plus three forecast tiles. The former 36-tile
        // cache retained gigabytes of prepared Paris geometry off screen.
        options.keep=12;
        options.availableMemory=[] { return saida::MemoryProfiler::availableHostBytes(); };
        // Nine tiles can be resident together. Leave 5% of their share for
        // landmarks that remain visible across a tile boundary.
        options.tileVertexTarget=std::min(r1::kTileVertexBudget,residentVertexBudget*95/900);
        options.enrichRetail=true;
        options.fetchCanopy=true;
        options.prepare=[](r1::ServedTile& tile){tile.prepared=prepareTile(tile.cooked);releaseCookedGeometry(tile.cooked);};
        options.log=[](const std::string& line){saida::Log::info("[World service] ",line);};
        service=std::make_unique<r1::WorldService>(std::move(options));
        // Nearest first to be needed last: warm() takes from the back.
        for(const auto& b:r1::palette().boats)warmList.push_back("assets/models/external/kenney_boats/"+b.model+".glb");
        for(const auto& k:r1::palette().props)for(const auto& m:k.models)warmList.push_back(m);
        for(const auto& [code,sign]:r1::palette().signs)for(const auto& [mount,m]:sign.mounts)warmList.push_back(m);
        for(const auto* kits:{&r1::palette().townSigns,&r1::palette().streetSigns})for(const auto& [country,kit]:*kits) {
            for(const auto& m:{kit.post,kit.bar})if(!m.empty())warmList.push_back(m);
            for(const auto& [lines,p]:kit.plates)for(const auto& m:{p.left,p.middle,p.right})warmList.push_back(m);
            for(const auto& [ch,g]:kit.glyphs)warmList.push_back(g.first);
        }
        for(const char* dir:{"assets/models/external/nature_selected","assets/models/external/nature_cards"}) {
            std::error_code ec;
            for(const auto& f:fs::directory_iterator(game/dir,ec))
                if(f.path().extension()==".glb")warmList.push_back(std::string(dir)+"/"+f.path().filename().string());
        }
        std::sort(warmList.begin(),warmList.end());
        warmList.erase(std::unique(warmList.begin(),warmList.end()),warmList.end());
        sea=std::make_unique<r1::SeaService>(game.string(),[](const std::string& line){saida::Log::info("[World sea] ",line);});
        sky=std::make_unique<r1::ConditionsService>(game.string(),[](const std::string& line){saida::Log::info("[World sky] ",line);});
        readFarLandmarks();
        e.window().setCursorCaptured(false);
    }
    ~World(){for(auto* e:listeners)if(ui->isLiveElement(e,generation)){
        for(const char* type:{"click","mousedown","mouseup","mousemove","mousescroll","dblclick"})e->RemoveEventListener(type,this);}}
    void setMenuGlobe(std::shared_ptr<r1::MenuGlobeState> model) {
        globe=std::move(model);globeTarget=globe->view;
    }
    bool failed() const {return testFailed;}
    void inspectInstant(double unixSeconds) {inspectAt=unixSeconds;}
    void inspectConditions(std::array<double,3> conditions) {
        if(!inspectAt||worldCapture.pngPath.empty())throw std::runtime_error("--weather requires --at and --screenshot");
        inspectWeather=conditions;
    }
    void inspectHeat(double temperature) {
        if(!inspectAt||worldCapture.pngPath.empty()||!std::isfinite(temperature)||temperature < -100||temperature>65)
            throw std::runtime_error("--temperature requires --at, --screenshot and a finite temperature in [-100,65]");
        inspectTemperature=temperature;
    }
    void captureFromAltitude(double metres) {captureAltitude=metres;}
    void captureAtGeography(std::array<double,3> camera) {captureGeo=camera;}
    void ProcessEvent(Rml::Event& event) override {
        auto id=event.GetCurrentElement()->GetId();
        const auto type=event.GetType();
        if(type=="mousedown"||type=="mousescroll"||type=="dblclick")touchGlobe();
        if(id=="map"||id=="menu") {
            if(!globe||optionsOpen||!menu)return;
            const double x=event.GetParameter<float>("mouse_x",0),y=event.GetParameter<float>("mouse_y",0);
            if(type=="mousedown"&&event.GetParameter<int>("button",0)==0) {
                globeDragging=globe->view.pick(x,y,ui->width(),ui->height()).has_value();
                globeMoved=false;globePress={x,y};globeDragView=globe->view;
            } else if(type=="mousemove"&&globeDragging) {
                const glm::dvec2 movement=glm::dvec2(x,y)-globePress;
                globeMoved=globeMoved||glm::length(movement)>4.;
                if(globeMoved) {
                    const double degrees=globeDragView.altitude()*2.*r1::GlobeView::tangent/ui->height()/rad;
                    globeTarget.lon=wrap(globeDragView.lon-movement.x*degrees/std::max(.08,std::cos(globeDragView.lat*rad)));
                    globeTarget.lat=std::clamp(globeDragView.lat+movement.y*degrees,-89.8,89.8);
                    globe->view.lon=globeTarget.lon;globe->view.lat=globeTarget.lat;
                    updateGlobePin();
                }
            } else if(type=="mouseup"&&globeDragging) {
                globeDragging=false;
                if(!globeMoved)if(auto hit=globe->view.pick(x,y,ui->width(),ui->height())) {
                    select(hit->lon,hit->lat);field("city-query",std::string());showCityChoices({});
                    warming=true;if(!smoke&&!std::getenv("R1WORLD_MENU_SMOKE"))request(pickLon,pickLat);
                }
            } else if(type=="mousescroll") {
                const double wheel=event.GetParameter<float>("wheel_delta_y",0);
                if(wheel<0)if(auto hit=globe->view.pick(x,y,ui->width(),ui->height())) {
                    globeTarget.lon=hit->lon;globeTarget.lat=hit->lat;
                }
                zoom=std::clamp(zoom*std::pow(1.6,-wheel),1.,131072.);
                event.StopPropagation();
            } else if(type=="dblclick") {
                if(auto hit=globe->view.pick(x,y,ui->width(),ui->height())) {
                    select(hit->lon,hit->lat);zoomMap(zoom*2.);
                }
            }
            return;
        }
        const bool press=event.GetType()=="mousedown";
        if(!press&&id==pressedId){pressedId.clear();return;}
        if(press)pressedId=id;
        if(id=="go")go();
        else if(id=="options")showOptions(true);
        else if(id=="options-back")showOptions(false);
        else if(id=="forced-apply")applyTypedTime();
        else if(id=="forced-clear"){forceTime(std::nullopt);field("forced-time",std::string());}
        else if(id.rfind("forced-preset-",0)==0)forceTime(std::stoi(id.substr(14))*60);
        else if(id=="weather-real")forceWeather(r1::WeatherOverride::Real);
        else if(id=="weather-clear")forceWeather(r1::WeatherOverride::Clear);
        else if(id=="weather-light-rain")forceWeather(r1::WeatherOverride::LightRain);
        else if(id=="weather-storm")forceWeather(r1::WeatherOverride::Storm);
        else if(id=="toaster-off")setToasterMode(false);
        else if(id=="toaster-on")setToasterMode(true);
        else if(id=="resume"&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}
        else if(id=="destination-go") {
            // The typed coordinates, from the Destination tab: leave the
            // settings only when they hold, and say why when they do not.
            if(go())showOptions(false);
            else text("destination-status","Invalid coordinates: latitude −90 to 90, longitude −180 to 180 (decimal point).");
        }
        else if(id=="street-map")zoomMap(std::max(zoom,32768.));
        else if(id=="reset-map")zoomMap(1);
        else if(id.rfind("city-choice-",0)==0) {
            const int choice=id.back()-'0';
            if(choice>=0&&choice<int(cityChoices.size())) {
                const auto selected=cityChoices[size_t(choice)];
                select(selected.lon,selected.lat);zoomMap(std::max(zoom,2048.));
                field("city-query",selected.label);
                cityInput=citySubmitted=selected.label;
                showCityChoices({});
                warming=true;if(!smoke)request(pickLon,pickLat);
            }
        }
        else {std::map<std::string,glm::dvec2> places{{"paris",{2.3522,48.8566}},{"tokyo",{139.7671,35.6812}},{"newyork",{-73.9855,40.758}},{"lawrence",{-95.2436,38.9585}},{"cape",{18.4241,-33.9249}},{"sydney",{151.2093,-33.8688}},{"pole",{0.,90.}}};
            if(places.count(id)){auto p=places.at(id);select(p.x,p.y);zoomMap(zoom);
                field("city-query",std::string());showCityChoices({});
                warming=true;request(pickLon,pickLat);}}
    }
    bool smokeZoom() {
        auto fail=[&](const char* why){
            saida::Log::error("[World menu E2E] FAIL zoom: ",why," (zoom=",zoom,")");
            testFailed=true;engine.sceneTree().quit();return false;
        };
        const double previousZoom=zoom;
        zoomMap(.25);
        if(zoom!=1)return fail("zoom must keep the whole-Earth lower bound");
        zoomMap(1e9);
        if(zoom!=131072.)return fail("zoom must keep the street-level upper bound");
        testClick("reset-map");
        if(zoom!=1)return fail("reset must restore the whole Earth");
        zoomMap(previousZoom);
        saida::Log::info("[World menu E2E] PASS zoom: bounds and reset");
        return true;
    }
    // The Options screen, as a player uses it: open it, force an hour, see the
    // Sun take it, have a bad hour refused without losing the good one, go
    // back to the real clock, close it. Presses only, like every other click.
    bool smokeOptions() {
        const bool savedToaster=toasterMode;
        const auto savedTime=forcedMinutes;
        const auto savedWeather=forcedWeather;
        auto fail=[&](const std::string& why){
            saida::Log::error("[World E2E] FAIL options: ",why);testFailed=true;engine.sceneTree().quit();return false;
        };
        testClick("options",false);
        if(!optionsOpen)return fail("the Options press did not open the screen");
        field("forced-time","14h30");testClick("forced-apply",false);
        if(forcedMinutes!=14*60+30)return fail("14h30 was not read as 14:30");
        double at=0;bool known=false;const int offset=utcOffset(known);
        if(!gameTime(at)||std::abs(std::fmod(at+offset,86400.)-(14*3600.+1800.))>1.)
            return fail("the Sun is not at 14:30 local (gameTime "+number(at,0)+", offset "+std::to_string(offset)+")");
        field("forced-time","25:00");testClick("forced-apply",false);
        if(forcedMinutes!=14*60+30)return fail("25:00 replaced the forced hour instead of being refused");
        testClick("forced-preset-12",false);
        if(forcedMinutes!=12*60)return fail("the 12:00 preset did not apply");
        testClick("forced-clear",false);
        if(forcedMinutes||forcedSent)return fail("Real time did not give the Sun back to the clock");
        // From the menu no tile may be resident yet: forcing the weather there
        // once threw "Missing terrain" and closed the game.
        testClick("weather-clear",false);
        if(forcedWeather!=r1::WeatherOverride::Clear)return fail("Clear sky did not force clear weather");
        testClick("weather-real",false);
        if(forcedWeather!=r1::WeatherOverride::Real)return fail("Real weather did not restore the forecast");
        testClick("toaster-on",false);
        if(!toasterMode)return fail("Mode grille-pain did not enable toaster mode");
        testClick("toaster-off",false);
        if(toasterMode)return fail("Standard quality did not disable toaster mode");
        // Go from the Destination tab refuses what is not a coordinate and
        // stays open to say so (the valid trip is the smoke's own Go below).
        {
            const std::string latitude=value("latitude"),longitude=value("longitude");
            const bool wasPending=pending;
            field("latitude",std::string("91"));testClick("destination-go",false);
            if(!optionsOpen||pending!=wasPending)return fail("Go accepted latitude 91 or left the settings");
            if(auto* status=ui->findElementById("destination-status");!status||status->GetInnerRML().empty())
                return fail("Go refused latitude 91 without saying why");
            field("latitude",latitude);field("longitude",longitude);
        }
        testClick("options-back",false);
        if(optionsOpen)return fail("Retour did not close the screen");
        saida::Log::info("[World E2E] options passed (forced hour reaches the Sun, bad hour refused, real time restored, weather forced from the menu, toaster mode toggled, bad destination refused in place)");
        forceTime(savedTime);
        forceWeather(savedWeather);
        // Testing both buttons must not silently change the measured preset.
        bool requestedToaster=savedToaster;
        if(const char* quality=std::getenv("R1WORLD_SMOKE_QUALITY")) {
            const std::string preset=quality;
            if(preset!="standard"&&preset!="toaster")return fail("R1WORLD_SMOKE_QUALITY must be standard or toaster");
            requestedToaster=preset=="toaster";
        }
        setToasterMode(requestedToaster);
        // A street by day for the walk and the fight: R1WORLD_SMOKE_HOUR=HH:MM
        // forces the local hour as the Options screen would. Night empties it.
        if(const char* hour=std::getenv("R1WORLD_SMOKE_HOUR")) {
            const auto minutes=r1::parseClockTime(hour);
            if(!minutes)return fail(std::string("R1WORLD_SMOKE_HOUR '")+hour+"' is not HH:MM");
            forceTime(minutes);
        }
        return true;
    }
    // The driver's own two phases, kept out of update() so the flow reads:
    // walk, drive, then either teleport or finish.
    void startSmokeDrive() {
        smokeDroveOnce=true;
        if(!carParked) {
            // The car not being there is a real outcome with a real cause --
            // no free kerb within 14 m -- and it fails on that cause rather
            // than on the drive that could not be attempted.
            saida::Log::error("[World E2E] FAIL no car parked within reach of the spawn");
            testFailed=true;engine.sceneTree().quit();return;
        }
        // A test driver chooses a free direction, like a human: aiming the car
        // at the nearest real wall is not a failure of the car. Same search the
        // spawn runs for the walk, over a road's worth of distance.
        smokeClear=0;double heading=carYaw;
        for(int direction=0;direction<32;++direction) {
            double candidate=direction*11.25,clear=clearAhead(carLon,carLat,carAlt,candidate,1.,60.);
            if(clear>smokeClear){smokeClear=clear;heading=candidate;}
        }
        r1::VehicleRuntime::deactivate(*car);
        carYaw=heading;placeCar();
        // The car is where the player spawned and the walk phase has taken him
        // eleven metres away from it, which is the first thing this test found
        // and the right thing for it to find: a door has a handle, not a
        // radius. So the driver walks back to it, the way a player does.
        smokeApproach=true;smokeApproachTime=0;
        saida::Log::info("[World E2E] walking back to the car, ",carDistance(),"m away");
    }
    void beginSmokeDriving() {
        if(!enterCar()) {
            saida::Log::error("[World E2E] FAIL could not enter the car, ",
                              carDistance(),"m away, parked=",carParked);
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeDriving=true;smokeDriveTime=0;smokeTopSpeed=0;smokeDriveStart=ecef(lon,lat,alt);
        if(std::getenv("R1WORLD_SMOKE_VEHICLE_DAMAGE")) {
            auto* body=r1::VehicleRuntime::body(*car);
            auto* damage=body->getBehaviour<saida::DeformableMeshBehaviour>();
            const auto& spec=vehicleSpec(*car);
            if(!damage->applyImpact({0,float(spec.height*.5),float(-spec.length*.5)},{0,0,1},damage->maximumImpactEnergy)) {
                saida::Log::error("[World E2E] FAIL could not damage the actual car mesh");
                testFailed=true;engine.sceneTree().quit();return;
            }
            r1::VehicleRuntime::synchronize(*car);
            saida::Log::info("[World E2E] damaged car, absorbed energy=",damage->absorbedEnergy(),
                " J, power loss=",r1::VehicleRuntime::driver(*car)->damage);
        }
        churn=0;churnFrames=0;dirtyFrames=0;
        saida::Log::info("[World E2E] driving, ",smokeClear,"m clear on heading ",carYaw);
    }
    // Getting into a car that belongs to the traffic, which is the whole of
    // "n'importe quelle voiture" and the one part of it a player cannot be
    // talked out of noticing when it is broken.
    //
    // The driver stands the player beside a live traffic car rather than
    // walking him to it: the car is moving, so walking to it is a pursuit
    // problem and this test is not about pursuit. Standing beside it is
    // exactly where stepping out of a car leaves him, so the reach it tests is
    // the reach a player gets.
    void runSmokeTakeover() {
        smokeTookOver=true;
        Loaded* tile=nullptr;size_t index=0;
        for(auto& [key,l]:loaded) {
            for(size_t i=0;i<l.cars.size();++i)
                if(l.cars[i]&&trafficSlotLive(l,i)){tile=&l;index=i;break;}
            if(tile)break;
        }
        if(!tile) {
            saida::Log::error("[World E2E] FAIL no traffic car to take over");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Node* wanted=tile->cars[index];
        saida::Node* previous=car;
        const double previousLon=carLon,previousLat=carLat;
        const auto where=geodeticOf(*tile,wanted->transform().position);
        // Two and a half metres to the east of it, which is a door's width and
        // inside kCarReach.
        const auto beside=advance(where.x,where.y,2.5,0.);
        if(!World::tile(beside.x,beside.y)) {
            saida::Log::error("[World E2E] FAIL the traffic car stands off the loaded world");
            testFailed=true;engine.sceneTree().quit();return;
        }
        lon=beside.x;lat=beside.y;alt=height(lon,lat);
        if(!enterCar()||!driving) {
            saida::Log::error("[World E2E] FAIL could not take over a traffic car 2.5 m away");
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(car!=wanted) {
            saida::Log::error("[World E2E] FAIL took over a different car than the one reached");
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(tile->cars[index]!=nullptr||tile->flow.agents()[index].alive) {
            saida::Log::error("[World E2E] FAIL the traffic still owns the car the player is in");
            testFailed=true;engine.sceneTree().quit();return;
        }
        // And the car he stepped out of is still standing where he left it,
        // which is the half of this that copying a model would have broken.
        const auto left=std::find_if(parked.begin(),parked.end(),
                                     [&](const Parked& p){return p.node==previous;});
        if(left==parked.end()||std::abs(left->lon-previousLon)>1e-9||std::abs(left->lat-previousLat)>1e-9) {
            saida::Log::error("[World E2E] FAIL the previous car did not stay where it was left");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] took over a traffic car; ",parked.size(),
                         " car(s) left standing");
        // Taking the same solver body preserves its momentum. Brake it through
        // normal frames before testing the door, just as for the first drive.
        smokeDriving=true;smokeBrake=true;smokeDriveTime=0;
    }

    // `--sail`: find a moored boat, stand beside it, take the helm, sail it
    // out at full throttle, be refused a landing at speed, then stop. Every
    // step fails on its own reason (rule 3 of CLAUDE.md).
    void runSmokeSail(float delta) {
        const double dt=std::min(.05,double(delta));
        smokeSailTime+=dt;
        if(smokeSailWait) {
            BoatReach best;double bestDistance=1e30;
            for(auto& [key,t]:loaded)
                for(size_t i=0;i<t.boats.size();++i) {
                    Mooring& m=t.boats[i];
                    if(m.taken||!mooredNode(t,m)||m.kind=="cargo"||m.kind=="liner")continue;
                    const double d=hullDistance(t.frame,m.local,m.heading,m.length,m.beam);
                    if(d<bestDistance){bestDistance=d;best={&t,i,BoatSource::Moored};}
                }
            if(!best.tile) {
                if(smokeSailTime>60.) {
                    saida::Log::error("[World E2E] FAIL sail: no moored boat streamed within 60 s");
                    testFailed=true;engine.sceneTree().quit();
                }
                return;
            }
            // Stand the player on the first dry spot beside the hull, which is
            // where a quay or a pier leaves him.
            Mooring& m=best.tile->boats[best.index];
            const auto centre=geodeticOf(*best.tile,glm::vec3(m.local));
            const double s=std::sin(m.heading*rad),c=std::cos(m.heading*rad);
            bool placed=false;
            for(double off:{1.,2.,3.}) {
                for(double along=-m.length*.4;along<=m.length*.4&&!placed;along+=1.)
                    for(double side:{1.,-1.}) {
                        const double o=(m.beam*.5+off)*side;
                        auto q=advance(centre.x,centre.y,s*along+c*o,c*along-s*o);
                        if(tile(q.x,q.y)&&!blocked(q.x,q.y)){lon=q.x;lat=q.y;alt=height(lon,lat);placed=true;break;}
                    }
                if(placed)break;
            }
            if(!placed) {
                saida::Log::error("[World E2E] FAIL sail: no standable ground beside the nearest boat (",
                                  m.kind,", ",m.name,")");
                testFailed=true;engine.sceneTree().quit();return;
            }
            smokeSailWait=false;
            if(!enterBoat()) {
                saida::Log::error("[World E2E] FAIL sail: could not take the helm from beside the hull");
                testFailed=true;engine.sceneTree().quit();return;
            }
            // Head for the longest run of open water, as a skipper would.
            smokeSailClear=0;double heading=boat.yaw;
            for(int direction=0;direction<32;++direction) {
                double candidate=direction*11.25,clear=0;
                for(double d=2.;d<=120.;d+=2.) {
                    auto q=onward(boat.lon,boat.lat,std::sin(candidate*rad)*d,std::cos(candidate*rad)*d);
                    if(!navigable(q.x,q.y))break;
                    clear=d;
                }
                if(clear>smokeSailClear){smokeSailClear=clear;heading=candidate;}
            }
            boat.yaw=yaw=heading;
            smokeSailing=true;smokeSailBrake=false;smokeSailTime=0;smokeSailTop=0;
            smokeSailStart=ecef(lon,lat,alt);
            saida::Log::info("[World E2E] sailing a ",boat.kind,", ",smokeSailClear,"m of open water ahead");
            return;
        }
        smokeSailTop=std::max(smokeSailTop,boat.speed);
        if(!navigable(lon,lat)) {
            saida::Log::error("[World E2E] FAIL sail: the boat left the water at ",lon,", ",lat);
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(!smokeSailBrake) {
            if(smokeSailTime<8.)return;
            const double covered=glm::length(ecef(lon,lat,alt)-smokeSailStart);
            if(covered<std::min(15.,smokeSailClear*.4)||smokeSailTop<std::min(3.,boat.top*.5)) {
                saida::Log::error("[World E2E] FAIL sail: covered=",covered,"m of ",smokeSailClear,
                                  "m clear, top=",smokeSailTop," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] sail passed, covered=",covered,"m top=",smokeSailTop," m/s");
            if(std::abs(boat.speed)>kBoatExitSpeed&&leaveBoat()) {
                saida::Log::error("[World E2E] FAIL sail: stepped ashore at ",std::abs(boat.speed)," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            smokeSailBrake=true;smokeSailTime=0;return;
        }
        if(std::abs(boat.speed)>kBoatExitSpeed) {
            if(smokeSailTime>40.) {
                saida::Log::error("[World E2E] FAIL sail: the boat would not slow down, still ",
                                  std::abs(boat.speed)," m/s");
                testFailed=true;engine.sceneTree().quit();
            }
            return;
        }
        smokeSailing=false;
        const double stopped=std::abs(boat.speed);
        smokeSwimBoat=boat.node;
        if(!leaveBoat()||!swimming) {
            saida::Log::error("[World E2E] FAIL swim: could not enter the water from a stopped boat");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS sailing: stopped at ",stopped," m/s and entered the water");
        smokeSwimming=true;smokeSailTime=0;smokeSwimStart=ecef(lon,lat,alt);
    }
    void runSmokeSwim(float delta) {
        smokeSailTime+=std::min(.05,double(delta));
        if(smokeSailTime<1.5)return;
        const double covered=glm::length(ecef(lon,lat,alt)-smokeSwimStart);
        const double rootBelow=origin.local(ecef(lon,lat,alt)).y-player->transform().position.y;
        if(!swimming||!onWater(lon,lat)||covered<1.||rootBelow<playerHeight*.39||rootBelow>playerHeight*.78) {
            saida::Log::error("[World E2E] FAIL swim: covered=",covered,"m, water=",onWater(lon,lat),
                              ", body depth=",rootBelow,"m");
            testFailed=true;engine.sceneTree().quit();return;
        }
        smokeSwimming=false;
        if(!car) {
            saida::Log::error("[World E2E] FAIL car sink: no car model to test");
            testFailed=true;engine.sceneTree().quit();return;
        }
        // A car already on this water cell must eject its driver and descend.
        carParked=true;car->setEnabled(true);
        carLon=lon;carLat=lat;carAlt=alt;carYaw=yaw;
        carSinking=false;driving=true;swimming=false;
        driveCar(.05,1.,0.,false);
        updateSinkingCar(1.);
        if(driving||!swimming||!carSinking||carAlt>carWaterLevel-1.) {
            saida::Log::error("[World E2E] FAIL car sink: driver afloat=",swimming,
                              ", car depth=",carWaterLevel-carAlt,"m");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS car sink: driver swimming, car ",carWaterLevel-carAlt,"m below water");
        if(!enterBoat()||boat.node!=smokeSwimBoat) {
            saida::Log::error("[World E2E] FAIL swim: could not reboard the same boat");
            testFailed=true;engine.sceneTree().quit();return;
        }
        saida::Log::info("[World E2E] PASS swim: ",covered,"m through water, reboarded the same boat");
        smokeSeaWait=true;smokeSailTime=0;
    }
    // Then the ships at sea: predicted offline from the local base, they must
    // be out within half a minute, or the run says why not.
    void runSmokeSea(float delta) {
        smokeSailTime+=std::min(.05,double(delta));
        size_t under=0;
        for(auto& s:seaShips)under+=s.v.speed>0;
        if(!seaShips.empty()&&smokeSailTime>12.) {
            saida::Log::info("[World E2E] PASS sea: ",seaShips.size()," ships out, ",under," under way");
            const SeaShip* target=nullptr;
            double nearest=1e30;
            for(const auto& ship:seaShips)if(ship.v.speed>2.) {
                const double distance=metresFrom(ship.v.lon,ship.v.lat);
                if(distance<nearest){nearest=distance;target=&ship;}
            }
            if(!target) {
                saida::Log::error("[World E2E] FAIL sea helm: no moving ship to approach");
                testFailed=true;engine.sceneTree().quit();return;
            }
            const Vessel& v=target->v;
            const double side=v.beam*.5+boat.beam*.5+1.;
            const auto alongside=advance(v.lon,v.lat,std::cos(v.yaw*rad)*side,-std::sin(v.yaw*rad)*side);
            boat.lon=lon=alongside.x;boat.lat=lat=alongside.y;
            boat.alt=alt=v.alt;boat.yaw=yaw=v.yaw;boat.speed=v.speed;
            const auto* hull=v.node;
            const double speed=v.speed;
            if(hopAboard()!=HopResult::Boarded||!sailing||boat.node!=hull||std::abs(boat.speed-speed)>1e-6) {
                saida::Log::error("[World E2E] FAIL sea helm: could not take the same moving hull from alongside");
                testFailed=true;
            } else saida::Log::info("[World E2E] PASS sea helm: took over the same moving hull at ",boat.speed," m/s");
            engine.sceneTree().quit();
        } else if(smokeSailTime>30.) {
            std::string last=sea->lastLine();
            if(last.empty())last="the sea said nothing";
            saida::Log::error("[World E2E] FAIL sea: no ship at sea after 30 s (",last,")");
            testFailed=true;engine.sceneTree().quit();
        }
    }

    // `--fly`: whatever flies where the map was clicked, one plane and one
    // helicopter. The plane lines up on the longest clear run it can find,
    // takes off, is refused the door in the air, is then flown low at a
    // building and must stop against it -- not crash -- come down and let its
    // pilot out. The helicopter spins up, climbs, flies forward, sets down
    // and lets its pilot out (or, set down on a roof, keeps him in). Every
    // step fails on its own reason (rule 3 of CLAUDE.md).
    // R1WORLD_FLY_SHOT=<png> with R1WORLD_FLY_SHOT_AT=<moment> photographs
    // the chase view at that moment of the run, then ends it: "stand", "climb",
    // "stop", "heli-stand" or "heli". A test can say the aircraft flew; only
    // a picture says it looked like flying (rule 1 of CLAUDE.md).
    void flyShot(const char* moment) {
        const char* path=std::getenv("R1WORLD_FLY_SHOT");
        const char* at=std::getenv("R1WORLD_FLY_SHOT_AT");
        if(!path||!at||std::string(at)!=moment||captureQueued)return;
        saida::CaptureRequest shot;
        shot.pngPath=path;shot.frame=3;shot.fixedStep=1.f/60.f;shot.settleTimeoutFrames=900;
        captureQueued=true;
        saida::Log::info("[World E2E] photographing '",moment,"' to ",path);
        engine.captureFrameThenExit(shot);
    }
    void flyFail(const std::string& why) {
        saida::Log::error("[World E2E] FAIL fly: ",why);
        testFailed=true;engine.sceneTree().quit();
    }
    static std::string flyCategory(const r1::AircraftType& t) {return t.klass=="helicopter"?"helicopter":"plane";}
    // The parked aircraft of a category not flown yet: business jets first
    // (they take off in the shortest run), then airliners, then helicopters.
    bool smokeNextAircraft(Loaded*& found,size_t& index) {
        found=nullptr;double best=1e30;int bestRank=9;
        for(auto& [key,t]:loaded) {
            const glm::dvec3 me=t.frame.local(ecef(lon,lat,alt));
            for(size_t i=0;i<t.aircraft.size();++i) {
                const AircraftSpot& s=t.aircraft[i];
                const auto* type=r1::aircraftType(s.type);
                if(s.taken||!s.node||!type||smokeFlown.count(flyCategory(*type)))continue;
                const int rank=type->klass=="jet"?0:type->klass=="airliner"?1:2;
                const double d=std::hypot(me.x-s.local.x,me.z-s.local.z);
                if(rank<bestRank||(rank==bestRank&&d<best)){bestRank=rank;best=d;found=&t;index=i;}
            }
        }
        return found!=nullptr;
    }
    // How far the aircraft could go along a heading before a building, the
    // water (on the ground) or the edge of the loaded world.
    double clearRun(double heading,double reach,double step,double above) {
        double clear=0;
        for(double d=step;d<=reach;d+=step) {
            const auto q=onward(plane.lon,plane.lat,std::sin(heading*rad)*d,std::cos(heading*rad)*d);
            if(!tile(q.x,q.y)||aircraftHits(q.x,q.y,plane.alt+above,heading)||(above<1.&&onWater(q.x,q.y)))break;
            clear=d;
        }
        return clear;
    }
    void runSmokeFly(float delta) {
        const double dt=std::min(.05,double(delta));
        smokeFlyTime+=dt;
        smokeF=smokeR=smokeUp=0;
        if(smokeFlyWait) {
            Loaded* t=nullptr;size_t i=0;
            if(!smokeNextAircraft(t,i)) {
                if(smokeFlown.empty()&&smokeFlyTime<90.)return;
                if(smokeFlown.empty())return flyFail("no aircraft streamed within 90 s of the spawn");
                saida::Log::info("[World E2E] PASS fly: flew ",smokeFlown.size()," kind(s) of aircraft");
                engine.sceneTree().quit();return;
            }
            // Line up only once the neighbourhood is resident: a run measured
            // over three tiles of four misses the buildings of the fourth.
            size_t resident=0;
            for(const auto& k:ring)resident+=loaded.count(k.key());
            if(resident<ring.size()&&smokeFlyTime<45.)return;
            AircraftSpot& spot=t->aircraft[i];
            const auto* type=r1::aircraftType(spot.type);
            // Stand the pilot beside the fuselage, where a walk would bring him.
            const auto centre=geodeticOf(*t,glm::vec3(spot.local));
            const double s=std::sin(spot.heading*rad),c=std::cos(spot.heading*rad);
            bool placed=false;
            for(double along:{type->length*.3,0.,-type->length*.25})
                for(double side:{-1.,1.}) {
                    if(placed)break;
                    const double o=(fuselageHalf(*type)+1.5)*side;
                    const auto q=advance(centre.x,centre.y,s*along+c*o,c*along-s*o);
                    if(tile(q.x,q.y)&&!blocked(q.x,q.y)){lon=q.x;lat=q.y;alt=height(lon,lat);placed=true;}
                }
            if(!placed)return flyFail("no standable ground beside the "+spot.type+" ("+spot.name+")");
            saida::Node* wanted=spot.node;
            if(!enterAircraft()||!piloting||plane.node!=wanted)
                return flyFail("could not take the controls of the "+spot.type+" the pilot stands beside");
            smokeFlyWait=false;smokeFlyTime=0;smokeFlyTop=0;
            smokeFlyStart=ecef(plane.lon,plane.lat,plane.alt);smokeFlyAlt=plane.alt;
            flyShot(plane.helicopter()?"heli-stand":"stand");
            if(plane.helicopter()) {
                smokeFlyPhase=10;
                saida::Log::info("[World E2E] flying a ",plane.type->name," (",spot.name,")");
            } else {
                // Line up as a pilot would: the longest clear run on the ground.
                smokeFlyClear=0;double heading=plane.yaw;
                for(int k=0;k<36;++k) {
                    const double h=k*10.,run=clearRun(h,1500.,10.,0.);
                    if(run>smokeFlyClear){smokeFlyClear=run;heading=h;}
                }
                plane.yaw=yaw=heading;
                smokeFlyPhase=1;
                saida::Log::info("[World E2E] flying a ",plane.type->name," (",spot.name,"), ",smokeFlyClear,
                                 " m clear on heading ",heading);
            }
            return;
        }
        Aircraft& p=plane;
        smokeFlyTop=std::max(smokeFlyTop,p.speed);
        switch(smokeFlyPhase) {
        case 1: {  // the take-off roll
            smokeF=1.;smokeUp=p.speed>=p.type->rotate?1.:0.;
            if(p.airborne&&p.alt-smokeFlyAlt>=25.) {
                saida::Log::info("[World E2E] PASS take-off: ",p.type->name," up ",p.alt-smokeFlyAlt," m, top ",
                                 smokeFlyTop," m/s, ",glm::length(ecef(p.lon,p.lat,p.alt)-smokeFlyStart)," m from the stand");
                flyShot("climb");
                smokeFlyPhase=2;smokeFlyTime=0;
            } else if(smokeFlyTime>60.) {
                flyFail("no take-off in 60 s: top "+number(smokeFlyTop,1)+" m/s for a rotation at "+
                        number(p.type->rotate,1)+", "+number(smokeFlyClear,0)+" m clear, airborne="+(p.airborne?"yes":"no"));
            }
            return;
        }
        case 2: {  // flown low at the nearest tall building: it must stop the aircraft
            const Loaded* in=nullptr;glm::dvec2 centre(0);double radius=0,best=1e30;
            for(auto& [key,t]:loaded) {
                const glm::dvec3 me=t.frame.local(ecef(p.lon,p.lat,p.alt));
                for(const auto& f:t.footprints) {
                    if(f.top>1e29||f.points.size()<3)continue;
                    glm::dvec2 c(0);
                    for(const auto& q:f.points)c+=q;
                    c/=double(f.points.size());
                    double r=0;
                    for(const auto& q:f.points)r=std::max(r,glm::length(q-c));
                    const auto at=geodeticOf(t,glm::vec3(float(c.x),0.f,float(c.y)));
                    const double tall=f.top-t.frame.local(ecef(at.x,at.y,groundAt(at.x,at.y,p.alt))).y;
                    const double d=std::hypot(me.x-c.x,me.z-c.y);
                    if(r<6.||tall<8.||d>=best)continue;
                    best=d;in=&t;centre=c;radius=r;
                }
            }
            double chosen=-1;
            if(in) {
                const auto middle=geodeticOf(*in,glm::vec3(float(centre.x),0.f,float(centre.y)));
                const double away=radius+p.type->length*.5+p.type->span*.5+25.;
                for(int k=0;k<36&&chosen<0;++k) {
                    const double h=k*10.;
                    const auto start=advance(middle.x,middle.y,-std::sin(h*rad)*away,-std::cos(h*rad)*away);
                    if(!tile(start.x,start.y)||onWater(start.x,start.y))continue;
                    const double ground=groundAt(start.x,start.y,p.alt);
                    if(aircraftHits(start.x,start.y,ground+3.,h))continue;
                    chosen=h;p.lon=start.x;p.lat=start.y;p.alt=ground+3.;
                }
                if(chosen>=0)saida::Log::info("[World E2E] flying at a building ",away," m ahead, ",
                                              p.type->stall*1.3," m/s, 3 m up");
            }
            if(chosen<0) {
                saida::Log::info("[World E2E] no building with a clear approach here; the stop is not tested");
                smokeFlyPhase=4;smokeFlyTime=0;return;
            }
            p.yaw=yaw=chosen;p.pitch=0;p.roll=0;p.climb=0;p.airborne=true;
            p.speed=p.type->stall*1.3;p.lever=1;p.thrust=1;planeStopped=false;
            smokeFlyPhase=3;smokeFlyTime=0;
            return;
        }
        case 3: {  // stopped, not crashed, and not inside the building
            smokeF=1.;
            if(planeStopped&&p.speed==0.) {
                if(buildingAt(p.lon,p.lat,p.alt+.4))return flyFail("the aircraft stopped inside the building");
                saida::Log::info("[World E2E] PASS stopped by a building, no crash, ",p.alt-planeGround," m up");
                flyShot("stop");
                smokeFlyPhase=4;smokeFlyTime=0;
            } else if(smokeFlyTime>6.) {
                flyFail("flew at a building for 6 s and was never stopped");
            }
            return;
        }
        case 4: {  // down, stopped, and out
            smokeF=-1.;smokeUp=-1.;
            if(p.airborne||std::abs(p.speed)>kAircraftExitSpeed) {
                if(smokeFlyTime>40.)flyFail("the plane did not come down and stop within 40 s");
                return;
            }
            if(!leaveAircraft())return flyFail("could not step down from the landed plane");
            saida::Log::info("[World E2E] PASS plane: down, stopped and out");
            smokeFlown.insert("plane");smokeFlyPhase=0;smokeFlyWait=true;smokeFlyTime=0;
            return;
        }
        case 10: {  // spin up and climb
            smokeUp=1.;
            if(p.alt-smokeFlyAlt>=20.) {
                saida::Log::info("[World E2E] PASS helicopter climb: ",p.alt-smokeFlyAlt," m in ",smokeFlyTime," s");
                flyShot("heli");
                double run=0,heading=p.yaw;
                for(int k=0;k<36;++k){const double h=k*10.,r=clearRun(h,300.,5.,0.);if(r>run){run=r;heading=h;}}
                p.yaw=yaw=heading;
                smokeFlyStart=ecef(p.lon,p.lat,p.alt);smokeFlyPhase=11;smokeFlyTime=0;
            } else if(smokeFlyTime>15.) {
                flyFail("the helicopter climbed "+number(p.alt-smokeFlyAlt,1)+" m in 15 s");
            }
            return;
        }
        case 11: {  // forward
            smokeF=1.;
            if(smokeFlyTime>=4.) {
                const double covered=glm::length(ecef(p.lon,p.lat,p.alt)-smokeFlyStart);
                if(covered<20.&&!planeStopped)return flyFail("the helicopter covered "+number(covered,1)+" m in 4 s");
                saida::Log::info("[World E2E] PASS helicopter forward: ",covered," m, top ",smokeFlyTop," m/s");
                smokeFlyPhase=12;smokeFlyTime=0;
            }
            return;
        }
        case 12: {  // brake to a hover, and jump out in the air
            smokeF=p.speed>1.?-1.:0.;
            if(p.airborne&&std::abs(p.speed)<=1.) {
                const double up=p.alt-groundAt(p.lon,p.lat,p.alt);
                if(!leaveAircraft()||piloting)return flyFail("F did not let the pilot out "+number(up,1)+" m up");
                if(jumpOffset<5.)return flyFail("jumped from "+number(up,1)+" m up but falls only "+number(jumpOffset,1)+" m");
                saida::Log::info("[World E2E] PASS jumped from the helicopter, ",jumpOffset," m up");
                smokeFlyPhase=13;smokeFlyTime=0;
            } else if(smokeFlyTime>30.) {
                flyFail("the helicopter did not come to a hover within 30 s");
            }
            return;
        }
        case 13: {  // he lands, and the helicopter he left comes down
            const bool down=jumpOffset<=0&&(leftAircraft.empty()||!leftAircraft.back().airborne);
            if(down) {
                if(!swimming&&blocked(lon,lat))return flyFail("the jump landed inside a building");
                flyShot("jump");
                saida::Log::info("[World E2E] PASS landed ",swimming?"in the water":"on the ground",
                                 " in ",smokeFlyTime," s, and the helicopter came down");
                smokeFlown.insert("helicopter");smokeFlyPhase=0;smokeFlyWait=true;smokeFlyTime=0;
            } else if(smokeFlyTime>20.) {
                flyFail("still falling after 20 s: "+number(jumpOffset,1)+" m up, helicopter airborne="+
                        (leftAircraft.empty()?"none":leftAircraft.back().airborne?"yes":"no"));
            }
            return;
        }
        default: return;
        }
    }

    // Diagnostic scene composition; this traversal is only run by the smoke test.
    void measureWalk() {
        const auto started=std::chrono::steady_clock::now();
        size_t nodes=0;
        engine.sceneTree().world().traverse([&](saida::Node&,const glm::mat4&){++nodes;});
        const double ms=msSince(started);
        saida::Log::info("[World perf] one bare traversal of ",nodes," nodes took ",ms," ms");
        // Where they are. A walk is paid per node, so the composition is the
        // whole optimisation brief.
        auto subtree=[](saida::Node& n){
            size_t count=0;
            std::function<void(saida::Node&)> go=[&](saida::Node& x){
                ++count;for(auto& c:x.children())go(*c);
            };
            go(n);return count;
        };
        size_t plants=0,plantNodes=0,otherNodes=0,tiles=0;
        for(auto& [key,t]:loaded) {
            ++tiles;
            size_t here=subtree(*t.node);
            for(const Plant& plant:t.vegetation){++plants;plantNodes+=subtree(*plant.node);}
            otherNodes+=here;
        }
        saida::Log::info("[World perf] ",tiles," tiles hold ",otherNodes," nodes, of which ",
                         plantNodes," belong to ",plants," plants (",
                         plants?double(plantNodes)/double(plants):0.," per plant)");
    }
    void reportChurn(const char* phase) {
        saida::Log::info("[World perf] ",phase,": ",
                         int(churnFrames>0?100.*dirtyFrames/churnFrames:0.),
                         "% of frames updated scene membership (",
                         churnFrames>0?churn/churnFrames:0.," indexed nodes per frame over ",
                         int(churnFrames)," frames)");
        churn=0;churnFrames=0;dirtyFrames=0;
    }
    // The cheat line, as a player uses it: open, type, Enter. Each code must
    // have done its thing beside him, or the smoke fails naming which.
    bool smokeCheated=false;
    // Then the line opened and closed over real frames, as a player presses T
    // again and again: the engine's update must keep the focus it was given,
    // and the private memory must not grow with the presses. The world is
    // still loading when the walk ends -- it grew by 200 to 370 MB over the
    // next seconds whatever the line did (16 October 2026: the focus alone, a
    // style alone, the line) -- so the presses wait until the memory holds
    // still: two 2-second readings within 16 MB. A world that never settles
    // within 90 s has its keyboard checked, and the memory is said unread.
    static constexpr int kSmokeCheatPresses=60;
    int smokeCheatCycles=0; double smokeCheatMemory=0;
    bool smokeCheatSettled=false; int smokeSettleStill=0; double smokeSettleSince=-1,smokeSettleRead=0,smokeSettleLast=0;
    double smokeMountedShotDelay=-1;
    void cycleSmokeCheat(double dt) {
        if(smokeMountedShotDelay>=0) {
            setAiming(true);poseArms();bikeHandlePose();
            smokeMountedShotDelay-=dt;
            if(smokeMountedShotDelay>0)return;
            smokeMountedShotDelay=-1;
            auto fail=[&](const char* reason){saida::Log::error("[World E2E] FAIL cheats: ",reason);testFailed=true;engine.sceneTree().quit();};
            const int rounds=arsenal.loaded();fire();
            if(arsenal.loaded()!=rounds-1||pistol->parent()!=playerBody||combatFilter().ignore!=r1::VehicleRuntime::body(*car)->bodyId()) {
                saida::Log::error("[World E2E] mounted shot before=",rounds," after=",arsenal.loaded(),
                    " attached=",pistol->parent()==playerBody," own-body-filter=",combatFilter().ignore==r1::VehicleRuntime::body(*car)->bodyId());
                fail("the mounted pistol did not fire or ignored the wrong collider");return;
            }
            if(!leaveCar()||playerBikeRider||playerBody->parent()!=player||car->findByPath("Vehicle collider/Rider")) {
                fail("leaving a bike did not return the real body and leave an empty saddle");return;
            }
            saida::Log::info("[World E2E] PASS motorcycle empty spawn, solver mount, armed shot and empty dismount");
            const double hp=health.hp;hurtPlayer(500.,glm::vec3(1,0,0),"the test");
            if(health.take(500.)||health.hp!=hp||wasted){fail("an invincible player was hurt");return;}
            health.shield=0;
            saida::Log::info("[World E2E] PASS cheats: helicopter, sport car, city bike, motocross and jet beside the player, invincibility");
            return;
        }
        if(!smokeCheatSettled) {
            if(smokeSettleSince<0){smokeSettleSince=smokeSettleRead=clock;smokeSettleLast=privateMegabytes();return;}
            if(clock-smokeSettleRead<2.)return;
            const double now=privateMegabytes();
            smokeSettleStill=std::abs(now-smokeSettleLast)<16.?smokeSettleStill+1:0;
            smokeSettleLast=now;smokeSettleRead=clock;
            if(smokeSettleStill<2&&clock-smokeSettleSince<90.)return;
            smokeCheatSettled=true;smokeCheatMemory=smokeSettleStill>=2?now:-1.;
            saida::Log::info("[World E2E] cheats: ",smokeSettleStill>=2?"memory settled":"memory still moving",
                             " after ",int(clock-smokeSettleSince)," s at ",int(now)," MB");
            return;
        }
        if(cheatOpen) {
            if(engine.uiInteraction().focusedWebCanvas()!=ui) {
                saida::Log::error("[World E2E] FAIL cheats: the line lost the keyboard after an update (press ",smokeCheatCycles/2,")");
                testFailed=true;engine.sceneTree().quit();return;
            }
            closeCheat();
        } else openCheat();
        if(++smokeCheatCycles<2*kSmokeCheatPresses)return;
        if(cheatOpen)closeCheat();
        smokeCheatCycles=0;
        if(smokeCheatMemory<0)
            saida::Log::warn("[World E2E] PASS cheats: ",kSmokeCheatPresses," presses of T kept the keyboard;"
                             " memory not judged, the world never settled");
        else {
            const double grown=privateMegabytes()-smokeCheatMemory;
            saida::Log::info("[World E2E] ",grown<64.?"PASS":"FAIL"," cheats: ",kSmokeCheatPresses,
                             " presses of T kept the keyboard; private memory went from ",int(smokeCheatMemory),
                             " MB by ",grown>=0?"+":"",int(grown)," MB");
            if(grown>=64.){testFailed=true;engine.sceneTree().quit();return;}
        }
        smokeFinishPhase();
    }
    bool smokeCheats() {
        smokeCheated=true;
        auto fail=[&](const std::string& why){
            saida::Log::error("[World E2E] FAIL cheats: ",why);testFailed=true;engine.sceneTree().quit();return false;
        };
        auto type=[&](const std::string& word){
            openCheat();
            if(!cheatOpen||engine.uiInteraction().focusedWebCanvas()!=ui)return false;
            if(held(GLFW_KEY_W)||clicked(GLFW_MOUSE_BUTTON_LEFT))return false;
            field("cheat-input",word);
            const std::string typed=value("cheat-input");closeCheat();runCheat(typed);
            return !cheatOpen&&engine.uiInteraction().focusedWebCanvas()==nullptr;
        };
        const size_t aircraft=leftAircraft.size(),cars=parked.size();
        if(!type("buzzoff"))return fail("the line did not take and give back the keyboard");
        if(leftAircraft.size()!=std::min(aircraft+1,kLeftAircraft)||!leftAircraft.back().helicopter())
            return fail("BUZZOFF left no helicopter: "+written["text:cheat-said"]);
        if(!type("Comet ")||parked.size()!=std::min(cars+1,kAbandonedCars)||vehicleSpec(*parked.back().node).name!="sport")
            return fail("COMET parked no sport car: "+written["text:cheat-said"]);
        if(!type("moto")||vehicleSpec(*parked.back().node).name!="moto_city")
            return fail("MOTO parked no city bike: "+written["text:cheat-said"]);
        if(!type("rocket")||vehicleSpec(*parked.back().node).name!="moto_cross")
            return fail("ROCKET parked no motocross bike: "+written["text:cheat-said"]);
        if(!type("plane")||leftAircraft.back().type->name!="bizjet")return fail("PLANE left no jet: "+written["text:cheat-said"]);
        for(const auto* a:{&leftAircraft[leftAircraft.size()-2],&leftAircraft.back()})
            if(metresBetween(a->lon,a->lat,lon,lat)>45.)return fail(a->type->name+" stands beyond 45 m");
        const size_t before=leftAircraft.size();
        if(!type("hesoyam")||leftAircraft.size()!=before)return fail("an unknown word did something");
        if(!type("painkiller")||!health.invincible())return fail("PAINKILLER gave no invincibility");
        const auto cityBike=std::find_if(parked.begin(),parked.end(),[&](const Parked& p) {
            return p.node&&vehicleSpec(*p.node).name=="moto_city";
        });
        if(cityBike==parked.end())return fail("the city bike disappeared before the rider check");
        const Parked bike=*cityBike;
        lon=bike.lon;lat=bike.lat;alt=height(lon,lat,bike.alt);
        if(car&&car->findByPath("Vehicle collider/Rider"))return fail("a parked bike contains a traffic rider");
        if(bike.node->findByPath("Vehicle collider/Rider"))return fail("MOTO spawned an occupied bike");
        if(!enterCar()||!ridingBike()||playerBikeRider!=playerBody||
           playerBody->parent()!=r1::VehicleRuntime::body(*car)||car->findByPath("Vehicle collider/Rider"))
            return fail("F did not mount exactly the real player body on the bike solver");
        selectWeapon(r1::combat::Weapon::Pistol);setAiming(true);
        // Weapon selection has a real 250 ms draw cooldown. Continue through
        // ordinary frames so this check exercises it rather than bypassing it.
        smokeMountedShotDelay=.35;
        return true;
    }
    void smokeFinishPhase() {
        if(!smokeCheated) {
            if(smokeCheats())smokeCheatCycles=1;
            return;
        }
        reportChurn("whole run");
        if(hopWanted&&!hopDone) {
            // Arrive from the driver's seat while looking out of a side window.
            // A stale lookYaw made forward walking go sideways after loading.
            if(!driving&&carParked&&carDistance()<kCarReach)enterCar();
            if(driving) {
                lookYaw=90.;lookIdle=.4;
                smokeArrivalHeading=wrap(yaw+lookYaw);
            }
            // Teleport the way a player does: open the map, type the
            // destination, press Go. Nothing here reaches past the UI.
            hopDone=true;showMap(true);
            field("longitude",hopLon);field("latitude",hopLat);
            hopArmed=true;hopWait=0;
            return;
        }
        showMap(true);testResume=true;
    }
    void sayBodies() {
        auto* physics=engine.sceneTree().world().physics();
        if(!physics)return;
        // The densest tile's static bodies, what kTileStaticBodies answers for.
        size_t trunks=0,densest=0;std::string densestKey;
        for(const auto& [key,t]:loaded) {
            if(auto* body=t.node->findByPath("Tree trunks"))trunks+=body->children().size();
            size_t statics=0;
            std::function<void(const saida::Node&)> count=[&](const saida::Node& n) {
                // A car's box is a traffic slot's body, counted in kTileMovingBodies.
                if(auto* body=dynamic_cast<const saida::StaticBodyNode*>(&n);
                   body&&!body->bodyId().IsInvalid()&&body->name()!="Vehicle collider")++statics;
                for(const auto& c:n.children())count(*c);
            };
            count(*t.node);
            if(statics>densest){densest=statics;densestKey=key;}
        }
        saida::Log::info("[World physics] ",physics->bodyCount()," bodies of ",physics->capacity().bodies,
                         " (kPhysicsBodies) across ",loaded.size()," tiles, ",trunks," tree trunks among them; densest tile ",
                         densestKey," holds ",densest," static bodies of ",kTileStaticBodies," (kTileStaticBodies)");
    }
    // A tile's trunks are one compound body: the query the car stops on and
    // the feet's solver meet every tile's first mounted trunk, a metre off the ground.
    bool trunksSolid() {
        auto* physics=engine.sceneTree().world().physics();
        size_t checked=0;
        for(const auto& [key,t]:loaded) {
            auto* body=dynamic_cast<saida::StaticBodyNode*>(t.node->findByPath("Tree trunks"));
            if(!body||body->children().empty())continue;
            // Trunks join as their trees stream: a body whose first trees
            // arrived this frame is built at the next sync, refused never.
            if(body->bodyId().IsInvalid()&&!body->bodyRefused())continue;
            auto* shape=dynamic_cast<saida::CollisionShapeNode*>(body->children().front().get());
            const glm::vec3 at(body->worldTransform()*glm::vec4(shape->offset-glm::vec3(0,1.f,0),1.f));
            bool met=false;
            if(physics)for(auto id:physics->overlapSphere(at,.32f,obstacleFilter()))met=met||id==body->bodyId();
            if(!met) {
                saida::Log::error("[World E2E] FAIL trunks: ",key,"'s first trunk is not solid (",
                                  body->children().size()," trunks, body ",body->bodyRefused()?"refused":body->bodyId().IsInvalid()?"unbuilt":"built",")");
                return false;
            }
            ++checked;
        }
        saida::Log::info("[World E2E] trunks solid in ",checked," tiles");
        return true;
    }
    // The height the walk and the car follow is the ground drawn and collided
    // with. A patch that disagreed by 30 cm stopped the car on open grass.
    // Coastal tiles are left out: the sea's levelling (gen/harbours
    // Cells::adjust) moves the drawn ground and not the manifest's grid.
    bool groundMatchesDrawing() {
        auto* physics=engine.sceneTree().world().physics();if(!physics)return true;
        const Loaded* t=tile(lon,lat);if(!t||t->ocean)return true;
        if(std::find(t->water.begin(),t->water.end(),uint8_t(2))!=t->water.end()) {
            saida::Log::info("[World E2E] ground check skipped: the tile meets the sea");return true;
        }
        double worst=0,worstLon=0,worstLat=0,worstHeight=0;size_t samples=0;
        for(int i=1;i<16;++i)for(int j=1;j<16;++j) {
            const double x=t->west+(t->east-t->west)*i/16.,y=t->south+(t->north-t->south)*j/16.;
            const double h=terrainHeight(*t,x,y);
            const auto hit=physics->raycast(glm::vec3(origin.local(ecef(x,y,h+2.))),{0,-1,0},4.f,obstacleFilter());
            if(!hit.hit)continue;
            auto* node=static_cast<saida::CollisionObjectNode*>(physics->bodyUserData(hit.body));
            if(!node||node->name().rfind("Ground",0)!=0)continue; // a street, a roof or a trunk is above it
            const double error=std::abs(double(hit.point.y)-origin.local(ecef(x,y,h)).y);
            if(error>worst){worst=error;worstLon=x;worstLat=y;worstHeight=h;}++samples;
        }
        if(worst>.05) {
            saida::Log::error("[World E2E] FAIL ground: the height followed is ",worst,
                              " m off the ground drawn (",samples," samples) at ",worstLon,",",worstLat,
                              " followed=",worstHeight," grid=",gridHeight(*t,worstLon,worstLat),
                              " onDeck=",onDeck(*t,worstLon,worstLat));
            return false;
        }
        saida::Log::info("[World E2E] ground followed within ",worst," m of the ground drawn (",samples," samples)");
        return true;
    }
    // Two tiles meet where their ground meets (gen/seams): a step at an edge
    // stopped the car on a bare field. Well under the 0.28 m the car meets as
    // an obstacle, with room for what smoothing a staggered seam leaves on a
    // quay wall. Tiles meeting the sea are left out, as above, and the ground
    // dug under a bridge is measured apart: each tile solves its bridges on
    // its own window (gen/bridges), so a dig on a seam may differ by a little.
    bool seamsClosed() {
        auto coastal=[](const Loaded& t){return t.ocean||std::find(t.water.begin(),t.water.end(),uint8_t(2))!=t.water.end();};
        // A dug node bends the ground out to the next node, a grid step away.
        auto dug=[](const Loaded& t,double x,double y){
            const double step=(t.north-t.south)*kMetresPerDegree/std::max(1,t.gridSize-1);
            for(const auto& d:t.data.value("groundDigs",json::array()))
                if(std::hypot((x-d[0].get<double>())*kMetresPerDegree*std::cos(y*rad),(y-d[1].get<double>())*kMetresPerDegree)<d[2].get<double>()+step)return true;
            return false;
        };
        // A tile whose next version is already cooked is on its way out: its
        // ground was joined to neighbours that have since moved on.
        auto leaving=[&](const std::string& key,const Loaded& t){
            const auto current=service->find(t.served->cooked.tile);
            return incoming.count(key)||(current&&current->serial!=t.served->serial);
        };
        double worst=0,worstDug=0;std::string where;size_t seams=0,changing=0;
        for(const auto& [ka,a]:loaded)for(const auto& [kb,b]:loaded) {
            if(&a==&b||a.gridSize<2||b.gridSize<2||coastal(a)||coastal(b))continue;
            const bool rows=std::abs(a.north-b.south)<1e-9,sides=std::abs(a.east-b.west)<1e-9&&std::abs(a.south-b.south)<1e-9;
            if(!rows&&!sides)continue;
            const double from=rows?std::max(a.west,b.west):a.south,to=rows?std::min(a.east,b.east):a.north;
            if(to-from<1e-9)continue;
            if(leaving(ka,a)||leaving(kb,b)){++changing;continue;}
            ++seams;
            for(int k=0;k<=200;++k) {
                const double s=from+(to-from)*k/200.,x=rows?s:a.east,y=rows?a.north:s;
                const double step=std::abs(gridHeight(a,x,y)-gridHeight(b,x,y));
                if(dug(a,x,y)||dug(b,x,y)){worstDug=std::max(worstDug,step);continue;}
                if(step>worst) {
                    worst=step;
                    auto said=[](const Loaded& t){return t.data.value("elevationSource",std::string())+", moved "+
                                                         t.data.value("groundSeams",json::object()).dump();};
                    where=ka+" ("+said(a)+") | "+kb+" ("+said(b)+") at "+std::to_string(x)+", "+std::to_string(y);
                }
            }
        }
        if(worst>.15) {
            saida::Log::error("[World E2E] FAIL seams: a ",worst," m step between ",where," (",seams," seams)");
            return false;
        }
        if(worstDug>kCarKerb) {
            saida::Log::error("[World E2E] FAIL seams: a ",worstDug," m step under a bridge dug differently by two tiles");
            return false;
        }
        saida::Log::info("[World E2E] ",seams," seams closed within ",worst," m; under a dug bridge, within ",worstDug,
                         " m; ",changing," pairs left out while a newer cook replaces one side");
        return true;
    }
    // True when the smoke has just failed on a refused body.
    bool checkPhysics() {
        auto* physics=engine.sceneTree().world().physics();
        if(!physics||physics->refusedBodies()==physicsRefused)return false;
        physicsRefused=physics->refusedBodies();
        saida::Log::error("[World physics] ",physicsRefused," bodies refused: ",physics->bodyCount()," of ",
                          physics->capacity().bodies," in use (kPhysicsBodies) across ",loaded.size()," tiles");
        if(!smoke)return false;
        saida::Log::error("[World E2E] FAIL physics: the world refused ",physicsRefused," bodies");
        testFailed=true;engine.sceneTree().quit();return true;
    }
    void update(float delta) {
        SAIDA_PROFILE_SCOPE("World/Update");
        SAIDA_PROFILE_COUNTER("World/PerfPhase", !perfSmoke.empty()&&perfReady?(smokeDriving?2:smokeStarted?1:0):0);
        const auto now=std::chrono::steady_clock::now();
        const double frameSeconds=lastFrame.time_since_epoch().count()
            ?std::chrono::duration<double>(now-lastFrame).count():0.;
        if(!arrivalSaid&&lastFrame.time_since_epoch().count()) {
            const double frameMs=std::chrono::duration<double,std::milli>(now-lastFrame).count();
            ++arrivalFrames;arrivalHitches+=frameMs>33.;arrivalWorst=std::max(arrivalWorst,frameMs);
            if(frameMs>50.) {
                const double ours=cost.stream+cost.parts+cost.warm+cost.props+cost.distant+cost.world;
                saida::Log::info("[World frame] ",frameMs," ms: stream ",cost.stream," parts ",cost.parts," warm ",cost.warm,
                                 " props ",cost.props," distant ",cost.distant," world ",cost.world,
                                 " engine ",frameMs-ours);
            }
            if(std::chrono::duration<double>(now-goStarted).count()>10.) {
                saida::Log::info("[World streaming] arrival: ",arrivalHitches," of ",arrivalFrames,
                                 " frames over 33 ms, worst ",arrivalWorst," ms");
                sayBodies();
                arrivalSaid=true;
            }
        }
        lastFrame=now;
        const bool debugKeys[]={held(GLFW_KEY_R),held(GLFW_KEY_V),held(GLFW_KEY_Y),held(GLFW_KEY_M),held(GLFW_KEY_T)};
        samplePerformance(frameSeconds);handlePerformanceKeys(debugKeys);placePerformanceCanvas();
        cost=FrameCost{};
        if(checkPhysics())return;
        if(generation!=ui->documentGeneration()||listeners.empty()) {
            generation=ui->documentGeneration();listeners.clear();
            written.clear();
            for(auto id:{"go","destination-go","resume","street-map","reset-map","paris","tokyo","newyork","lawrence","cape","sydney","pole",
                         "city-choice-0","city-choice-1","city-choice-2","city-choice-3","city-choice-4",
                         "options","options-back","forced-apply","forced-clear",
                         "weather-real","weather-clear","weather-light-rain","weather-storm",
                         "toaster-off","toaster-on",
                         "forced-preset-8","forced-preset-12","forced-preset-17","forced-preset-22"})
                if(auto* e=ui->findElementById(id)){e->AddEventListener("click",this);
                    e->AddEventListener("mousedown",this);listeners.push_back(e);}
            for(auto id:{"map","menu"})if(auto* e=ui->findElementById(id)) {
                if(std::string(id)=="map")for(auto type:{"mousedown","mousescroll","dblclick"})e->AddEventListener(type,this);
                for(auto type:{"mousemove","mouseup"})e->AddEventListener(type,this);
                listeners.push_back(e);
            }
            if(!listeners.empty()) {
                select(pickLon,pickLat);
                const bool menuSmoke=std::getenv("R1WORLD_MENU_SMOKE")!=nullptr;
                if((smoke||menuSmoke)&&!smokeZoom())return;
                if(menuSmoke){engine.sceneTree().quit();return;}
                if(smoke) {
                    double sx=pickLon,sy=pickLat;
                    testClick("map");
                    double expectedLon=10./r1::MapView::width*360.-180.,expectedLat=90.-10./r1::MapView::height*180.;
                    if(std::abs(pickLon-expectedLon)>1e-5||std::abs(pickLat-expectedLat)>1e-5){
                        saida::Log::error("[World E2E] FAIL map click coordinate conversion");testFailed=true;engine.sceneTree().quit();return;
                    }
                    select(sx,sy);zoomMap(2);
                    const r1::MapPoint zoomClick=r1::MapView(sx,sy,mapZoomLevel()).pointAt(10,10);
                    testClick("map");
                    if(std::abs(pickLon-zoomClick.lon)>1e-5||std::abs(pickLat-zoomClick.lat)>1e-5){
                        saida::Log::error("[World E2E] FAIL detailed map click coordinate conversion");
                        testFailed=true;engine.sceneTree().quit();return;
                    }
                    select(sx,sy);testClick("street-map",false);
                    if(mapZoomLevel()!=17){
                        saida::Log::error("[World E2E] FAIL street map zoom level");testFailed=true;engine.sceneTree().quit();return;
                    }
                    zoomMap(1);
                    showCityChoices({{"Paris","Paris · France",2.3522,48.8566}});
                    testClick("city-choice-0",false);
                    if(std::abs(pickLon-2.3522)>1e-5||std::abs(pickLat-48.8566)>1e-5||
                       value("longitude")!=number(2.3522)||value("latitude")!=number(48.8566)) {
                        saida::Log::error("[World E2E] FAIL city suggestion did not fill destination");
                        testFailed=true;engine.sceneTree().quit();return;
                    }
                    field("city-query",std::string());select(sx,sy);
                    // The press alone must start the journey.
                    goCount=0;testClick("go",false);
                    if(!pending||goCount!=1){saida::Log::error("[World E2E] FAIL Go press, pending=",pending," calls=",goCount);testFailed=true;engine.sceneTree().quit();return;}
                    // A full press+release must still act exactly once.
                    testClick("go");
                    if(goCount!=2){saida::Log::error("[World E2E] FAIL Go press+release ran ",goCount," times");testFailed=true;engine.sceneTree().quit();return;}
                    saida::Log::info("[World E2E] map click and Go passed (press-only and press+release)");
                    // Last: the screen it hides is still laid out over the menu
                    // until the next frame, as after any menu change (hopArmed).
                    if(!smokeOptions())return;
                }
            }
        }
        clock+=delta;
        applyForcedTime();
        updateCityLookup();
        updateGlobe(saida::Time::unscaledDelta());
        if(menu&&globe){const bool map=menuTiles.pump(),imagery=imageryTiles.pump();if(map||imagery)renderMapTiles();}
        if(smoke && pending && refused==tileAt(pickLon,pickLat).key()) {
            saida::Log::error("[World E2E] FAIL ",refused," did not fit the resident vertex budget");
            testFailed=true;engine.sceneTree().quit();return;
        }
        if(smoke){testElapsed+=delta;if(testElapsed>(trafficValidation_?trafficValidationSeconds_*3.+120.:hopWanted?420:smokeFly?300:180)){saida::Log::error("[World E2E] FAIL data timeout");testFailed=true;engine.sceneTree().quit();return;}}
        if(testResume){
            resumeWait+=delta;
            if(resumeWait>.25){
                testClick("resume");double d=glm::length(ecef(lon,lat,alt)-smokeStart);
                testFailed=d<=2||menu||!smokeRan||!smokeJumped;
                saida::Log::info("[World E2E] ",testFailed?"FAIL":"PASS"," walking=",d,"m resident=",loaded.size()," resume=",!menu);
                engine.sceneTree().quit();
            }
            return;
        }
        if(hopArmed) {
            // A menu that has just been shown has not been laid out yet, and an
            // element's absolute offset is still the hidden document's until it
            // has. Clicking in that frame aims at the top-left corner and hits
            // nothing -- which looks exactly like a dead button, and is not one.
            hopWait+=delta;
            if(hopWait>.3) {
                hopArmed=false;testClick("go",false);
                if(!pending){saida::Log::error("[World E2E] FAIL the second Go did nothing");testFailed=true;engine.sceneTree().quit();return;}
                saida::Log::info("[World E2E] teleporting to ",hopLon,", ",hopLat);
            }
        }
        // Escape closes an open cheat line rather than opening the map
        // (World::updateCheats); M is a letter typed in it.
        bool key=(!performanceDebug&&held(GLFW_KEY_M))||engine.window().keyDown(GLFW_KEY_ESCAPE);
        if(key&&!wasMenuKey&&!cheatOpen){if(optionsOpen)showOptions(false);else if(menu&&playing){pending=false;warming=false;request(lon,lat);showMap(false);}else showMap(true);}wasMenuKey=key;
        poll+=delta;hud+=delta;
        if(poll>(pending?.016:.10)){poll=0;if(pending||playing||warming)cost.stream=timed([&]{stream();});}
        cost.parts=timed([&]{uploadParts();});
        updateBuildingLods();
        if(!playing)cost.warm=timed([&]{warm();});
        updateRadio(delta);
        if(!playing||menu){updateRain(delta);return;}
        updateCheats(std::min(.05,double(delta)));
        updateInteriors(delta);
        const bool fast=(driving&&std::abs(carSpeed)>kFastDetail)||(sailing&&std::abs(boat.speed)>kFastDetail)
                        ||(piloting&&std::abs(plane.speed)>kFastDetail);
        seaTime+=delta;
        // §5, "le détail suit la vitesse": above 15 km/h the plan drops L5 --
        // mobilier, clutter, détail de façade -- and shrinks the rest. Street
        // furniture is streamed in rather than drawn from a pool, so dropping
        // it means not spending the frame's 2 ms importing what the player is
        // about to leave behind. It resumes the moment he slows down. The
        // signs do not wait: they are what a driver reads (gen/predict.hpp).
        ring=nearby(lon,lat);
        const uint64_t moved=engine.sceneTree().world().indexedNodesTotal()-indexedAtFrame;
        indexedAtFrame=engine.sceneTree().world().indexedNodesTotal();
        churn+=double(moved);churnFrames+=1;dirtyFrames+=moved?1:0;
        cost.props=timed([&]{streamProps(fast);});
        cost.distant=timed([&]{updateFar();updateFarPack();updateFarRelief();});
        cost.world=timed([&]{updateNature(fast?.55:1.);updateTraffic(delta);updateBeach(delta);updateCrowd(delta);updateSea(delta);bendGrass();});
        conditionsRead+=delta;
        if(conditionsRead>.5){conditionsRead=0;readConditions();}
        if((farPack.node!=nullptr)!=fogFar)applyWeather();
        // The view reaches the horizon where something is drawn out to it:
        // 4.7 km at eye height, 113 km from a thousand metres, never past
        // the far pack. Elsewhere it stops at the 5 km haze, as it always has.
        if(toasterMode) {
            camera->farZ=800.f;
        } else {
            camera->farZ=farPack.node?float(std::clamp(std::sqrt(2.*r1::kRMean*std::max(2.,alt+10.))*1.2+1500.,5000.,r1::kFarPackRadius+5000.)):5000.f;
            // Out to the farthest ring drawn: a summit 200 km off stands above
            // the horizon from a beach, and the haze, not the far plane, hides it.
            if(farRelief.node&&farRelief.node->visible()&&farRelief.reach>0)
                camera->farZ=std::max(camera->farZ,float(farRelief.reach+2000.));
        }
        // The sky takes the haze of the farthest ground drawn, so the horizon
        // is one gradient rather than a silhouette between fogged land and
        // clear sky (engine fogSkyDistance).
        engine.sceneTree().world().settings().fogSkyDistance=camera->farZ;
        updateSnow(delta);
        if(checkStanding)keepStanding();
        updateSnowCover();
        engine.window().setCursorCaptured(true);
        auto mouse=saida::Input::mouseDelta();
        // At the wheel the mouse does not steer -- the car does -- so it turns
        // the head instead: a free yaw around the car, which is what you want
        // at a junction and what the first drive was missing entirely. It
        // recentres behind the car once the mouse stops and the car is rolling,
        // so the default view is still the road ahead.
        if(driving||sailing||piloting) {
            if(std::abs(mouse.x)>1e-4){lookYaw=wrap(lookYaw+mouse.x*.12);lookIdle=0;}
            else lookIdle+=delta;
            if(!aiming&&lookIdle>.6&&std::abs(driving?carSpeed:sailing?boat.speed:plane.speed)>2.)
                lookYaw-=lookYaw*(1-std::exp(-2.2*std::min(.05,double(delta))));
        } else yaw+=mouse.x*.12;
        pitch=std::clamp(pitch-mouse.y*.12,-65.,25.);
        double f=(held(GLFW_KEY_W)||held(GLFW_KEY_Z)?1.:0.)-(held(GLFW_KEY_S)?1.:0.);
        double r=(held(GLFW_KEY_D)?1.:0.)-(held(GLFW_KEY_A)||held(GLFW_KEY_Q)?1.:0.);
        if(smokeStarted&&worldCapture.pngPath.empty()){
            if(!perfSmoke.empty()&&!perfReady) {
                // Measure motion after the initial import, with normal real
                // time and presentation. Later streaming remains included.
                const bool ready=engine.resources().assetLoadsSettled()&&std::all_of(ring.begin(),ring.end(),[&](Tile t){
                    const auto i=loaded.find(t.key());if(i==loaded.end())return false;
                    const auto& resident=i->second;
                    return resident.geography&&!resident.data.value("provisional",false)&&buildingsReady(resident)
                        &&resident.nextPart>=std::any_cast<const PreparedTile&>(resident.served->prepared).parts.size()
                        &&resident.nextProp==resident.props.size();
                });
                perfReadyWait=ready?perfReadyWait+delta:0.;
                if(perfReadyWait>=2.){perfReady=true;saida::Log::info("[World perf] warm ",perfSmoke," starts, preset=",toasterMode?"toaster":"standard");}
            }
            if(perfSmoke.empty()||perfReady){f=1;r=0;smokeWalk+=std::min(.05,double(delta));}
            else f=r=0;
            if(qualitySwitchSmoke&&perfReady&&qualitySwitches<24) {
                qualitySwitchWait+=delta;
                if(qualitySwitchWait>=2.) {
                    qualitySwitchWait=0;
                    setToasterMode(!toasterMode);
                    ++qualitySwitches;
                    saida::Log::info("[World quality test] switch=",qualitySwitches," private_mb=",int(privateMegabytes()));
                }
            }
        }
        if(wadeTo&&smokeStarted) {
            const auto to=origin.local(ecef(wadeTo->x,wadeTo->y,alt))-origin.local(ecef(lon,lat,alt));
            yaw=std::atan2(to.x,-to.z)/rad;
        }
        if(idleSmoke&&smokeStarted&&smokeWalk>1.) {
            f=r=0;
            if(!idleSmokeStopped){idleSmokeStopped=true;idleSmokeStart=ecef(lon,lat,alt);}
        }
        if(playerDown){f=r=0;}  // on the ground, he does not walk
        if(retailTest())f=r=0;
        if(performanceSmoke){f=r=0;smokeStarted=false;}
        if(trafficValidation_){f=r=0;smokeStarted=false;}
        if(smokeApproach) {
            auto to=origin.local(ecef(carLon,carLat,carAlt))-origin.local(ecef(lon,lat,alt));
            yaw=std::atan2(to.x,-to.z)/rad;f=1;r=0;
            smokeApproachTime+=std::min(.05,double(delta));
        }
        double length=std::hypot(f,r),dt=std::min(.05,double(delta));
        // One door key for both directions, and the refusals are inside
        // enterCar/leaveCar so that every one of them says something.
        bool doorKey=held(GLFW_KEY_F);
        if(doorKey&&!wasEnterKey&&!playerDown) {
            if(driving)leaveCar();
            else if(sailing){if(hopAboard()==HopResult::NoTarget)leaveBoat();}
            else if(piloting)leaveAircraft();
            else if(swimming){
                if(!enterBoat()&&!enterAircraft())text("stream-status","No boat in reach — swim to one.");
            }
            else {
                // The nearest of a car, a boat and an aircraft: one key for every vehicle.
                Reach carReach;BoatReach boatReach;PlaneReach planeReach;double carAway=0,boatAway=0,planeAway=0;
                const bool car=nearestCar(carReach,carAway),vessel=nearestBoat(boatReach,boatAway);
                const bool aircraft=nearestAircraft(planeReach,planeAway);
                if(aircraft&&(!car||planeAway<carAway)&&(!vessel||planeAway<boatAway))enterAircraft();
                else if(vessel&&(!car||boatAway<carAway))enterBoat();
                else if(!enterCar()) {
                    // Pressing the door key from across the street is a question,
                    // and it gets an answer rather than silence.
                    if(carParked||!parked.empty()||trafficLive()>0)
                        text("stream-status",car
                             ?"Door blocked."
                             :"No vehicle in reach — get closer.");
                }
            }
        }
        wasEnterKey=doorKey;
        bool moving=false;
        footEast=footNorth=0;
        stagger=std::max(0.,stagger-dt);
        // His feet are a body while he is: not at the wheel, at the helm or
        // at the controls, where people meet the vehicle instead.
        // Down, his body is the ragdoll's and his feet leave the physics.
        if(feet->enabled()!=(player->enabled()&&!playerDown))feet->setEnabled(player->enabled()&&!playerDown);
        if(driving) {
            if(smokeDriving){f=smokeBrake?-1.:1.;r=0;}
            driveCar(dt,f,r,held(GLFW_KEY_SPACE)&&!smokeDriving);
            moving=std::abs(carSpeed)>.2;
        } else if(sailing) {
            if(smokeSailing){f=smokeSailBrake?-1.:1.;r=0;}
            sailBoat(dt,f,r);
            moving=std::abs(boat.speed)>.2;
        } else if(piloting) {
            double up=(held(GLFW_KEY_SPACE)||held(GLFW_KEY_UP)?1.:0.)
                     -(held(GLFW_KEY_LEFT_SHIFT)||held(GLFW_KEY_LEFT_CONTROL)||held(GLFW_KEY_DOWN)?1.:0.);
            if(smokeFly){f=smokeF;r=smokeR;up=smokeUp;}
            if(plane.helicopter())flyHelicopter(dt,f,r,up);else flyPlane(dt,f,r,up);
            lon=plane.lon;lat=plane.lat;alt=plane.alt;yaw=plane.yaw;
            moving=std::abs(plane.speed)>.2||plane.airborne;
            request(lon,lat);
        } else if(swimming) {
            swimTime+=dt;
            if(smokeSwimming){f=1.;r=0.;length=1.;}
            if(smokeWaterSpawn){f=1.;r=0.;length=1.;smokeWalk+=dt;}
            if(length>0) {
                const double speed=held(GLFW_KEY_LEFT_SHIFT)?2.2:1.5;
                const double east=(sin(yaw*rad)*f+cos(yaw*rad)*r)/length*speed*dt;
                const double north=(cos(yaw*rad)*f-sin(yaw*rad)*r)/length*speed*dt;
                const auto next=onward(lon,lat,east,north);
                if(tile(next.x,next.y)) {
                    if(deepWater(next.x,next.y)) {
                        lon=next.x;lat=next.y;alt=waterLevel(lon,lat);moving=true;
                        swimHeading=wrap(std::atan2(east,north)/rad);
                    } else if(!obstructed(next.x,next.y,alt)) {
                        const bool wading=onWater(next.x,next.y);
                        lon=next.x;lat=next.y;alt=height(lon,lat,alt);swimming=false;moving=true;
                        text("stream-status",wading?"You can stand.":"Ashore.");
                        if(wading)saida::Log::info("[World swim] standing at ",lon,", ",lat," in ",waterLevel(lon,lat)-alt," m of water");
                        else saida::Log::info("[World swim] ashore at ",lon,", ",lat);
                    }
                    if(moving)swimHeading=wrap(std::atan2(east,north)/rad);
                } else text("stream-status","More data is on its way… swimming held at the edge of the area.");
            }
            if(moving)request(lon,lat);
        } else if(length>0&&jumpOffset<3.) {  // falling from higher, he cannot steer
            double speed=held(GLFW_KEY_LEFT_SHIFT)&&!aiming?7.:2.8;
            // A stride broken by running into someone (World::staggerPlayer).
            if(stagger>0)speed*=.35;
            double east=(sin(yaw*rad)*f+cos(yaw*rad)*r)/length*speed*dt;
            double north=(cos(yaw*rad)*f-sin(yaw*rad)*r)/length*speed*dt;
            footEast=east/dt;footNorth=north/dt;
            // People are bodies: he stops at them and slides round them.
            auto next=moveFeet(east,north,dt);
            if(tile(next.x,next.y)) {
                // What his feet rest on there: the ground, a deck, or whatever
                // the character solver stood him on.
                const double support=std::max(height(next.x,next.y,alt),next.z);
                if(deepWater(next.x,next.y,support)) {
                    lon=next.x;lat=next.y;alt=waterLevel(lon,lat);
                    swimming=true;swimTime=0;swimLean=0;
                    swimHeading=wrap(std::atan2(east,north)/rad);
                    jumpOffset=jumpVelocity=0;moving=true;
                    text("stream-status","In the water — swim for the shore.");
                    const auto bed=waterBed(lon,lat);
                    if(bed)saida::Log::info("[World swim] entered water at ",lon,", ",lat," depth=",waterLevel(lon,lat)-*bed," m");
                    else saida::Log::info("[World swim] entered water at ",lon,", ",lat," (no known bed: deep)");
                } else {
                    lon=next.x;lat=next.y;alt=support;moving=true;
                    auto facing=glm::angleAxis(float(-std::atan2(east,north)),glm::vec3(0,1,0));
                    player->transform().rotation=glm::slerp(player->transform().rotation,facing,float(1-std::exp(-16*dt)));
                }
            }else text("stream-status","More data is on its way… movement held at the edge of the area.");
            request(lon,lat);
        } else if(jumpOffset<=0) {
            // Standing still, he is still a body: whoever walks into him
            // meets his feet, and a shoulder moves him a little.
            const auto nudged=moveFeet(0,0,dt);
            if(tile(nudged.x,nudged.y)) {
                const double support=std::max(height(nudged.x,nudged.y,alt),nudged.z);
                if(!deepWater(nudged.x,nudged.y,support)){lon=nudged.x;lat=nudged.y;alt=support;}
            }
        }
        // Not while he lies in the ragdoll: its bodies follow his node, which
        // the rebase does not carry. He is up again in a few seconds.
        if(!playerDown&&glm::length(origin.local(ecef(lon,lat,alt)))>kRebaseDistance)rebaseOrigin();
        if(!driving&&!sailing&&!piloting) {
            if(swimming) {
                jumpOffset=jumpVelocity=0;wasJump=false;
                swimLean+=(double(moving)-swimLean)*(1-std::exp(-5*dt));
                const double lean=35.*rad*swimLean;
                const double heave=.035*std::sin(swimTime*4.);
                const double rootBelow=playerHeight*(std::cos(lean)-kSwimHeadAbove);
                player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt-rootBelow+heave)));
                player->transform().rotation=glm::angleAxis(float(-swimHeading*rad),glm::vec3(0,1,0))
                                             *glm::angleAxis(float(-lean),glm::vec3(1,0,0));
                playPlayerClip(moving?"run":"idle");
            } else {
                bool jump=held(GLFW_KEY_SPACE);
                if(!idleSmoke&&smokeStarted&&worldCapture.pngPath.empty()&&smokeWalk>.6&&smokeWalk<.8)jump=true;
                if(jump&&!wasJump&&jumpOffset<=0)jumpVelocity=std::sqrt(2*22.*1.5);
                wasJump=jump;
                const double previous=jumpOffset;
                if(jumpVelocity-22*dt>-kFallTerminal){jumpOffset+=jumpVelocity*dt-11*dt*dt;jumpVelocity-=22*dt;}
                else{jumpVelocity=-kFallTerminal;jumpOffset+=jumpVelocity*dt;}
                if(jumpOffset<=0) {
                    // Come down on water, from an aircraft: he swims.
                    if(jumpVelocity<-5.&&tile(lon,lat)&&deepWater(lon,lat,alt)) {
                        swimming=true;swimTime=0;swimLean=0;swimHeading=yaw;alt=waterLevel(lon,lat);
                        text("stream-status","In the water — swim for the shore.");
                    } else if(jumpVelocity<0)landed(-jumpVelocity);
                    jumpOffset=0;jumpVelocity=0;
                }
                if(jumpOffset>0) {
                    feet->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+previous+.06)));
                    const auto end=feet->moveAndSlide({0,float((jumpOffset-previous)/dt),0},float(dt));
                    const double resolved=end.y-origin.local(ecef(lon,lat,alt+.06)).y;
                    if(resolved<jumpOffset-.01&&jumpVelocity>0)jumpVelocity=0;
                    jumpOffset=std::max(0.,resolved);
                    if(jumpVelocity<0&&feet->isOnFloor()) {
                        landed(-jumpVelocity);
                        alt+=jumpOffset;jumpOffset=jumpVelocity=0;
                    }
                }
                player->transform().position=glm::vec3(origin.local(ecef(lon,lat,alt+jumpOffset+.06)));
                const bool sprint=moving&&held(GLFW_KEY_LEFT_SHIFT);
                playPlayerClip(jumpOffset>0?"jump":sprint?"sprint":moving?"run":"idle");
            }
        }
        updateSinkingCar(dt);
        placeCar();
        placeBoats();
        placeAircraft(dt);
        updateCombat(dt);
        if(smokeStarted&&worldCapture.pngPath.empty()){
            smokeRan=smokeRan||(moving&&animators.front()->currentClip()=="run");
            smokeJumped=smokeJumped||(jumpOffset>.5&&animators.front()->currentClip()=="jump");
        }
        // At the controls of a plane the view pitches with the nose, a little.
        const double viewPitch=std::clamp(pitch+(piloting&&!plane.helicopter()?plane.pitch*.6:0.),-80.,40.);
        camera->transform().rotation=glm::angleAxis(float(-(yaw+lookYaw)*rad),glm::vec3(0,1,0))*glm::angleAxis(float(viewPitch*rad),glm::vec3(1,0,0));
        // A car is longer than a man and moves twice as fast, so the camera
        // stands further back -- far enough to see the bonnet turn.
        // A boat's camera stands off by its own length: a container ship is
        // two hundred metres of hull, and eight metres behind its bridge is
        // inside it.
        // Aiming, the camera closes in over his right shoulder.
        aimBlend+=((aiming?1.:0.)-aimBlend)*(1-std::exp(-12*dt));
        const double maxFollow=sailing?std::clamp(boat.length*1.1+6.,9.,230.)
                              :piloting?plane.type->length*.9+10.
                              :ridingBike()?5.2-1.8*aimBlend
                              :driving?std::max(kDrivingFollow,vehicleSpec(*car).length*.7+5.)
                              :kOnFootFollow+(kAimFollow-kOnFootFollow)*aimBlend;
        const double eye=sailing?std::clamp(boat.length*.12+2.,2.,26.):piloting?plane.type->height*.75+1.5:driving?vehicleSpec(*car).height+.5:swimming?playerHeight*1.17
                        :playerDown?playerHeight*.35:playerHeight*(.89+.06*aimBlend);
        const glm::vec3 anchor=sailing&&boat.node?boat.node->transform().position
                              :piloting&&plane.node?plane.node->transform().position
                              :driving?car->transform().position
                              :playerDown?downAnchor():player->transform().position;
        const glm::vec3 target=anchor+glm::vec3(0,eye,0)
            +camera->transform().rotation*glm::vec3(float(kAimShoulder*aimBlend),0.f,0.f);
        const glm::vec3 backward=camera->transform().rotation*glm::vec3(0,0,1);
        double clear=maxFollow;
        if(auto* physics=engine.sceneTree().world().physics()) {
            const auto hit=physics->raycast(target,backward,float(maxFollow),obstacleFilter());
            if(hit.hit)clear=std::max(.15,double(hit.distance)-.3);
        }
        // Keep the camera above the geographic surface and inside loaded tiles.
        for(double d=.25;d<=clear;d+=.25){
            auto q=onward(lon,lat,backward.x*d,-backward.z*d);
            if(!tile(q.x,q.y)&&piloting)continue;  // in the air, unstreamed ground ahead is no wall
            if(!tile(q.x,q.y)||height(q.x,q.y,alt+jumpOffset)+.3>alt+jumpOffset+.06+eye+backward.y*d){clear=std::max(.15,d-.25);break;}
        }
        followDistance=clear<followDistance?clear:followDistance+(clear-followDistance)*(1-std::exp(-7*dt));
        camera->transform().position=target+backward*float(followDistance);
        if(smoke&&!worldCapture.pngPath.empty()) {
            if(captureView.set) {
                // The viewpoint is relative to where the player stands now, not
                // to the origin of the spawn: a first visit spawns on ground
                // that the surveyed relief later raises (Kyoto: 0 m, then the
                // hillside), and a camera 30 m above the old ground is inside
                // the hill.
                glm::vec3 at=player->transform().position;
                if(captureAltitude)at.y+=float(*captureAltitude-alt);
                glm::vec3 position=at+glm::vec3(captureView.position[0],captureView.position[1],captureView.position[2]);
                glm::vec3 target=at+glm::vec3(captureView.target[0],captureView.target[1],captureView.target[2]);
                if(captureGeo) {
                    // A surveyed inspection camera may overlap the spawn.
                    // Keep the player active for streaming, hide only its mesh.
                    player->setVisible(false);
                    const auto& geo=*captureGeo;
                    const double eye=captureAltitude?*captureAltitude:height(geo[0],geo[1])+geo[2];
                    Frame photo(geo[0],geo[1],eye);
                    position=origin.local(photo.origin);
                    const glm::dvec3 direction(captureView.target[0]-captureView.position[0],
                        captureView.target[1]-captureView.position[1],captureView.target[2]-captureView.position[2]);
                    target=origin.local(photo.origin+photo.basis*direction);
                }
                camera->transform().position=position;
                camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
            }
            if(std::getenv("R1WORLD_CAPTURE_VEHICLE")) {
                r1::VehicleRuntime::synchronize(*car);
                const auto& spec=vehicleSpec(*car);const auto rotation=car->transform().rotation;
                const auto position=car->transform().position+rotation*glm::vec3(float(spec.width*1.8),float(spec.height*1.4),float(-spec.length*1.2));
                const auto target=car->transform().position+rotation*glm::vec3(0,float(spec.height*.5),0);
                player->setVisible(false);camera->transform().position=position;
                camera->transform().rotation=glm::quatLookAt(glm::normalize(target-position),glm::vec3(0,1,0));
            }
            if(motorcycleCaptureStarted&&car) {
                r1::VehicleRuntime::synchronize(*car);
                const auto rotation=car->transform().rotation;
                const auto at=car->transform().position;
                const bool armed=std::getenv("R1WORLD_CAPTURE_MOTORCYCLE_AIM")!=nullptr;
                camera->transform().position=at+rotation*(armed?glm::vec3(-1.8f,1.5f,2.6f):glm::vec3(1.7f,1.3f,-1.7f));
                camera->transform().rotation=glm::quatLookAt(glm::normalize(at+glm::vec3(0,.95f,0)-camera->transform().position),glm::vec3(0,1,0));
                if(std::getenv("R1WORLD_CAPTURE_MOTORCYCLE_AIM")) {
                    selectWeapon(r1::combat::Weapon::Pistol);setAiming(true);poseArms();bikeHandlePose();
                }
            }
            if(std::getenv("R1WORLD_CAPTURE_SIGNAL"))frameTrafficSignal();
            auto want=nearby(lon,lat);
            captureWait+=dt;
            // A first visit mounts its ground before its streets: a picture of
            // that is a picture of nothing. The capture waits for OSM, and
            // after two minutes takes what there is and says so.
            // The same for the surveyed summits: a mountain photographed
            // before its summit list landed is the relief they correct.
            size_t provisional=0,summitsPending=0;
            // And the rings to the horizon, where they are drawn.
            const bool farPending=farRelief.node&&farRelief.node->visible()&&
                (!farRelief.job||[&]{std::lock_guard<std::mutex> g(farRelief.job->lock);return farRelief.job->busy||!farRelief.job->done.empty();}()
                 ||[&]{for(int k=0;k<kFarLevels;++k)if(!farRelief.node->level(k))return true;return false;}());
            bool settled=engine.resources().assetLoadsSettled()&&std::all_of(want.begin(),want.end(),[&](Tile t){
                auto i=loaded.find(t.key());if(i==loaded.end())return false;
                if(i->second.data.value("provisional",false))++provisional;
                if(i->second.data.contains("peaks")&&i->second.data.at("peaks").value("pending",false))++summitsPending;
                // Its meshes too: a tile cooked again uploads them over
                // several frames (uploadParts), and its props may be done first.
                const auto& resident=i->second;
                if(!resident.geography||resident.nextPart<std::any_cast<const PreparedTile&>(resident.served->prepared).parts.size())return false;
                if(!buildingsReady(resident))return false;
                for(const auto& building:resident.buildings) {
                    const int desired=building.lod->desiredLevel();
                    if(desired<0)return false;
                    const auto& state=building.levels[size_t(desired)];
                    if(!state.refused&&(building.lod->activeLevel()!=desired||building.lod->fadingLevel()>=0))return false;
                }
                return resident.nextProp==resident.props.size()&&
                    resident.nextLettering==resident.data.value("lettering",json::array()).size();
            });
            // A capture that waits says what for, every ten seconds: otherwise
            // the only report is the smoke's data timeout, which blames the
            // network for whatever it was (CLAUDE.md §3).
            captureReport+=dt;
            if(!settled&&!captureQueued&&captureReport>=10.) {
                captureReport=0;
                std::string why;
                for(Tile t:want) {
                    auto i=loaded.find(t.key());
                    if(i==loaded.end()){why+=" "+t.key()+" not mounted;";continue;}
                    const auto& r=i->second;
                    const size_t parts=r.served?std::any_cast<const PreparedTile&>(r.served->prepared).parts.size():0;
                    if(!r.geography)why+=" "+t.key()+" no geography;";
                    else if(r.nextPart<parts)why+=" "+t.key()+" parts "+std::to_string(r.nextPart)+"/"+std::to_string(parts)+";";
                    else if(r.nextProp<r.props.size())why+=" "+t.key()+" props "+std::to_string(r.nextProp)+"/"+std::to_string(r.props.size())+";";
                }
                saida::Log::info("[World capture wait] ",int(captureWait)," s, not settled:",why.empty()?" every tile is in":why);
            }
            // Summits refine a relief that is already there: a minute less.
            if(settled&&((provisional&&captureWait<120.)||((summitsPending||farPending)&&captureWait<45.)))settled=false;
            if(settled&&provisional&&!captureQueued)
                saida::Log::error("[World capture] ",provisional," of ",want.size(),
                                  " tiles still wait for OSM after two minutes; photographed as they are");
            if(settled&&summitsPending&&!captureQueued)
                saida::Log::error("[World capture] ",summitsPending," of ",want.size(),
                                  " tiles still wait for their surveyed summits after 45 s; photographed as they are");
            // R1WORLD_CAPTURE_SEA: a picture of the sea waits for its ships
            // (half a minute at most -- the log then says there were none).
            if(settled)captureSeaWait+=dt;
            const bool seaReady=(!std::getenv("R1WORLD_CAPTURE_SEA")||(!seaShips.empty()&&captureSeaWait>4.)
                ||captureSeaWait>30.)
                // The local weather is part of the picture: a few seconds for it.
                &&(effectiveWeather().known||captureSeaWait>6.);
            if(settled&&seaReady&&!captureQueued){
                if(const char* motorcycle=std::getenv("R1WORLD_CAPTURE_MOTORCYCLE")) {
                    auto fail=[&](const char* reason){saida::Log::error("[World E2E] FAIL motorcycle: ",reason);testFailed=true;engine.sceneTree().quit();};
                    if(!motorcycleCaptureStarted) {
                        const std::string kind=motorcycle;
                        if(kind=="traffic") {
                            Reach target;
                            for(auto& [key,t]:loaded)for(size_t i=0;i<t.cars.size();++i)
                                if(t.cars[i]&&trafficSlotLive(t,i)&&r1::motorcycle(vehicleSpec(*t.cars[i]).name)){target={t.cars[i],&t,i,2};break;}
                            if(!target.node){fail("no live traffic motorcycle at this spawn");return;}
                            auto* collider=r1::VehicleRuntime::body(*target.node);
                            if(!collider||!collider->findByPath("Rider")){fail("traffic motorcycle has no driver");return;}
                            if(!takeTrafficCar(target)){fail("traffic takeover refused");return;}
                            if(target.tile->cars[target.agent]){fail("traffic still owns the stolen motorcycle");return;}
                        } else {
                            if(kind!="moto_city"&&kind!="moto_cross"){fail("unknown requested motorcycle");return;}
                            const auto error=spawnParkedVehicle(kind);
                            if(!error.empty()){fail(error.c_str());return;}
                            if(parked.back().node->findByPath("Vehicle collider/Rider")){fail("spawned motorcycle has a driver");return;}
                            takeAbandonedCar({parked.back().node,nullptr,parked.size()-1,1});
                        }
                        lon=carLon;lat=carLat;alt=carAlt;
                        if(!enterCar()||!playerBikeRider||playerBikeRider!=playerBody||playerBody->parent()!=r1::VehicleRuntime::body(*car)){
                            fail("real player body is not attached to the solver");return;
                        }
                        if(car->findByPath("Vehicle collider/Rider")){fail("previous driver remains on the motorcycle");return;}
                        motorcycleCaptureStarted=true;
                        // Clear the unused parked car from this inspection view;
                        // it otherwise stands between the camera and the saddle.
                        for(const auto& p:parked)if(p.node!=car)p.node->setVisible(false);
                        saida::Log::info("[World E2E] PASS motorcycle empty spawn or live traffic takeover and single solver-attached player");
                        return;
                    }
                    motorcycleCaptureWait+=dt;
                    if(std::getenv("R1WORLD_CAPTURE_MOTORCYCLE_AIM")) {
                        selectWeapon(r1::combat::Weapon::Pistol);setAiming(true);poseArms();bikeHandlePose();
                        if(motorcycleCaptureWait>1.&& !motorcycleCaptureFired) {
                            const auto before=arsenal.loaded();fire();motorcycleCaptureFired=true;
                            if(arsenal.loaded()!=before-1||combatFilter().ignore!=r1::VehicleRuntime::body(*car)->bodyId()){
                                fail("mounted weapon shot or own-body filter failed");return;
                            }
                            saida::Log::info("[World E2E] PASS motorcycle pistol fires with shared ammo, effects and collider filtering");
                        }
                    }
                    if(motorcycleCaptureWait<2.)return;
                    // Inspect the final solved pose, not just the IK request:
                    // an unreachable target must not pass with a floating hand.
                    const auto* animator=animators.front();const auto& rig=*animator->rig();
                    const auto& matrices=animator->globalPose().globalMatrices;
                    for(int h=0;h<2;++h) {
                        if(h==1&&aiming)continue;
                        const int bone=rig.findBoneIndex(h?"Bip01 R Hand":"Bip01 L Hand");
                        const auto palm=r1::rocketboxBarGrip(rig,h==1);
                        const auto actual=glm::vec3(matrices.at(size_t(bone))*glm::vec4(palm.position,1.f));
                        const float error=glm::length(actual-motorcycleContact(*playerBody,vehicleSpec(*car).name,h,true))*float(humanScale);
                        saida::Log::info("[World E2E] motorcycle ",h?"right":"left"," palm grip error=",error," m");
                        if(error>.015f){fail("solved palm is more than 1.5 cm from its handlebar grip");return;}
                    }
                    saida::Log::info("[World E2E] PASS solved motorcycle hand contacts");
                }
                // Optional visual proof uses the shipped car, normal cage API
                // and renderer, after source/terrain streaming has completed.
                if(std::getenv("R1WORLD_CAPTURE_VEHICLE_DENT")) {
                    if(!captureVehicleDent) {
                        const auto& spec=vehicleSpec(*car);
                        auto* damage=r1::VehicleRuntime::body(*car)->getBehaviour<saida::DeformableMeshBehaviour>();
                        if(!damage->applyImpact({0,float(spec.height*.5),float(-spec.length*.5)},{0,0,1},damage->maximumImpactEnergy)) {
                            saida::Log::error("[World E2E] FAIL vehicle dent: source geometry is not ready");
                            testFailed=true;engine.sceneTree().quit();return;
                        }
                        captureVehicleDent=true;
                    }
                    captureVehicleWait+=dt;
                    constexpr double kDentSettleTime=2.;
                    if(captureVehicleWait<kDentSettleTime||!engine.resources().assetLoadsSettled())return;
                }
                // An inspection picture is of the world: no HUD, no minimap.
                if(inspectAt){style("hud","display","none");minimapUi->setEnabled(false);}
                size_t plants=0;for(auto& [key,t]:loaded)plants+=t.vegetation.size();
                saida::Log::info("[World nature] resident plants=",plants," shared static prototypes=",naturePrototypes.size());
                int measuredW=0,measuredH=0;engine.window().size(measuredW,measuredH);
                saida::Log::info("[World capture quality] preset=",toasterMode?"toaster":"standard",
                    " window=",measuredW,"x",measuredH," farZ=",camera->farZ);
                std::array<size_t,3> buildingLevels{};size_t detailedBuildings=0;
                for(const auto& [key,t]:loaded)for(const auto& b:t.buildings) {
                    const int level=b.lod->activeLevel();
                    if(level>=0)++buildingLevels[size_t(level)];
                    if(level==0&&!t.reducedDensity)++detailedBuildings;
                }
                saida::Log::info("[World building LOD] visible Near/Mid/Far=",buildingLevels[0],"/",buildingLevels[1],"/",buildingLevels[2],
                    " detailed Near=",detailedBuildings," Far triangles per building <=12");
                saida::Log::info("[World traffic] resident cars=",trafficLive()," of ",trafficWanted()," asked for");
                const auto& rendering=engine.sceneTree().world().settings();
                saida::Log::info("[World PBR] ibl=",rendering.iblEnabled," specular=",rendering.iblSpecularIntensity,
                                 " sky=",rendering.skyboxTexture," exposure=",rendering.skyboxExposure);
                saida::Log::info("[World rain] mm_h=",effectiveWeather().rain,
                    " intensity=",rendering.rainIntensity," wetness=",rendering.wetness,
                    " storm_clouds=",rendering.stormCloudCover," sheltered=",rainSheltered);
                car->traverse([&](saida::Node& n,const glm::mat4&){
                    if(n.material()&&n.name().rfind("paint-",0)==0) {
                        const auto& m=n.material()->desc();
                        saida::Log::info("[World PBR] ",n.name()," metallic=",m.metallic," roughness=",m.roughness,
                                         " type=",int(m.type)," rgb=",m.baseColor.r,",",m.baseColor.g,",",m.baseColor.b);
                    }
                });
                size_t spots=0,gear=0,swimmers=0,resting=0;
                for(const auto& [key,t]:loaded) {
                    spots+=t.beachSpots.size();
                    for(const auto& v:t.beachVisitors)if(v.spot>=0) {
                        gear+=v.furniture&&v.furniture->enabled();
                        const auto& spot=t.beachSpots[size_t(v.spot)];
                        const auto pose=r1::beachPose(spot.towel,spot.water,v.age,unsigned(v.spot));
                        swimmers+=pose.swimming;resting+=pose.recline>.9;
                    }
                }
                const auto beachWeather=effectiveWeather();
                saida::Log::info("[World beach] spots=",spots," visitors=",beachLive," towels=",gear,
                    " resting=",resting," bathing=",swimmers," temperature=",beachWeather.temperature,
                    " known=",beachWeather.temperatureKnown," factor=",
                    r1::beachAttendance(localSolarHour(),beachSun,beachWeather));
                captureQueued=true;engine.captureFrameThenExit(worldCapture);
            }
        }
        updateRain(delta);
        if(hud>.5) {
            hud=0;
            updateMinimap();
            text("local-conditions",weatherLabel());
            text("coordinates",number(lat)+"°  /  "+number(lon)+"°     "+number(alt,1)+" m"
                 +(driving?"     "+std::to_string(int(std::round(std::abs(carSpeed)*3.6)))+" km/h":"")
                 +(sailing?"     "+std::to_string(int(std::round(std::abs(boat.speed)*1.943844)))+" knots":"")
                 +(piloting?"     "+std::to_string(int(std::round(std::abs(plane.speed)*3.6)))+" km/h · "
                    +std::to_string(int(std::round(plane.alt-planeGround)))+" m AGL"
                    +(plane.helicopter()?"":" · throttle "+std::to_string(int(std::round(plane.lever*100)))+" %"):""));
            Reach within;BoatReach moored;PlaneReach standing;double howFar=0,boatFar=0,planeFar=0;
            std::string mode=driving
                ?" · Driving · F: get out"
                :piloting?std::string(plane.helicopter()
                    ?" · Helicopter · Space/Shift: climb/descend · W/S: forward/back · A/D: turn · F: get out"
                    :" · Plane · W/S: throttle · A/D: bank · Space/Shift: pitch up/down · F: get out")
                  +(tile(lon,lat)?"":" · terrain below the aircraft unknown")
                :sailing?" · At the helm · F: disembark or step aboard"
                :swimming?" · In the water · WASD/ZQSD: swim · F: climb back aboard"
                :nearestAircraft(standing,planeFar)?" · F: take the aircraft"
                :nearestBoat(moored,boatFar)?" · F: take the boat"
                :(nearestCar(within,howFar)?" · F: get in the vehicle":performanceDebug?" · Esc: map":" · M: map");
            const auto* currentTile=tile(lon,lat);
            // A cook that ran out of memory is said where the player looks,
            // and fails the smoke on that reason (World service, OUT-OF-MEMORY).
            const auto serviceStatus=service->status();
            if(serviceStatus.outOfMemory>outOfMemorySaid) {
                outOfMemorySaid=serviceStatus.outOfMemory;
                saida::Log::warn("[World memory] the world service ran out of memory (",outOfMemorySaid," times); ",
                                 serviceStatus.allowed," tile(s) cooked at once now, private ",int(privateMegabytes())," MB");
                if(smoke){saida::Log::error("[World E2E] FAIL out of memory cooking ",serviceStatus.error);testFailed=true;engine.sceneTree().quit();return;}
            }
            if(outOfMemorySaid)mode+=" · Memory full: tiles load "+std::string(serviceStatus.allowed>1?"more slowly":"one at a time");
            if(serviceStatus.memoryPaused)mode+=" · Loading waits for free memory";
            if(currentTile&&currentTile->data.value("provisional",false))
                mode+=" · Streets and buildings on their way (OpenStreetMap)";
            if(currentTile&&currentTile->data.value("groundPending",false))
                mode+=" · Provisional terrain: elevation loading";
            std::string retailPlace;
            for(const auto& [key,t]:loaded) {
                auto p=t.frame.local(ecef(lon,lat,alt));const r1::P2 q{p.x,p.z};
                for(const auto& room:t.interiors)if(r1::pointInPolygon(q,room.plan.ring))retailPlace=room.plan.place(q);
                if(!retailPlace.empty())break;
                for(const auto& lot:t.parking){bool in=false;for(const auto& ring:lot.rings)in^=r1::pointInPolygon(q,ring);
                    if(in){retailPlace="Car park "+lot.name;break;}}
            }
            if(!retailPlace.empty())mode=" · "+retailPlace+mode;
            combatHud();
            text("stream-status",std::to_string(loaded.size())+" active tiles"+mode
                 +(fast?" · detail reduced at this speed":"")
                 +(physicsRefused?" · collisions incomplete: the physics world is full":""));
        }
        if(performanceSmoke){runPerformanceSmoke(delta);return;}
        if(smokeFighting){runSmokeFight(dt);return;}
        if(trafficValidation_){validateTraffic(dt);return;}
        if(smokeWaterSpawn&&smokeWalk>1.5) {
            const double covered=glm::length(ecef(lon,lat,alt)-smokeStart);
            const double rootBelow=origin.local(ecef(lon,lat,alt)).y-player->transform().position.y;
            testFailed=!swimming||!onWater(lon,lat)||carParked||covered<1.||rootBelow<playerHeight*.39||rootBelow>playerHeight*.78;
            saida::Log::info("[World E2E] ",testFailed?"FAIL":"PASS"," water spawn: swam ",covered,
                             "m, body depth=",rootBelow,"m, car parked=",carParked);
            engine.sceneTree().quit();return;
        }
        if(retailTest()){runRetailTest(delta);return;}
        if(smokeFlyWait||smokeFlyPhase){runSmokeFly(delta);return;}
        if(smokeSailWait||smokeSailing){runSmokeSail(delta);return;}
        if(smokeSwimming){runSmokeSwim(delta);return;}
        if(smokeSeaWait){runSmokeSea(delta);return;}
        if(idleSmoke&&smokeStarted&&idleSmokeStopped) {
            const auto displacement=origin.local(ecef(lon,lat,alt))-origin.local(idleSmokeStart);
            idleSmokeDrift=std::max(idleSmokeDrift,std::hypot(displacement.x,displacement.z));
            if(smokeWalk>14.) {
                const bool passed=smokeRan&&idleSmokeDrift<.005&&feet->isOnFloor()
                    &&jumpOffset==0&&animators.front()->currentClip()=="idle";
                saida::Log::info("[World E2E] ",passed?"PASS":"FAIL",
                    " stop after walking: idle_horizontal_drift_m=",idleSmokeDrift,
                    " grounded=",feet->isOnFloor()," clip=",animators.front()->currentClip());
                testFailed=!passed;smokeStarted=false;engine.sceneTree().quit();return;
            }
        }
        if(wadeSmoke&&smokeStarted) {
            auto fail=[&](const std::string& why){
                saida::Log::error("[World E2E] FAIL wade: ",why);testFailed=true;engine.sceneTree().quit();
            };
            if(!wadeTo) {
                std::istringstream in(wadeSmoke);double x=0,y=0;
                if(!(in>>x>>y)||!std::isfinite(x)||!std::isfinite(y))return fail(std::string("R1WORLD_WADE_SMOKE '")+wadeSmoke+"' is not '<lon> <lat>'");
                wadeTo=glm::dvec2(x,y);
            }
            if(swimming)return fail("swimming at "+number(lon,6)+", "+number(lat,6)+" after "+number(wadeOverWater,1)+
                                    " m over water, feet at "+number(alt,2)+" m");
            if(onWater(lon,lat)) {
                wadeOverWater+=std::hypot(footEast,footNorth)*std::min(.05,double(delta));
                wadeDeepest=std::max(wadeDeepest,waterLevel(lon,lat)-alt);
            }
            const double left=metresFrom(wadeTo->x,wadeTo->y);
            if(left<2.||smokeWalk>40.) {
                if(left>=2.)return fail("stopped "+number(left,1)+" m short of the target, at "+number(lon,6)+", "+number(lat,6));
                if(wadeOverWater<1.)return fail("the walk never crossed a water cell");
                saida::Log::info("[World E2E] PASS wade: ",wadeOverWater," m walked over water cells, deepest ",
                                 wadeDeepest," m of water at the feet (swims from ",playerHeight*kSwimDepth," m)");
                smokeStarted=false;engine.sceneTree().quit();
            }
            return;
        }
        if(!idleSmoke&&smokeStarted&&smokeWalk>4&&(!qualitySwitchSmoke||qualitySwitches>=24)) {
            if(!smokeRan||!smokeJumped||jumpOffset!=0||followDistance<.15||followDistance>kOnFootFollow+.001){
                saida::Log::error("[World E2E] FAIL player animation/jump/follow");testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] player run/jump/landing/follow passed, distance=",followDistance);
            sayBodies();
            if(!trunksSolid()||!groundMatchesDrawing()||!seamsClosed()){testFailed=true;engine.sceneTree().quit();return;}
            // A neighbourhood that asked for people and shows none is the
            // failure; a moor at night that asked for none is not one.
            if(bodiesRefused>0||feetRefused) {
                saida::Log::error("[World E2E] FAIL crowd: ",bodiesRefused," people cannot look or stagger",
                                  feetRefused?", and the player's feet are no physics body":"");
                testFailed=true;engine.sceneTree().quit();return;
            }
            if(crowdWanted()>0&&crowdLive()==0) {
                saida::Log::error("[World E2E] FAIL crowd: ",crowdWanted()," people asked for, none placed");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] crowd: ",crowdLive()," people of ",crowdWanted(),
                             " asked for at ",localSolarHour(),"h solar (factor ",crowdFactor(),"), bumps ",playerBumps);
            if(ragdollRefused>0) {
                saida::Log::error("[World E2E] FAIL combat: ",ragdollRefused," knocked-down bodies refused by the physics world");
                testFailed=true;engine.sceneTree().quit();return;
            }
            smokeStarted=false;
            if(perfSmoke=="walk") {saida::Log::info("[World perf] PASS warm walking");engine.sceneTree().quit();return;}
            if(perfSmoke.empty()&&!smokeFought&&crowdLive()>0){startSmokeFight();return;}
            if(!smokeDroveOnce&&!onSeaIce(lon,lat)){startSmokeDrive();return;}
            if(!smokeDroveOnce)saida::Log::info("[World E2E] on the sea ice: no car to drive, the walk is the test");
            smokeFinishPhase();
        }
        if(smokeApproach) {
            if(carDistance()<=kCarReach*.8){smokeApproach=false;beginSmokeDriving();return;}
            if(smokeApproachTime>20.) {
                // What stands in the way, said: the next step toward the car.
                auto to=origin.local(ecef(carLon,carLat,carAlt))-origin.local(ecef(lon,lat,alt));
                const double l=std::max(1e-6,std::hypot(to.x,to.z));
                const auto q=onward(lon,lat,to.x/l*.5,-to.z/l*.5);
                saida::Log::error("[World E2E] FAIL could not walk back to the car, still ",
                                  carDistance(),"m away after ",smokeApproachTime,"s; player at ",lon,", ",lat,
                                  " alt ",alt,", car alt ",carAlt,"; next step: water=",onWater(q.x,q.y,alt),
                                  " obstacle=",blocked(q.x,q.y,alt)," blocked=",blocked(q.x,q.y,alt),
                                  " level there ",height(q.x,q.y,alt));
                testFailed=true;engine.sceneTree().quit();
            }
            return;
        }
        if(smokeDriving&&smokeBrake) {
            smokeDriveTime+=std::min(.05,double(delta));
            // The traffic takeover also exercises service braking before exit.
            if(std::abs(carSpeed)>kSmokeBrakeSpeed) {
                if(smokeDriveTime>20.) {
                    saida::Log::error("[World E2E] FAIL the car would not slow down, still ",
                                      std::abs(carSpeed)," m/s");
                    testFailed=true;engine.sceneTree().quit();
                }
                return;
            }
            smokeDriving=false;
            saida::Log::info("[World E2E] braked to ",std::abs(carSpeed)," m/s");
            if(!leaveCar()||driving||!player->enabled()) {
                saida::Log::error("[World E2E] FAIL could not step out of the car at ",
                                  std::abs(carSpeed)," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] on foot again at ",lon,", ",lat);
            if(!smokeTookOver&&trafficLive()>0){runSmokeTakeover();return;}
            smokeFinishPhase();
            return;
        }
        if(smokeDriving) {
            smokeDriveTime+=std::min(.05,double(delta));
            smokeTopSpeed=std::max(smokeTopSpeed,carSpeed);
            size_t live=0;
            for(auto& [key,t]:loaded)
                for(const auto& a:t.flow.agents()) {
                    if(!a.alive)continue;
                    ++live;
                    smokeTrafficMoved=smokeTrafficMoved||a.speed>2.f;
                }
            smokeTrafficSeen=std::max(smokeTrafficSeen,live);
            // Check the exit while the car is still moving. In a dense city a
            // clear 60 m stretch can end before five seconds at full throttle.
            const double covered=glm::length(ecef(lon,lat,alt)-smokeDriveStart);
            const double needed=std::min(20.,smokeClear*.6);
            if(smokeDriveTime<2. || (smokeDriveTime<5. &&
               (covered<needed||std::abs(carSpeed)<=kSmokeBrakeSpeed)))return;
            // A street that asked for cars and got none is the failure this
            // assertion exists for -- and a spawn on a moor that asked for
            // none is not one, so the claim is checked against the ask.
            if(trafficWanted()>0&&(smokeTrafficSeen==0||!smokeTrafficMoved)) {
                saida::Log::error("[World E2E] FAIL traffic: ",trafficWanted(),
                                  " cars asked for, ",smokeTrafficSeen," seen, moving=",
                                  smokeTrafficMoved);
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] traffic: ",smokeTrafficSeen," cars driving of ",
                             trafficWanted()," the neighbourhood asked for");
            // What is actually being asserted: that the car goes somewhere, and
            // that it goes there faster than a man can run (7 m/s sprinting).
            // The distance is measured against the clearance the driver found
            // rather than against a fixed number, because a spawn is wherever
            // the map was clicked and some of them are courtyards.
            if(covered<needed||smokeTopSpeed<7.2||std::abs(carSpeed)<=kSmokeBrakeSpeed) {
                saida::Log::error("[World E2E] FAIL drive: covered=",covered,
                                  "m of ",smokeClear,"m clear, top=",smokeTopSpeed," m/s","; last stopped by ",carStop," at ",carLon,", ",carLat," alt ",carAlt);
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] drive passed, covered=",covered,
                             "m top=",smokeTopSpeed," m/s (",int(smokeTopSpeed*3.6)," km/h), power loss=",
                             r1::VehicleRuntime::driver(*car)->damage);
            reportChurn("driving");measureWalk();
            auto* body=r1::VehicleRuntime::body(*car);
            const auto velocity=body->physicsWorld()->linearVelocity(body->bodyId());
            const double exitSpeed=carSpeed;
            if(!leaveCar()||driving||!player->enabled()||
               glm::distance(velocity,body->physicsWorld()->linearVelocity(body->bodyId()))>.001f) {
                saida::Log::error("[World E2E] FAIL moving exit or vehicle momentum at ",exitSpeed," m/s");
                testFailed=true;engine.sceneTree().quit();return;
            }
            saida::Log::info("[World E2E] PASS moving exit at ",exitSpeed," m/s with vehicle momentum preserved");
            smokeDriving=false;
            if(perfSmoke=="drive"){saida::Log::info("[World perf] PASS warm driving");engine.sceneTree().quit();return;}
            if(!smokeTookOver&&trafficLive()>0){runSmokeTakeover();return;}
            smokeFinishPhase();
            return;
        }
    }
};

int main(int argc,char** argv) {
    try {
        if(argc>1&&std::string(argv[1])=="--geo-contract") {
            json result=json::array();
            for(double la:{-90.,-89.999,-33.,0.,48.8566,89.999,90.})for(double lo:{-180.,-179.999,2.3522,179.999,180.}) {
                Frame a(lo,la,123.);auto p=ecef(wrap(lo+.001),std::clamp(la+.001,-90.,90.),456.);
                auto q=a.origin+a.basis*a.local(p);auto step=advance(lo,la,500,500);
                result.push_back({{"lon",lo},{"lat",la},{"tile",tileAt(lo,la).key()},
                                  {"roundTripError",glm::length(p-q)},{"step",{step.x,step.y}}});
            }
            std::cout<<result.dump()<<std::endl;return 0;
        }
        fs::path game=fs::absolute(fs::path(argv[0])).parent_path().parent_path().parent_path();
        bool smoke=false,sail=false,fly=false;double startLon=2.3522,startLat=48.8566;bool hop=false;double hopLon=0,hopLat=0;
        for(int i=1;i<argc;++i){std::string a=argv[i];if(a=="--project"&&i+1<argc)game=fs::absolute(argv[++i]);else if(a=="--smoke")smoke=true;else if(a=="--sail")sail=true;else if(a=="--fly")fly=true;else if(a=="--spawn"&&i+2<argc){startLon=std::stod(argv[++i]);startLat=std::stod(argv[++i]);}else if(a=="--spawn2"&&i+2<argc){hop=true;hopLon=std::stod(argv[++i]);hopLat=std::stod(argv[++i]);}}
        if(!std::isfinite(startLon)||!std::isfinite(startLat)||std::abs(startLat)>90||std::abs(startLon)>180)throw std::runtime_error("Invalid --spawn coordinate");
        // This development executable uses the existing engine's baked paths
        // for shaders/fonts, and the project's root for content.
        saida::PhysicsCapacity physics;physics.bodies=kPhysicsBodies;
        auto menuGlobe=r1::registerMenuGlobe();
        saida::Engine engine(nullptr,(game/"R1World.saidaproj").string(),false,{},physics);
        engine.resources().assetLoader().setBudget(kDecodedAssetBudget);
        if(!saida::SceneSerializer::loadIntoScene(engine.scene(),engine.resources(),(game/"scenes/earth.scene").string()))throw std::runtime_error("Cannot load Earth scene");
        engine.mountWorld();saida::Time::setScale(1);
        saida::CaptureRequest capture;saida::runtime::CaptureViewpoint view;std::string error;
        if(!saida::runtime::parseCaptureArgs(argc,argv,capture,view,error))throw std::runtime_error(error);
        engine.setCameraFovOverride(view.fovDegrees);
        std::string profile;
        if(!saida::runtime::parseProfileArgs(argc,argv,profile,error))throw std::runtime_error(error);
        engine.profileTo(profile);
        // The Atlas, the ground classes and the surfaces: data the generator
        // reads before any tile is cooked, and refuses to run without.
        r1::loadPalette(game.string());
        World world(engine,game,smoke,capture,startLon,startLat,hop,hopLon,hopLat,view,sail,fly);
        world.setMenuGlobe(menuGlobe);
        for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--at") {
            const double at=std::stod(argv[i+1]);
            if(!std::isfinite(at))throw std::runtime_error("Invalid --at instant");
            world.inspectInstant(at);
        }
        for(int i=1;i+1<argc;++i)if(std::string(argv[i])=="--camera-altitude") {
            const double metres=std::stod(argv[i+1]);
            if(!std::isfinite(metres))throw std::runtime_error("Invalid --camera-altitude");
            world.captureFromAltitude(metres);
        }
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--camera-geo") {
            if(i+3>=argc)throw std::runtime_error("--camera-geo requires longitude latitude eye-height-above-ground");
            std::array<double,3> geo{std::stod(argv[i+1]),std::stod(argv[i+2]),std::stod(argv[i+3])};
            if(!std::isfinite(geo[0])||!std::isfinite(geo[1])||!std::isfinite(geo[2])||
               std::abs(geo[0])>180||std::abs(geo[1])>90||geo[2]<=0||geo[2]>10000)
                throw std::runtime_error("Invalid --camera-geo coordinate or eye height");
            if(!smoke||capture.pngPath.empty()||!view.set)throw std::runtime_error("--camera-geo requires --smoke, --screenshot and a capture viewpoint");
            world.captureAtGeography(geo);
            saida::Log::info("[World inspection] fixed camera longitude=",std::setprecision(12),geo[0]," latitude=",geo[1]," eye_agl_m=",geo[2]);
        }
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--weather") {
            if(i+3>=argc)throw std::runtime_error("--weather requires cloud fraction, rain mm/h and visibility metres");
            std::array<double,3> conditions{};
            for(size_t k=0;k<conditions.size();++k) {
                const std::string value=argv[i+1+int(k)];size_t end=0;
                conditions[k]=std::stod(value,&end);
                if(end!=value.size()||!std::isfinite(conditions[k]))throw std::runtime_error("Invalid --weather value");
            }
            if(conditions[0]<0.||conditions[0]>1.||conditions[1]<0.||conditions[2]<0.)
                throw std::runtime_error("--weather requires cloud in [0,1], nonnegative rain and visibility");
            world.inspectConditions(conditions);
        }
        for(int i=1;i<argc;++i)if(std::string(argv[i])=="--temperature") {
            if(i+1>=argc)throw std::runtime_error("--temperature requires degrees Celsius");
            std::string value=argv[i+1];size_t end=0;double temperature=std::stod(value,&end);
            if(end!=value.size())throw std::runtime_error("Invalid --temperature value");
            world.inspectHeat(temperature);
        }
        const saida::Engine::FrameFn followPhysics=[&](float dt){world.afterSimulation(dt);};
        engine.setOnFrame([&](float dt){
            world.update(dt);
            // Only driving changes the pose after physics. An empty hook also
            // skips the engine's full transform propagation for that stage.
            engine.setAfterSimulation(world.needsAfterSimulation()?followPhysics:saida::Engine::FrameFn{});
        });
        if(!smoke&&!capture.pngPath.empty())engine.captureFrameThenExit(capture);
        engine.run();return engine.captureFailed()||world.failed()?1:0;
    }catch(const std::exception& e){saida::Log::error("R1World: ",e.what());return 1;}
}
