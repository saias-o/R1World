// R1World's land animals at run time. Included inside World, like the beach
// and the police: the residents are cooked with each tile (gen/fauna.hpp),
// the engine's wildlife add-on (engine/plugins/wildlife) lives their lives,
// and this draws them: one pooled node per slot with its own animator over a
// model's shared meshes, rig and clips (CLAUDE.md §5: a hundred cows upload
// one cow), its two levels of detail, and the clip its state calls for at the
// speed it moves, so hooves stay planted.
//
// assets/animals/animals.json (tools/r1/animals.py) says how each model was
// measured: where its hooves touch the ground, which way its head points,
// which clip plays what and how fast each gait carries the body.

// How many animals live at once around the player, nearest tiles first, and
// how many slots one tile pools: each is a node, an animator and a kinematic
// hit/collision box, including small residents. Birds share it: central
// Paris holds some 40 pigeons within sight, and the beasts must still come.
static constexpr size_t kFaunaAnimals=64;
struct AnimalKind {
    std::string model,lod0,lod1;
    double ground=0,height=0,length=0,width=0,forward=0;
    std::map<std::string,std::vector<std::string>> roles;
    double walkSpeed=0,runSpeed=0;
    std::vector<std::pair<std::string,std::vector<std::unique_ptr<saida::MeshNode>>>> levels;
    saida::Rig* rig=nullptr;
    std::vector<std::pair<std::string,const saida::AnimationClip*>> clips;
    bool ready=false,failed=false,bird=false;
    std::string refusal;
};
std::map<std::string,AnimalKind> animalKinds;
std::set<std::string> animalRefusalsSaid;
bool faunaSmokeSaid=false;

void loadAnimals() {
    std::ifstream input(game/"assets/animals/animals.json");
    if(!input)throw std::runtime_error("Missing assets/animals/animals.json (python -m r1.animals)");
    json doc;input>>doc;
    for(const auto& [id,m]:doc.at("models").items()) {
        AnimalKind kind;
        kind.model=id;kind.lod0=m.at("lod0");kind.lod1=m.at("lod1");
        kind.ground=m.value("ground",0.);kind.height=m.value("height",0.);
        kind.length=m.value("length",0.);kind.width=m.value("width",0.);kind.forward=m.value("forward",0.);
        const json roles=m.value("roles",json::object());
        for(const auto& [role,clips]:roles.items())kind.roles[role]=clips.get<std::vector<std::string>>();
        const auto gaits=m.value("gaits",json::object());
        if(gaits.contains("walk"))kind.walkSpeed=gaits.at("walk").value("speed",0.);
        if(gaits.contains("run"))kind.runSpeed=gaits.at("run").value("speed",0.);
        if(!m.value("sameRig",false)){kind.failed=true;kind.refusal="its LOD1 has another skeleton";}
        for(const auto& s:r1::fauna().species)if(s.bird.enabled)
            for(const auto& variant:s.models)if(variant.model==id)kind.bird=true;
        animalKinds.emplace(id,std::move(kind));
    }
    size_t species=r1::fauna().species.size();
    saida::Log::info("[World fauna] ",species," species, ",animalKinds.size()," measured models");
    loadAnimalRemains();
}
// A refusal is said once, in the log and to the player, and fails a smoke
// (CLAUDE.md §3); the animal is not drawn rather than drawn wrong.
void refuseAnimal(AnimalKind& kind,const std::string& why) {
    kind.failed=true;kind.refusal=why;
    if(!animalRefusalsSaid.insert(kind.model).second)return;
    saida::Log::error("[World fauna] ",kind.model," is not drawn: ",why);
    text("stream-status","Some animals are unavailable: "+kind.model+" ("+why+").");
    if(smoke){saida::Log::error("[World E2E] FAIL fauna: ",kind.model,": ",why);testFailed=true;}
}
// The meshes of one prototype, flattened with the transforms above them --
// the root joints' parent frame included (GLTFLoader::skinnedPrimitiveFrame).
static void collectSkinned(const saida::Node& source,std::vector<std::unique_ptr<saida::MeshNode>>& out) {
    std::function<void(const saida::Node&,const saida::Transform&)> walk=[&](const saida::Node& n,const saida::Transform& above) {
        saida::Transform here;
        here.rotation=above.rotation*n.transform().rotation;
        here.scale=above.scale*n.transform().scale;
        here.position=above.position+above.rotation*(above.scale*n.transform().position);
        if(n.mesh()) {
            auto mesh=std::make_unique<saida::MeshNode>(n.name(),n.mesh(),n.material());
            mesh->transform()=here;out.push_back(std::move(mesh));
        }
        for(const auto& c:n.children())walk(*c,here);
    };
    walk(source,saida::Transform{});
}
// A model's shared parts, from its two prototypes on first use; null while
// they stream in or once refused.
AnimalKind* animalKind(const std::string& model) {
    auto found=animalKinds.find(model);
    if(found==animalKinds.end())return nullptr;
    AnimalKind& kind=found->second;
    if(kind.failed)return nullptr;
    if(kind.ready)return &kind;
    saida::Node* nearModel=prototype(kind.lod0);
    saida::Node* farModel=prototype(kind.lod1);
    if(!nearModel||!farModel)return nullptr;
    std::vector<saida::Animator*> animators;
    nearModel->findBehavioursInChildren(animators);
    if(animators.empty()||!animators.front()->rig()){refuseAnimal(kind,"no skeleton in "+kind.lod0);return nullptr;}
    kind.rig=const_cast<saida::Rig*>(animators.front()->rig());
    for(const auto& [name,clip]:animators.front()->clips())kind.clips.push_back({name,clip});
    for(const auto& [role,names]:kind.roles)
        for(const auto& name:names)
            if(std::none_of(kind.clips.begin(),kind.clips.end(),[&](const auto& c){return c.first==name;})) {
                refuseAnimal(kind,"no clip "+name+" for its "+role);return nullptr;
            }
    std::vector<std::unique_ptr<saida::MeshNode>> nearMeshes,farMeshes;
    collectSkinned(*nearModel,nearMeshes);collectSkinned(*farModel,farMeshes);
    if(nearMeshes.empty()||farMeshes.empty()){refuseAnimal(kind,"no mesh in one of its levels");return nullptr;}
    kind.levels.emplace_back("Near",std::move(nearMeshes));
    kind.levels.emplace_back("Far",std::move(farMeshes));
    kind.ready=true;
    return &kind;
}
// One animal as drawn: a node with its animator over a kind's shared meshes,
// both levels in the rig's imported parent frame, and a kinematic hit box.
bool makeBeast(saida::Node& parent,const std::string& model,double scale,Beast& beast) {
    AnimalKind* kind=animalKind(model);
    if(!kind)return false;
    if(beast.node){beast.node->queueFree();beast.node=nullptr;}
    auto root=std::make_unique<saida::Node>("animal-"+model);
    auto visual=std::make_unique<saida::Node>("Skeleton frame");
    visual->transform()=r1::animalTransform(kind->rig->rootParent());
    auto* animator=visual->addBehaviour<saida::Animator>();
    animator->setRig(kind->rig);
    beast.birdPose=kind->bird?animator->addModifier<r1::BirdPose>(*kind->rig,float(kind->forward)):nullptr;
    for(const auto& [name,clip]:kind->clips)animator->addClip(name,clip);
    for(const auto& [level,meshes]:kind->levels) {
        auto holder=std::make_unique<saida::Node>(level);
        for(const auto& m:meshes) {
            auto mesh=std::make_unique<saida::MeshNode>(m->name(),m->mesh(),m->material());
            mesh->transform()=r1::animalTransform(glm::inverse(kind->rig->rootParent())*m->localMatrix());
            holder->addChild(std::move(mesh));
        }
        visual->addChild(std::move(holder));
    }
    // The near model while the animal stands taller than 5% of the screen.
    visual->addBehaviour<saida::LODGroupBehaviour>()->setLevels({{"Near",.05f},{"Far",0.f}});
    beast.visual=root->addChild(std::move(visual));
    {
        // A box over its body, along its head axis, at its actual size.
        auto body=std::make_unique<saida::RigidBodyNode>();
        body->kinematic=true;
        const bool sideways=std::abs(std::sin(kind->forward))>.7;
        auto box=std::make_unique<saida::CollisionShapeNode>();
        box->shapeType=saida::CollisionShapeType::Box;
        const float along=float(std::max(.006,kind->length*(kind->bird?.65:.9))),across=float(std::max(.004,kind->width*(kind->bird?.22:.9)));
        box->halfExtents=.5f*glm::vec3(sideways?along:across,float(std::max(.004,kind->height*.9)),sideways?across:along);
        box->offset=glm::vec3(0.f,float(kind->ground+kind->height*.45),0.f);
        body->addChild(std::move(box));
        beast.body=static_cast<saida::RigidBodyNode*>(root->addChild(std::move(body)));
    }
    root->transform().scale=glm::vec3(float(scale));
    beast.node=parent.addChild(std::move(root));
    beast.animator=animator;beast.model=model;beast.clip.clear();beast.playback=-1;beast.poseRate=-1;
    beast.shown=false;
    return true;
}
#include "fauna_combat.hpp"
#include "bird_runtime.hpp"
// The clip a state calls for, and its playback rate at `speed` m/s: a gait
// plays as fast as the body moves over its measured stride speed.
std::pair<std::string,float> beastClip(const AnimalKind& kind,saida::wildlife::State state,double speed,double scale,uint64_t identity,bool aloft) {
    auto pick=[&](const char* role)->const std::vector<std::string>*{
        auto it=kind.roles.find(role);return it==kind.roles.end()||it->second.empty()?nullptr:&it->second;
    };
    auto any=[&](std::initializer_list<const char*> roles)->const std::vector<std::string>*{
        for(const char* r:roles)if(auto* found=pick(r))return found;
        return nullptr;
    };
    using S=saida::wildlife::State;
    if(aloft)if(auto* fly=pick("fly"))return {fly->front(),1.f};
    const std::vector<std::string>* list=nullptr;double stride=0;
    switch(state) {
    case S::Walk: list=pick("walk");stride=kind.walkSpeed;if(!list){list=pick("run");stride=kind.runSpeed;}break;
    case S::Run: case S::Flee: list=pick("run");stride=kind.runSpeed;if(!list){list=pick("walk");stride=kind.walkSpeed;}break;
    case S::Eat: list=any({"eat","idle"});break;
    default: list=any({"idle","eat","fly","walk"});break;
    }
    if(!list)list=any({"idle","eat","walk","run","fly"});
    if(!list)return {"",1.f};
    const std::string& clip=(*list)[identity%list->size()];
    if(stride>1e-4&&speed>0)return {clip,float(std::clamp(speed/(stride*scale),.3,2.4))};
    return {clip,1.f};
}

void readFauna(Loaded& tile) {
    const json& f=tile.data.value("fauna",json());
    tile.faunaGrid=r1::HabitatGrid::from(f.value("grid",json()));
    tile.residents=r1::readResidents(f);
    tile.faunaKinds.clear();
    std::vector<saida::wildlife::Kind> kinds;
    std::vector<saida::wildlife::Group> groups;
    const std::string country=f.value("country",std::string()),climate=f.value("climate",std::string());
    const double lon=tile.data.at("lon").get<double>(),lat=tile.data.at("lat").get<double>();
    std::map<std::string,std::string> skipped;
    for(const auto& r:tile.residents) {
        const r1::Species* species=r1::speciesById(r.species);
        if(!species){skipped[r.species]="no longer in fauna.json";continue;}
        saida::wildlife::Group group;
        group.home={float(r.home.x),float(r.home.y)};group.identity=r.identity;
        for(const auto& model:r.models) {
            auto measured=animalKinds.find(model);
            if(measured==animalKinds.end()){skipped[model]="not measured";continue;}
            size_t index=0;
            while(index<tile.faunaKinds.size()&&!(tile.faunaKinds[index].species==species&&tile.faunaKinds[index].model==model))++index;
            if(index==tile.faunaKinds.size()) {
                const AnimalKind& m=measured->second;
                const double scale=species->scale;
                saida::wildlife::Kind k;
                k.walkSpeed=float((m.roles.count("walk")?m.walkSpeed:m.roles.count("run")?m.runSpeed*.6:0.)*scale);
                if(m.roles.count("fly")&&!species->bird.enabled)k.walkSpeed=float(std::max(.6,species->spread*.3));
                k.runSpeed=float(m.roles.count("run")?m.runSpeed*scale:0.);
                k.fleeSpeed=float(species->dart?std::max(1.5,m.length*scale*8):
                                  k.runSpeed>0?k.runSpeed*1.8f:k.walkSpeed*2.f);
                k.fleeDistance=float(species->flee);
                k.flee=species->flee<=0?saida::wildlife::FleeStyle::None:
                       species->dart?saida::wildlife::FleeStyle::Dart:saida::wildlife::FleeStyle::Run;
                k.spread=float(species->spread);k.roam=float(species->home);
                k.legMin=float(std::min(6.,species->home*.2));k.legMax=float(std::max(double(k.legMin)+1.,std::min(30.,species->home*.6)));
                // Without a standing clip it keeps moving: an Asian elephant
                // browses as it walks, a wasp never lands.
                k.canStand=m.roles.count("idle")||m.roles.count("eat");
                k.canEat=m.roles.count("eat")>0;
                if(species->surface=="air"){k.surface=saida::wildlife::Surface::Air;k.hoverLow=float(species->hoverLow);k.hoverHigh=float(species->hoverHigh);}
                // Stream animals in well before their flight distance: a deer
                // must already be there when a walker approaches it.
                // A bird that flies is seen by the most it shows, wingspan
                // or length, against the sky: a hawk modelled wings spread
                // is 11 cm tall and 1.1 m across, and is met at the latter.
                constexpr double viewScale=250.,flightMargin=3.,minView=24.,maxView=450.;
                const bool flier=species->bird.enabled&&species->bird.flies;
                const double seen=flier?std::max({m.height,m.length,m.width}):m.height;
                k.viewRange=float(std::clamp(std::max(viewScale*std::sqrt(std::max(.0001,seen*scale)),
                                                     flightMargin*species->flee),minView,maxView));
                k.radius=float(std::max(.02,m.width*scale*.5));
                if(species->bird.enabled&&species->bird.flies) {
                    const auto& b=species->bird;auto& f=k.flight;f.enabled=true;
                    f.speed=float(b.speed);f.acceleration=float(b.acceleration);f.climb=float(b.climb);f.turn=float(b.turn);
                    f.low=float(b.low);f.high=float(b.high);f.flyMin=float(b.flyMin);f.flyMax=float(b.flyMax);
                    f.restMin=float(b.restMin);f.restMax=float(b.restMax);f.glide=float(b.glide);f.flapHz=float(b.flapHz);
                    f.groundShare=float(b.groundShare);f.perchLow=float(b.perchLow);f.perchHigh=float(b.perchHigh);
                    k.flee=saida::wildlife::FleeStyle::Run;k.canStand=true;
                    // The new small-bird rigs have no measured stride; this
                    // short foraging gait is inferred and animated explicitly.
                    if(m.roles.count("idle")&&!m.roles.count("walk"))k.walkSpeed=float(std::clamp(m.length*scale*2.,.12,.7));
                }
                kinds.push_back(k);
                tile.faunaKinds.push_back({species,model,r1::rangeAt(*species,country,lon,lat,climate)});
            }
            group.members.push_back(uint16_t(index));
        }
        if(!group.members.empty())groups.push_back(std::move(group));
    }
    for(const auto& [what,why]:skipped)saida::Log::warn("[World fauna] ",tile.data.value("key",std::string()),": ",what," skipped, ",why);
    tile.groupSpecies.clear();
    for(const auto& g:groups)tile.groupSpecies.push_back(tile.faunaKinds[g.members.front()].species);
    tile.herds.reset(std::move(kinds),std::move(groups),kFaunaSlots,
                     uint64_t(std::hash<std::string>{}(tile.data.at("key").get<std::string>()))|1u);
    for(size_t g=0;g<tile.herds.groups().size();++g)
        for(size_t m=0;m<tile.herds.groups()[g].members.size();++m)
            if(animalDeaths.count(animalResidentId(tile.herds.groups()[g].identity,m)))
                tile.herds.setMemberEnabled(g,m,false);
    Loaded* self=&tile;
    tile.herds.setAllowed([self](uint16_t kind,saida::wildlife::Vec2 at) {
        const auto& grid=self->faunaGrid;
        if(grid.cells.empty()||kind>=self->faunaKinds.size())return false;
        const int i=grid.index(at.x,at.y);
        if(i<0)return false;
        const auto& k=self->faunaKinds[kind];
        if(k.species->bird.enabled&&k.species->bird.flies)return grid.cells[size_t(i)]!=uint8_t(r1::Habitat::None);
        return r1::suitability(*k.species,k.range,grid,i)>0;
    });
    tile.herds.setFlightLandingAllowed([self](uint16_t kind,saida::wildlife::Vec2 at) {
        const int i=self->faunaGrid.index(at.x,at.y);
        if(i<0||kind>=self->faunaKinds.size())return false;
        const auto& k=self->faunaKinds[kind];return r1::suitability(*k.species,k.range,self->faunaGrid,i)>0;
    });
    tile.herds.setFlightGround([this,self](saida::wildlife::Vec2 at){return float(groundUnder(*self,at.x,at.y,0.));});
    tile.herds.setFlightClear([this,self](auto a,float ay,auto b,float by){return birdFlightClear(*self,a,ay,b,by);});
    tile.beasts.resize(kFaunaSlots);
    tile.faunaActiveAge=1e9;
    if(!tile.residents.empty()) {
        std::map<std::string,int> count;
        for(const auto& r:tile.residents)++count[r.species];
        std::string said;
        for(const auto& [id,n]:count)said+=(said.empty()?"":", ")+id+" x"+std::to_string(n);
        saida::Log::info("[World fauna] ",tile.data.value("key",std::string()),": ",tile.residents.size()," resident groups (",said,")");
    }
}
// Which groups are out now: their hour, their season, the rain. Each group
// draws against a stable share, so the same group is out for the same half
// hour on every machine.
void faunaActivity(Loaded& tile,double hour,int month,const r1::WeatherState& weather) {
    const double lat=tile.data.at("lat").get<double>();
    const uint64_t slot=uint64_t(std::floor(hour*2.));
    for(size_t g=0;g<tile.herds.groups().size()&&g<tile.groupSpecies.size();++g) {
        const r1::Species& s=*tile.groupSpecies[g];
        const auto& kind=tile.faunaKinds[tile.herds.groups()[g].members.front()];
        double out=r1::activityAt(s.activity,hour)*r1::seasonAt(s,kind.range,month,lat);
        if(s.bird.enabled&&s.bird.flies) {
            const double season=r1::seasonAt(s,kind.range,month,lat);
            tile.herds.setActive(g,season>0.);
            tile.herds.setFlightActivity(g,float(r1::birdActivityAt(s,hour,month,lat,weather.known?weather.rain:0.,weather.known?weather.windSpeed:0.)));
            continue;
        }
        if(weather.known&&(weather.rain>.5||weather.snowfall>.3))out*=s.rain;
        const uint64_t draw=std::hash<uint64_t>{}(tile.herds.groups()[g].identity*0x9E3779B97F4A7C15ull+slot);
        tile.herds.setActive(g,double(draw%10000)/10000.<out);
    }
}
int localMonth() {
    double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
    const std::time_t local=std::time_t(unixSeconds+lon/15.*3600.);
    std::tm parts{};
#ifdef _WIN32
    gmtime_s(&parts,&local);
#else
    gmtime_r(&local,&parts);
#endif
    return parts.tm_mon+1;
}
void updateFauna(float delta) {
    SAIDA_PROFILE_SCOPE("World/Fauna");
    const double dt=std::min(.05,double(delta));
    updateAnimalRemains();
    const double hour=localSolarHour();
    const int month=localMonth();
    const auto weather=effectiveWeather();
    size_t remaining=kFaunaAnimals;
    const bool onFoot=!driving&&!sailing&&!piloting&&!swimming;
    for(auto tile:ring) {
        auto found=loaded.find(tile.key());
        if(found==loaded.end())continue;
        Loaded& l=found->second;
        if(l.herds.groups().empty())continue;
        refreshBirdPerches(l);
        const double wind=weather.known?weather.windSpeed:0.,from=r1::radians(weather.windFrom);
        l.herds.setWind({float(-std::sin(from)*wind),float(std::cos(from)*wind)});
        l.faunaActiveAge+=dt;
        if(l.faunaActiveAge>=2.){faunaActivity(l,hour,month,weather);l.faunaActiveAge=0;}
        const glm::dvec3 eye=cameraTileLocal(l);
        saida::wildlife::Scene scene;
        scene.observer={float(eye.x),float(eye.z)};
        const glm::vec3 ahead=camera->transform().rotation*glm::vec3(0,0,-1);
        const glm::dvec3 look=glm::transpose(l.frame.basis)*origin.basis*glm::dvec3(ahead.x,ahead.y,ahead.z);
        const double flat=std::hypot(look.x,look.z);
        if(flat>1e-6)scene.facing={float(look.x/flat),float(look.z/flat)};
        if(onFoot) {
            const glm::dvec3 p=l.frame.local(ecef(lon,lat,alt));
            // A sprinting walker is feared from further than a stroller.
            const double pace=std::hypot(footEast,footNorth);
            scene.threats.push_back({{float(p.x),float(p.z)},float(pace>3.?1.4:1.)});
        }
        if(driving) {
            const glm::dvec3 p=l.frame.local(ecef(carLon,carLat,carAlt));
            scene.threats.push_back({{float(p.x),float(p.z)},float(std::abs(carSpeed)>3.?1.5:.6)});
        }
        if(faunaShotWanted())scene.threats.clear();  // a sitter for the portrait
        l.herds.setBudget(remaining);
        {
            SAIDA_PROFILE_SCOPE("World/Fauna/Herds");
            l.herds.update(float(dt),scene);
        }
        remaining-=std::min(remaining,l.herds.live());
        SAIDA_PROFILE_SCOPE("World/Fauna/Sync");
        syncBeasts(l,eye,dt);
    }
    checkFaunaSmoke();
    faunaShot(dt);
    sayFauna(dt);
}
// Every 20 s, what lives around the player: groups out, spawned, animals
// alive and drawn, and the nearest home -- so a walk without animals says why.
double faunaSaidAge=0;
void sayFauna(double dt) {
    faunaSaidAge+=dt;
    if(faunaSaidAge<(smoke?5.:20.))return;
    faunaSaidAge=0;
    size_t groups=0,out=0,spawned=0,live=0,drawn=0,birds=0,flying=0,perched=0,perches=0;
    double nearest=1e9;std::string what="none";
    for(auto& [key,t]:loaded) {
        const glm::dvec3 me=t.frame.local(ecef(lon,lat,alt));
        for(size_t g=0;g<t.herds.groups().size();++g) {
            ++groups;
            const auto& group=t.herds.groups()[g];
            const bool active=t.herds.active(g);
            out+=active;spawned+=t.herds.spawned(g);
            const double d=std::hypot(group.home.x-me.x,group.home.y-me.z);
            if(d<nearest) {
                nearest=d;
                const auto& k=t.herds.kinds()[group.members.front()];
                what=t.faunaKinds[group.members.front()].species->id+(active?" (out":" (resting")+
                     ", seen from "+std::to_string(int(k.viewRange))+" m)";
            }
        }
        live+=t.herds.live();
        perches+=t.birdPerches.size();
        for(const auto& a:t.herds.animals())if(a.alive&&t.faunaKinds[a.kind].species->bird.enabled) {
            ++birds;
            flying+=a.flightPhase==saida::wildlife::FlightPhase::Takeoff||a.flightPhase==saida::wildlife::FlightPhase::Cruise||a.flightPhase==saida::wildlife::FlightPhase::Approach;
            perched+=a.flightPhase==saida::wildlife::FlightPhase::Perched;
        }
        for(const auto& b:t.beasts)drawn+=b.node&&b.node->enabled();
    }
    saida::Log::info("[World fauna] around the player: ",groups," groups, ",out," out, ",spawned," spawned, ",live,
                     " animals alive, ",drawn," drawn; nearest home ",int(nearest)," m: ",what);
    saida::Log::info("[World birds] ",birds," alive, ",flying," flying, ",perched," perched; ",perches," bark support points on mounted trees");
}
// R1WORLD_FAUNA_SHOT=<png> with --smoke photographs the nearest animal out
// (R1WORLD_FAUNA_SHOT_SPECIES narrows it to a species id) from its side, a
// few body lengths away, then ends the run: a test says the animal is built,
// only a picture says it stands on its feet, its size, facing the way it
// walks (CLAUDE.md rule 1).
std::string faunaShotTile; size_t faunaShotSlot=0; uint64_t faunaShotIdentity=0,faunaShotMoved=0; double faunaShotClock=0;
bool faunaShotWanted() const { return smoke&&std::getenv("R1WORLD_FAUNA_SHOT"); }
void faunaShot(double dt) {
    if(!faunaShotWanted()||captureQueued)return;
    const char* only=std::getenv("R1WORLD_FAUNA_SHOT_SPECIES");
    Loaded* tile=faunaShotTile.empty()?nullptr:tileByKey(faunaShotTile);
    if(!tile||faunaShotSlot>=tile->herds.animals().size()||!tile->herds.animals()[faunaShotSlot].alive||
       tile->herds.animals()[faunaShotSlot].identity!=faunaShotIdentity) {
        if(tile&&faunaShotSlot<tile->herds.animals().size()) {
            const auto& gone=tile->herds.animals()[faunaShotSlot];
            const glm::dvec3 eye=cameraTileLocal(*tile);
            saida::Log::info("[World fauna E2E] lost the sitter: alive=",gone.alive," identity ",gone.identity,"/",faunaShotIdentity,
                             " state=",int(gone.state)," at ",std::hypot(gone.position.x-eye.x,gone.position.y-eye.z),
                             " m from the camera, group active=",tile->herds.active(gone.group));
        }
        // The nearest animal out, drawn and big enough to be seen.
        double best=1e9;faunaShotTile.clear();
        for(auto& [key,t]:loaded) {
            const glm::dvec3 me=t.frame.local(ecef(lon,lat,alt));
            for(size_t i=0;i<t.herds.animals().size()&&i<t.beasts.size();++i) {
                const auto& a=t.herds.animals()[i];
                if(!a.alive||!t.beasts[i].node)continue;
                const auto& k=t.faunaKinds[a.kind];
                if(only&&k.species->id.find(only)==std::string::npos)continue;
                const double d=std::hypot(a.position.x-me.x,a.position.y-me.z);
                if(d<best){best=d;faunaShotTile=key;faunaShotSlot=i;faunaShotIdentity=a.identity;}
            }
        }
        faunaShotClock=0;
        if(faunaShotTile.empty()) {
            // Nobody out near him: walk him to the nearest group that is,
            // close enough for it to come, facing away so it may.
            double nearest=1e9;Loaded* home=nullptr;size_t group=0;
            for(auto& [key,t]:loaded) {
                const glm::dvec3 me=t.frame.local(ecef(lon,lat,alt));
                for(size_t g=0;g<t.herds.groups().size();++g) {
                    const auto& k=t.faunaKinds[t.herds.groups()[g].members.front()];
                    if(!t.herds.active(g)||(only&&k.species->id.find(only)==std::string::npos))continue;
                    const auto h=t.herds.groups()[g].home;
                    const double d=std::hypot(h.x-me.x,h.y-me.z);
                    if(d<nearest){nearest=d;home=&t;group=g;}
                }
            }
            if(!home)return;
            // First to its home, inside its roam; then to where it is in it.
            saida::wildlife::Vec2 h=home->herds.groups()[group].home,where;
            const bool known=home->herds.whereabouts(group,where);
            const uint64_t step=home->herds.groups()[group].identity*2+(known?1:0);
            if(faunaShotMoved==step)return;
            faunaShotMoved=step;
            if(known)h=where;
            const double reach=home->herds.kinds()[home->herds.groups()[group].members.front()].viewRange*.6;
            const double ground=groundUnder(*home,h.x+reach,h.y,0.);
            const auto point=home->frame.origin+home->frame.basis*glm::dvec3(h.x+reach,ground,h.y);
            const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
            lon=geo.x;lat=geo.y;alt=geo.z;yaw=90.;
            saida::Log::info("[World fauna E2E] walked to ",home->faunaKinds[home->herds.groups()[group].members.front()].species->id,
                             known?" where it is":"'s home",", ",int(reach)," m off, from ",int(nearest)," m");
            return;
        }
        tile=tileByKey(faunaShotTile);
        saida::Log::info("[World fauna E2E] portrait of ",tile->faunaKinds[tile->herds.animals()[faunaShotSlot].kind].species->id,
                         " (",tile->faunaKinds[tile->herds.animals()[faunaShotSlot].kind].model,") ",best," m away");
    }
    const auto& a=tile->herds.animals()[faunaShotSlot];
    const auto& k=tile->faunaKinds[a.kind];
    const AnimalKind& kind=animalKinds.at(k.model);
    const double size=std::max(kind.length,kind.height)*k.species->scale;
    const double distance=std::max(2.5,size*3.5+2.);
    // From its right side, at eye height for a walker.
    const double side=a.heading+r1::kPi/2;
    const double x=a.position.x+std::sin(side)*distance,z=a.position.y+std::cos(side)*distance;
    const double ground=groundUnder(*tile,x,z,0.);
    const auto point=tile->frame.origin+tile->frame.basis*glm::dvec3(x,ground,z);
    const auto geo=r1::ecefToGeodetic(point.x,point.y,point.z);
    lon=geo.x;lat=geo.y;alt=geo.z;
    yaw=std::atan2(a.position.x-x,-(a.position.y-z))/rad;
    pitch=-std::atan2(playerHeight*.9,distance)/rad*.8;
    faunaShotClock+=dt;
    if(faunaShotClock<2.5)return;
    saida::CaptureRequest shot;
    shot.pngPath=std::getenv("R1WORLD_FAUNA_SHOT");shot.frame=20;shot.fixedStep=1.f/60.f;shot.settleTimeoutFrames=900;
    captureQueued=true;
    saida::Log::info("[World fauna E2E] photographing ",k.species->id," to ",shot.pngPath);
    engine.captureFrameThenExit(shot);
}
void syncBeasts(Loaded& tile,const glm::dvec3& eye,double dt) {
    const auto& animals=tile.herds.animals();
    if(tile.beasts.size()<animals.size())tile.beasts.resize(animals.size());
    for(size_t i=0;i<animals.size();++i) {
        const auto& a=animals[i];
        Beast& b=tile.beasts[i];
        if(!a.alive){if(b.node&&b.node->enabled())b.node->setEnabled(false);continue;}
        const auto& kindOf=tile.faunaKinds[a.kind];
        const std::string& model=kindOf.model;
        const double scale=kindOf.species->scale;
        if(!b.node||b.model!=model) {
            if(!makeBeast(*tile.node,model,scale,b))continue;
        }
        AnimalKind& kind=animalKinds.at(model);
        if(b.identity!=a.identity){b.identity=a.identity;b.shown=false;}
        if(!b.node->enabled())b.node->setEnabled(true);
        const double x=a.position.x,z=a.position.y;
        const bool bird=kindOf.species->bird.enabled&&kindOf.species->bird.flies;
        // A bird in the add-on's flight already knows the ground under it
        // (setFlightGround): asking it again costs as much as its whole flight.
        const double ground=bird&&a.flightInitialized?double(a.elevation-a.height):groundUnder(tile,x,z,b.shown?b.y:eye.y);
        b.y=ground;
        const bool aloft=bird?(a.flightPhase!=saida::wildlife::FlightPhase::Ground&&a.flightPhase!=saida::wildlife::FlightPhase::Perched):a.height>.01f;
        b.node->transform().position=glm::vec3(float(x),float((bird&&a.flightInitialized?a.elevation:ground+a.height)-kind.ground*scale),float(z));
        // The plugin's heading is from +z toward +x, a model's head is at
        // `forward` in its own frame: turn by the difference, eased.
        const double want=double(a.heading)-kind.forward;
        if(!b.shown){b.yaw=want;b.shown=true;}
        else b.yaw+=std::remainder(want-b.yaw,6.283185307179586)*(1-std::exp(-8.*dt));
        auto rotation=glm::angleAxis(float(b.yaw),glm::vec3(0,1,0));
        if(bird) {
            const glm::vec3 forward(float(std::sin(kind.forward)),0,float(std::cos(kind.forward)));
            rotation=rotation*glm::angleAxis(a.bank,forward)*glm::angleAxis(-a.pitch,glm::cross(glm::vec3(0,1,0),forward));
            if(b.birdPose)b.birdPose->setFlight(aloft,a.flap,float(kindOf.species->bird.flapHz),a.speed,tile.herds.groups()[a.group].identity+a.member);
        }
        b.node->transform().rotation=rotation;
        auto [clip,rate]=beastClip(kind,a.state,a.speed,scale,a.identity,aloft);
        if(bird&&aloft&&a.flap<.5f&&kind.roles.count("glide"))clip=kind.roles.at("glide").front();
        const float bucket=std::round(rate*10.f)/10.f;
        if(!clip.empty()&&(clip!=b.clip||std::abs(bucket-b.playback)>.05f)) {
            saida::ClipView view;view.name=clip+"@"+std::to_string(int(bucket*10));view.source=clip;view.speed=bucket;
            b.animator->playView(view,b.clip.empty()?0.f:.25f);
            b.clip=clip;b.playback=bucket;
        }
        // Animation LOD, as the crowd's: every frame close by, then 15, 8, 4 Hz held.
        const double d=std::hypot(x-eye.x,z-eye.z);
        const float poseRate=bird?(d<40.?0.f:d<100.?20.f:12.f):(d<15.?0.f:d<40.?15.f:d<80.?8.f:4.f);
        if(poseRate!=b.poseRate){b.animator->setPoseRate(poseRate,saida::Animator::PoseRateMode::Hold);b.poseRate=poseRate;}
    }
}
// The smoke builds every kind the mounted tiles' residents use, both levels,
// and says how many live; a model that cannot be drawn fails it by name.
void checkFaunaSmoke() {
    if(!smoke||faunaSmokeSaid||loaded.empty())return;
    std::set<std::string> needed;
    size_t groups=0;
    for(const auto& [key,t]:loaded){groups+=t.residents.size();for(const auto& r:t.residents)for(const auto& m:r.models)needed.insert(m);}
    bool pending=false;
    for(const auto& model:needed) {
        auto it=animalKinds.find(model);
        if(it==animalKinds.end()){saida::Log::error("[World E2E] FAIL fauna: ",model," is not measured");testFailed=true;continue;}
        if(it->second.failed)continue;
        if(!animalKind(model))pending=true;
    }
    if(pending)return;
    faunaSmokeSaid=true;
    size_t live=0;
    for(const auto& [key,t]:loaded)live+=t.herds.live();
    const bool refused=std::any_of(needed.begin(),needed.end(),[&](const std::string& m){auto it=animalKinds.find(m);return it==animalKinds.end()||it->second.failed;});
    saida::Log::info("[World E2E] ",refused?"FAIL":"PASS"," fauna: ",needed.size()," models built for ",groups,
                     " resident groups on ",loaded.size()," tiles, ",live," animals out now");
    if(refused)testFailed=true;
}

// ── indoors ─────────────────────────────────────────────────────────────────
//
// A furnished home sometimes has a spider on a wall, a trail of ants in the
// kitchen, a mouse, a wasp at the window, a cat asleep: rare, drawn from the
// building and the day (gen/fauna.cpp planIndoor). They stand in the room's
// node and go with it.
void planRoomAnimals(LiveInterior& room,const Loaded& t) {
    r1::IndoorContext context;
    const json& f=t.data.value("fauna",json());
    context.country=f.value("country",std::string());context.climate=f.value("climate",std::string());
    context.lon=t.data.at("lon").get<double>();context.lat=t.data.at("lat").get<double>();
    context.hour=localSolarHour();context.month=localMonth();
    double unixSeconds=double(std::time(nullptr));gameTime(unixSeconds);
    context.day=int64_t(std::floor((unixSeconds+context.lon/15.*3600.)/86400.));
    context.rural=t.faunaGrid.cells.empty()?false:t.faunaGrid.builtShare(room.plan.door.x,room.plan.door.y)<.15;
    room.animals=r1::planIndoor(room.plan,room.layout,context);
    room.beasts.clear();room.animalsPlaced=false;
    for(const auto& a:room.animals)
        saida::Log::info("[World fauna] ",room.plan.name.empty()?"a home":room.plan.name," shelters ",a.species,
                         a.place=="trail"?" ("+std::to_string(a.count)+" ants)":"");
}
void updateRoomAnimals(LiveInterior& room,const Loaded& t,double dt) {
    if(room.animals.empty()||!room.node)return;
    if(!room.animalsPlaced) {
        // Wait for every model still streaming in; a refused one is left out.
        for(const auto& a:room.animals) {
            auto it=animalKinds.find(a.model);
            if(it!=animalKinds.end()&&!it->second.failed&&!animalKind(a.model))return;
        }
        for(size_t i=0;i<room.animals.size();++i) {
            const auto& a=room.animals[i];
            const r1::Species* s=r1::speciesById(a.species);
            const int n=a.place=="trail"?a.count:1;
            for(int k=0;k<n;++k) {
                if(animalDeaths.count(indoorAnimalId(room,i,k)))continue;
                IndoorBeast beast;beast.animal=i;beast.member=k;
                if(!makeBeast(*room.node,a.model,s?s->scale:1.,beast.beast))continue;
                room.beasts.push_back(std::move(beast));
            }
        }
        room.animalsPlaced=true;
    }
    const auto eye=cameraTileLocal(t);
    for(auto& ib:room.beasts) {
        if(ib.gone||!ib.beast.node)continue;
        const auto& a=room.animals[ib.animal];
        const AnimalKind& kind=animalKinds.at(a.model);
        ib.clock+=dt;
        glm::dvec3 at(a.at.x,room.plan.floor+a.height,a.at.y);
        double heading=a.yaw;
        std::string role="idle";
        if(a.place=="trail") {
            // Ants file along the wall and back, a few centimetres apart.
            const double v=std::max(.01,kind.walkSpeed>.005?kind.walkSpeed:.03);
            const double run=std::fmod(ib.clock*v+ib.member*a.length/std::max(1,a.count),2*a.length);
            const double s=run<a.length?run:2*a.length-run;
            heading=a.yaw+(run<a.length?0.:r1::kPi);
            at.x+=std::sin(a.yaw)*s;at.z+=std::cos(a.yaw)*s;role="walk";
        } else if(a.place=="air") {
            at.x+=.35*std::sin(ib.clock*.9+ib.member);at.z+=.35*std::cos(ib.clock*.7);at.y+=.12*std::sin(ib.clock*2.3);
            heading=ib.clock*.8;role="fly";
        } else if(a.place=="floor") {
            const r1::Species* s=r1::speciesById(a.species);
            const double close=std::hypot(eye.x-at.x,eye.z-at.z);
            if(s&&s->dart&&!ib.darting&&close<std::max(1.,s->flee)){ib.darting=true;ib.dartClock=0;}
            if(ib.darting) {
                // Off along the wall and gone under the furniture.
                ib.dartClock+=dt;ib.offset+=dt*1.6;
                if(ib.dartClock>1.){ib.gone=true;ib.beast.node->setEnabled(false);continue;}
            }
            at.x+=std::sin(a.yaw)*ib.offset;at.z+=std::cos(a.yaw)*ib.offset;
        }
        ib.beast.node->transform().position=glm::vec3(float(at.x),float(at.y-kind.ground),float(at.z));
        glm::quat q=glm::angleAxis(float(heading-kind.forward),glm::vec3(0,1,0));
        if(a.place=="wall") {
            // Belly to the wall: the model's up turned onto the wall's normal.
            const glm::vec3 normal(float(a.normal.x),0.f,float(a.normal.y));
            const glm::vec3 axis=glm::cross(glm::vec3(0,1,0),normal);
            const glm::quat onto=glm::length(axis)>1e-4f?glm::angleAxis(1.5707963f,glm::normalize(axis)):glm::quat(1,0,0,0);
            q=onto*glm::angleAxis(float(a.yaw-kind.forward),glm::vec3(0,1,0));
        }
        ib.beast.node->transform().rotation=q;
        const auto list=kind.roles.count(role)?kind.roles.at(role):kind.roles.count("idle")?kind.roles.at("idle"):
                        kind.roles.empty()?std::vector<std::string>{}:kind.roles.begin()->second;
        if(list.empty())continue;
        const std::string& clip=list.front();
        // A spider on its wall waits, still: its pose is held, not played.
        const bool still=a.place=="wall";
        if(clip!=ib.beast.clip) {
            saida::ClipView view;view.name=clip;view.source=clip;view.speed=1.f;
            ib.beast.animator->playView(view,0.f);ib.beast.clip=clip;
            ib.beast.animator->setPoseRate(still?.001f:0.f,saida::Animator::PoseRateMode::Hold);
        }
    }
}
