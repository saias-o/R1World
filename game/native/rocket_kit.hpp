#pragma once
#include "combat_kit.hpp"
#include "rocket.hpp"

namespace r1 {
inline constexpr glm::vec3 kLauncherMuzzle{0,.12f,.63f};
inline constexpr glm::vec3 kLauncherSupport{0,-.025f,.25f};

inline std::unique_ptr<saida::Node> buildRocketLauncher(saida::ResourceManager& resources) {
    detail::PistolBuilder body,steel,rubber,markings,glass;
    // Closed radial profiles: outside runs toward +Z, the inside returns
    // toward -Z. Annular ends, bevels and bore all have their own outward
    // normals and winding; no double-sided material hiding missing geometry.
    auto lathe=[](detail::PistolBuilder& mesh,std::initializer_list<glm::vec2> points,
                  glm::vec3 centre,glm::quat rotation=glm::quat(1,0,0,0),glm::vec3 scale=glm::vec3(1)) {
        const std::vector<glm::vec2> profile(points); // z, radius
        constexpr int sides=32;
        for(size_t p=1;p<profile.size();++p) {
            const auto delta=profile[p]-profile[p-1];
            for(int i=0;i<sides;++i) {
                const uint32_t base=uint32_t(mesh.data.vertices.size());
                for(int corner=0;corner<4;++corner) {
                    const auto zr=profile[p-(corner<2?1:0)];
                    const float angle=float(i+(corner==1||corner==2))*6.28318530718f/sides;
                    const float c=std::cos(angle),s=std::sin(angle);
                    saida::Vertex v{};
                    v.pos=centre+rotation*(glm::vec3(c*zr.y,s*zr.y,zr.x)*scale);
                    v.normal=rotation*glm::normalize(glm::vec3(delta.x*c,delta.x*s,-delta.y)/scale);
                    v.tangent=glm::vec4(glm::normalize(rotation*(glm::vec3(-s,c,0)*scale)),1);
                    v.color=glm::vec3(1);v.texCoord=v.lightmapUV={float(i+(corner==1||corner==2))/sides,zr.x};
                    mesh.data.vertices.push_back(v);
                }
                for(uint32_t n:{0u,1u,2u,0u,2u,3u})mesh.data.indices.push_back(base+n);
            }
        }
    };
    const glm::vec3 axis(0,.12f,0);
    lathe(body,{{-.59f,.088f},{-.56f,.098f},{.50f,.098f},{.54f,.092f},
                {.54f,.077f},{-.59f,.077f},{-.59f,.088f}},axis);
    // Machined muzzle and flared exhaust, including their dark inner walls.
    lathe(steel,{{.49f,.098f},{.51f,.109f},{.60f,.109f},{.62f,.10f},
                 {.62f,.078f},{.60f,.075f},{.49f,.075f},{.49f,.098f}},axis);
    lathe(steel,{{-.74f,.137f},{-.72f,.145f},{-.67f,.145f},{-.57f,.104f},
                 {-.55f,.104f},{-.55f,.078f},{-.67f,.116f},{-.74f,.119f},{-.74f,.137f}},axis);
    for(float z:{-.42f,.34f})
        lathe(rubber,{{z-.024f,.098f},{z-.016f,.112f},{z+.016f,.112f},
                      {z+.024f,.098f},{z-.024f,.098f}},axis);
    lathe(rubber,{{-.38f,.099f},{-.36f,.106f},{-.13f,.106f},{-.11f,.099f},{-.38f,.099f}},axis);
    rubber.box({0,-.007f,-.26f},{.14f,.045f,.25f}); // shoulder cushion
    steel.box({0,.023f,.015f},{.07f,.06f,.16f}); // receiver
    const auto gripTurn=glm::angleAxis(1.42f,glm::vec3(1,0,0));
    lathe(rubber,{{-.025f,0},{-.025f,.026f},{-.012f,.034f},{.105f,.03f},
                  {.12f,.024f},{.12f,0}},glm::vec3(0,.015f,-.015f),gripTurn,{.73f,1,1});
    steel.box({0,-.045f,.065f},{.016f,.012f,.095f}); // trigger guard
    steel.box({0,-.015f,.108f},{.016f,.065f,.012f},-.2f);
    steel.box({0,-.005f,.051f},{.009f,.036f,.008f},.22f);
    lathe(rubber,{{-.025f,0},{-.025f,.026f},{-.013f,.035f},{.075f,.032f},
                  {.088f,.026f},{.088f,0}},glm::vec3(0,.03f,.25f),gripTurn,{.85f,1,1});
    for(int i=0;i<4;++i) {
        rubber.box({0,-.018f-float(i)*.021f,-.02f},{.055f,.008f,.063f},-.15f);
        steel.box({0,.224f,-.035f+float(i)*.045f},{.055f,.012f,.018f});
    }
    steel.box({0,.213f,.035f},{.04f,.025f,.22f}); // sight rail
    steel.box({-.08f,.235f,.055f},{.17f,.025f,.05f});
    const glm::vec3 optic(-.125f,.257f,0);
    lathe(rubber,{{-.085f,.031f},{-.075f,.039f},{.16f,.039f},{.18f,.033f},
                  {.18f,.026f},{-.085f,.026f},{-.085f,.031f}},optic);
    lathe(glass,{{.171f,0},{.171f,.026f},{.176f,.026f},{.176f,0}},optic);
    lathe(glass,{{-.082f,0},{-.082f,.026f},{-.078f,.026f},{-.078f,0}},optic);
    // Small identification bands and fasteners give scale without bright paint.
    lathe(markings,{{.402f,.0982f},{.418f,.0982f}},axis);
    for(int i=0;i<3;++i)markings.box({-.099f,.125f,.02f+float(i)*.022f},{.003f,.022f,.012f});
    for(float z:{-.42f,.34f})for(float side:{-1.f,1.f})
        steel.box({side*.112f,.12f,z},{.012f,.022f,.022f});
    auto root=std::make_unique<saida::Node>("Rocket launcher");
    const std::array<detail::PistolBuilder*,5> parts{&body,&steel,&rubber,&markings,&glass};
    const char* names[]={"olive-tube","machined-metal","rubber-furniture","identification","optic-lenses"};
    const glm::vec3 colors[]={{.14f,.17f,.075f},{.075f,.081f,.086f},{.023f,.027f,.025f},{.38f,.32f,.12f},{.025f,.10f,.12f}};
    const float roughness[]={.65f,.32f,.82f,.62f,.13f},metallic[]={.1f,.8f,0,0,.45f};
    for(size_t i=0;i<parts.size();++i) {
        const std::string key=std::string("rocket/launcher/")+names[i];
        auto* mesh=resources.getMesh(resources.queueMemoryMesh(saida::prepareMesh(std::move(parts[i]->data)),key));
        if(!mesh)throw std::runtime_error("Rocket launcher geometry allocation refused");
        saida::MaterialDesc material;material.baseColor=glm::vec4(colors[i],1);
        material.roughness=roughness[i];material.metallic=metallic[i];
        root->addChild(std::make_unique<saida::MeshNode>(key,mesh,resources.getMaterial(material)));
    }
    return root;
}
// Project-authored geometry, shared by the entire projectile pool. +Z is the
// bore, matching the existing hand attachment and aiming convention.
inline std::unique_ptr<saida::Node> buildRocketModel(saida::ResourceManager& resources,bool launcher) {
    if(launcher)return buildRocketLauncher(resources);
    detail::PistolBuilder metal,trim;
    auto tube=[&](detail::PistolBuilder& mesh,float z0,float z1,float r0,float r1,float y) {
        constexpr int sides=20;
        for(int i=0;i<sides;++i) {
            const uint32_t base=uint32_t(mesh.data.vertices.size());
            for(int corner=0;corner<4;++corner) {
                const bool end=corner>=2;
                const float angle=float((i+(corner==1||corner==2))*6.28318530718/sides);
                const float radius=end?r1:r0;
                saida::Vertex v{};v.pos={std::cos(angle)*radius,y+std::sin(angle)*radius,end?z1:z0};
                v.normal=glm::normalize(glm::vec3(std::cos(angle),std::sin(angle),(r0-r1)/(z1-z0)));
                v.color=glm::vec3(1);v.texCoord=v.lightmapUV={float(i)/sides,float(end)};
                v.tangent={-std::sin(angle),std::cos(angle),0,1};mesh.data.vertices.push_back(v);
            }
            for(uint32_t n:{0u,1u,2u,0u,2u,3u})mesh.data.indices.push_back(base+n);
        }
    };
    {
        tube(metal,-.65f,.35f,.14f,.14f,0);
        tube(trim,.35f,.86f,.14f,0,0);
        tube(trim,-.7f,-.65f,.08f,.14f,0);
        tube(trim,.15f,.22f,.148f,.148f,0);
        metal.box({0,0,-.48f},{.65f,.025f,.30f});
        metal.box({0,0,-.48f},{.025f,.65f,.30f});
    }
    auto root=std::make_unique<saida::Node>(launcher?"Rocket launcher":"Heavy rocket");
    for(auto* part:{&metal,&trim}) {
        const std::string key=std::string("rocket/")+(launcher?"launcher/":"projectile/")+(part==&metal?"body":"trim");
        auto* mesh=resources.getMesh(resources.queueMemoryMesh(saida::prepareMesh(std::move(part->data)),key));
        if(!mesh)throw std::runtime_error("Rocket geometry allocation refused");
        saida::MaterialDesc material;material.baseColor=part==&metal?glm::vec4(.12f,.15f,.075f,1):glm::vec4(.035f,.04f,.045f,1);
        material.roughness=.42f;material.metallic=part==&metal?.35f:.65f;
        root->addChild(std::make_unique<saida::MeshNode>(key,mesh,resources.getMaterial(material)));
    }
    return root;
}

class RocketEffects {
    struct Blast {std::array<saida::ParticleSystemNode*,4> layers{};saida::LightNode* light=nullptr;float age=10;};
    std::array<Blast,rocket::kExplosions> blasts_{};
public:
    void build(saida::Node& world) {
        for(auto& b:blasts_) {
            for(int k=0;k<4;++k) {
                auto* e=world.createChild<saida::ParticleSystemNode>();b.layers[k]=e;
                e->setName("SaidaFX rocket explosion "+std::to_string(k));
                e->effectClass=k==1?saida::ParticleSystemNode::EffectClass::Smoke:saida::ParticleSystemNode::EffectClass::Explosion;
                e->looping=false;e->playing=false;e->spawnRate=0;e->gravity={0,1.5f,0};
                e->shape=saida::ParticleSystemNode::Shape::Sphere;e->radius=.7f;
                if(k==0) { // incandescent expanding core
                    e->maxParticles=96;e->burstCount=64;e->lifetime=.85f;e->startSize=1.4f;e->endSizeScale=2.8f;
                    e->startSpeed=9;e->drag=3;e->emissive=7;
                    e->startColor={1,.58f,.12f,1};e->endColor={.45f,.055f,.008f,0};
                } else if(k==1) {
                    e->blendMode=saida::ParticleSystemNode::BlendMode::Alpha;
                    e->maxParticles=72;e->burstCount=52;e->lifetime=3.2f;e->startSize=1.3f;e->endSizeScale=4.5f;
                    e->startSpeed=5;e->drag=1.8f;e->emissive=0;
                    e->startColor={.12f,.105f,.09f,.72f};e->endColor={.19f,.18f,.17f,0};
                } else if(k==2) {
                    e->maxParticles=128;e->burstCount=96;e->lifetime=1.5f;e->startSize=.08f;e->endSizeScale=.1f;
                    e->startSpeed=23;e->drag=.6f;e->stretch=5;e->gravity={0,-9.8f,0};e->emissive=8;
                    e->startColor={1,.72f,.25f,1};e->endColor={.8f,.1f,.015f,0};
                } else { // expanding particle ring, using the supported SaidaFX shape
                    e->shape=saida::ParticleSystemNode::Shape::Ring;e->radius=1.2f;e->ringThickness=.15f;
                    e->blendMode=saida::ParticleSystemNode::BlendMode::Alpha;
                    e->maxParticles=80;e->burstCount=72;e->lifetime=.9f;e->startSize=.6f;e->endSizeScale=3;
                    e->startSpeed=16;e->drag=1.2f;e->gravity={0,.3f,0};e->emissive=0;
                    e->startColor={.26f,.23f,.18f,.65f};e->endColor={.22f,.20f,.17f,0};
                }
            }
            b.light=world.createChild<saida::LightNode>("Rocket blast light",saida::LightType::Point);
            b.light->color={1,.48f,.12f};b.light->range=32;b.light->castShadows=false;b.light->setEnabled(false);
        }
    }
    void explode(glm::vec3 at) {
        auto& b=*std::max_element(blasts_.begin(),blasts_.end(),[](const auto& a,const auto& b){return a.age<b.age;});
        b.age=0;
        for(auto* e:b.layers){e->consumeBurstCount();e->transform().position=at;e->burst();}
        b.light->transform().position=at;b.light->intensity=65;b.light->setEnabled(true);
    }
    void update(float dt) {
        for(auto& b:blasts_) {b.age+=dt;b.light->intensity=65*std::exp(-b.age*12);b.light->setEnabled(b.age<.55f);}
    }
    void clear() {for(auto& b:blasts_){b.age=10;if(b.light)b.light->setEnabled(false);for(auto* e:b.layers)if(e){e->stop();e->consumeBurstCount();}}}
    void rebase(glm::mat3 rotation,glm::vec3 translation) {
        for(auto& b:blasts_) {for(auto* e:b.layers)e->transform().position=rotation*e->transform().position+translation;
            b.light->transform().position=rotation*b.light->transform().position+translation;}
    }
};
}
