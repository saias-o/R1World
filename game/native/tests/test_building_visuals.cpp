#include "check.hpp"
#include "gen/buildings.hpp"

#include <algorithm>

using namespace r1;

namespace {
Ring rectangle(double x, double z, double width = 12, double depth = 8) {
    return {{x,z},{x+width,z},{x+width,z+depth},{x,z+depth}};
}
BuildingOutput visualBuildings(const std::vector<std::pair<Ring,Tags>>& input,
                               BuildingLod lod = BuildingLod::Full, bool textured = false) {
    std::vector<OsmWay> owned;
    int64_t id=900;
    for (const auto& [ring,tags] : input) {
        owned.push_back({id++,ring,tags});owned.back().points.push_back(ring.front());
    }
    std::vector<const OsmWay*> ways;
    for (const auto& way : owned) ways.push_back(&way);
    MaterialFor material;
    if (textured) material=[](const Swatch& s,bool doubleSided) {
        Material m;m.name=s.name;m.color={s.color[0],s.color[1],s.color[2],1};m.doubleSided=doubleSided;
        m.baseColorTexture="scan_albedo";m.normalTexture="scan_normal";m.metallicRoughnessTexture="scan_mr";
        return m;
    };
    return buildBuildings(ways,[](double x,double z){return P3{x,0,z};},profileByKey("CHAMONIX"),
                          {0,0},-1,0,material,material,lod);
}
size_t triangleCount(const std::vector<MeshPart>& parts) {
    size_t count=0;for (const auto& part:parts) count+=part.mesh.indices.size()/3;return count;
}
double top(const std::vector<MeshPart>& parts) {
    double result=-1e300;
    for (const auto& part:parts) for (const auto& p:part.mesh.positions) result=std::max(result,p.y);
    return result;
}
bool covers(const std::vector<MeshPart>& parts,P3 point) {
    auto dot=[](P3 a,P3 b){return a.x*b.x+a.y*b.y+a.z*b.z;};
    auto minus=[](P3 a,P3 b){return P3{a.x-b.x,a.y-b.y,a.z-b.z};};
    for (const auto& part:parts) for (size_t i=0;i+2<part.mesh.indices.size();i+=3) {
        const auto a=part.mesh.positions[part.mesh.indices[i]],b=part.mesh.positions[part.mesh.indices[i+1]],
                   c=part.mesh.positions[part.mesh.indices[i+2]];
        const auto n=faceNormal(a,b,c),v0=minus(b,a),v1=minus(c,a),v2=minus(point,a);
        if (std::abs(dot(n,v2))>1e-6) continue;
        const double aa=dot(v0,v0),ab=dot(v0,v1),bb=dot(v1,v1),ac=dot(v0,v2),bc=dot(v1,v2);
        const double den=aa*bb-ab*ab;if (std::abs(den)<1e-12) continue;
        const double u=(bb*ac-ab*bc)/den,v=(aa*bc-ab*ac)/den;
        if (u>=-1e-8 && v>=-1e-8 && u+v<=1+1e-8) return true;
    }
    return false;
}
}  // namespace

TEST(BuildingVisuals, capture_keeps_every_baseline_triangle_in_its_building) {
    const auto out=visualBuildings({{rectangle(0,0),{{"height","9"},{"roof:shape","gabled"}}},
                                    {rectangle(40,0),{{"height","14"},{"roof:shape","flat"}}}});
    CHECK(out.visuals.size()==2 && out.staticParts.empty());
    size_t captured=0;
    for (size_t b=0;b<out.visuals.size();++b) {
        const auto& visual=out.visuals[b];CHECK(visual.id==900+int64_t(b));CHECK(visual.footprint==b);
        captured+=triangleCount(visual.reducedNear);
        for (const auto& part:visual.reducedNear) for (const auto& p:part.mesh.positions) {
            CHECK(p.x>=double(b)*40-1 && p.x<=double(b)*40+13);
        }
    }
    CHECK(captured==triangleCount(out.parts));
}

TEST(BuildingVisuals, near_models_openings_and_preserves_the_same_portal_when_reduced) {
    const auto out=visualBuildings({{rectangle(0,0),{{"building","house"},{"height","12"},{"building:levels","3"},
                                                   {"roof:shape","gabled"}}}},BuildingLod::SimpleRoofline);
    CHECK(out.interiors.size()==1 && out.visuals.size()==1);
    const auto& visual=out.visuals.front();const auto& interior=out.interiors.front();
    CHECK(triangleCount(visual.levels[0])>triangleCount(visual.reducedNear));
    bool glazing=false,reveals=false;
    for (const auto& part:visual.levels[0]) {
        glazing|=part.name.find("glazing")!=std::string::npos;
        reveals|=part.name.find("reveals")!=std::string::npos;
    }
    CHECK(glazing && reveals);
    const P3 doorway{interior.door.x,interior.floor+1.0,interior.door.y};
    CHECK(!covers(visual.levels[0],doorway));CHECK(!covers(visual.reducedNear,doorway));
    CHECK(covers(visual.levels[1],doorway));  // sealed while rooms are distant
}

TEST(BuildingVisuals, mid_keeps_concave_footprints_and_roof_height_without_details) {
    const Ring concave{{0,0},{16,0},{16,5},{6,5},{6,14},{0,14}};
    const auto out=visualBuildings({{concave,{{"height","8"},{"roof:shape","flat"}}}});
    const auto& visual=out.visuals.front();
    CHECK(visual.levels[1].size()==2);
    NEAR(top(visual.levels[1]),top(visual.levels[0]),1e-9);
    CHECK(!covers(visual.levels[1],{12,4,10}));
    for (const auto& part:visual.levels[1]) {
        CHECK(part.name=="Walls" || part.name=="Roofs");
        for (const auto& p:part.mesh.positions) CHECK(p.x<=6.000001 || p.z<=5.000001);
    }
    CHECK(triangleCount(visual.levels[1])<triangleCount(visual.reducedNear));
}

TEST(BuildingVisuals, far_is_a_closed_oriented_box_of_twelve_triangles_with_exact_height) {
    Ring ring=rectangle(-6,-4);const double angle=radians(33);
    for (auto& p:ring) p={p.x*std::cos(angle)-p.y*std::sin(angle),p.x*std::sin(angle)+p.y*std::cos(angle)};
    const auto out=visualBuildings({{ring,{{"height","11"},{"roof:height","3"},{"roof:shape","hipped"}}}},
                                  BuildingLod::Full,true);
    const auto& visual=out.visuals.front();const auto box=orientedBox(out.footprints.front());
    CHECK(triangleCount(visual.levels[2])==12);NEAR(top(visual.levels[2]),visual.high.y,1e-9);
    NEAR(visual.high.y,11,1e-9);
    for (const auto& part:visual.levels[2]) {
        const auto& m=part.material;
        CHECK(m.baseColorTexture.empty() && m.normalTexture.empty() && m.metallicRoughnessTexture.empty());
        CHECK(m.heightTexture.empty() && m.parallaxDepth==0);
        for (const auto& p:part.mesh.positions) {
            NEAR(std::abs((p.x-box.cx)*box.ux+(p.z-box.cz)*box.uz),box.halfU,1e-8);
            NEAR(std::abs((p.x-box.cx)*box.vx()+(p.z-box.cz)*box.vz()),box.halfV,1e-8);
            CHECK(p.y==visual.low.y || p.y==visual.high.y);
        }
    }
}

TEST(BuildingVisuals, near_uses_the_shared_party_wall_and_entrance_plan) {
    const auto out=visualBuildings({{rectangle(0,0,10,10),{{"building:levels","2"}}},
                                    {rectangle(10,0,10,10),{{"building:levels","2"}}}});
    CHECK(out.interiors.size()==2);
    for (const auto& visual:out.visuals) for (const auto& part:visual.levels[0]) {
        if (part.name.find("glazing")==std::string::npos) continue;
        for (const auto& p:part.mesh.positions) CHECK(std::abs(p.x-10)>0.5);
    }
    for (const auto& room:out.interiors) CHECK(std::abs(room.door.x-10)>0.1);
}

TEST(BuildingVisuals, open_canopies_and_shelters_stay_out_of_closed_visual_levels) {
    const auto out=visualBuildings({{rectangle(0,0),{{"building","roof"}}},
                                    {rectangle(30,0,4,3),{{"building","yes"},{"r1:bus-shelter","rural"}}},
                                    {rectangle(50,0),{{"building","house"}}}});
    CHECK(out.openRoofs==1 && out.stats.busShelters==1);
    CHECK(out.visuals.size()==1 && out.visuals.front().id==902);
    CHECK(!out.staticParts.empty());
    CHECK(triangleCount(out.parts)==triangleCount(out.visuals.front().reducedNear)+triangleCount(out.staticParts));
    CHECK(out.visuals.front().footprint==3);  // the three narrow shelter walls
}

TEST(BuildingVisuals, all_levels_are_deterministic_and_keep_independent_indices) {
    const std::vector<std::pair<Ring,Tags>> input{{rectangle(0,0),{{"height","12"},{"roof:shape","gabled"}}}};
    const auto a=visualBuildings(input),b=visualBuildings(input);
    for (size_t level=0;level<3;++level) {
        const auto& x=a.visuals.front().levels[level];const auto& y=b.visuals.front().levels[level];
        CHECK(x.size()==y.size());
        for (size_t part=0;part<x.size();++part) {
            CHECK(x[part].mesh.positions==y[part].mesh.positions);
            CHECK(x[part].mesh.indices==y[part].mesh.indices);
            for (const auto index:x[part].mesh.indices) CHECK(index<x[part].mesh.vertexCount());
        }
    }
}
