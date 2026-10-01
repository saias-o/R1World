#include "interiors.hpp"
#include "palette.hpp"
#include <algorithm>

namespace r1 {
std::vector<MeshPart> buildBuildingFixture(const InteriorFixture& f) {
    if(f.kind=="vehicle")return {}; // Runtime reuses the authored road fleet.
    Mesh wood(UvMode::Planar),soft,steel,ceramic,dark,paper,paint,linen,green;
    const double w=f.size.x,d=f.size.y,h=f.height;
    auto box=[](Mesh& m,double x,double z,double y,double width,double depth,double height){
        if(width>0&&depth>0&&height>0)m.addBox({x,y,z},{width,height,depth});
    };
    // Low-poly rounded cushions with analytic smooth normals. The same
    // bounded prototype is shared by every matching household fixture.
    auto cushion=[](Mesh& m,double x,double z,double y,double width,double depth,double height,double radius) {
        const double half[3]={width/2,height/2,depth/2},center[3]={x,y,z};
        radius=std::min(radius,std::min({half[0],half[1],half[2]})*.8);
        for(int axis=0;axis<3;++axis)for(double side:{-1.,1.})for(int a=0;a<4;++a)for(int b=0;b<4;++b) {
            P3 positions[4],normals[4];
            const int first=(axis+1)%3,second=(axis+2)%3;
            const int steps[4][2]={{a,b},{a+1,b},{a+1,b+1},{a,b+1}};
            for(int i=0;i<4;++i) {
                double q[3]={},inner[3],delta[3];q[axis]=side*half[axis];
                q[first]=half[first]*(steps[i][0]*.5-1.);q[second]=half[second]*(steps[i][1]*.5-1.);
                double length=0;for(int j=0;j<3;++j){inner[j]=std::clamp(q[j],-half[j]+radius,half[j]-radius);delta[j]=q[j]-inner[j];length+=delta[j]*delta[j];}
                length=std::sqrt(length);
                positions[i]={center[0]+inner[0]+radius*delta[0]/length,center[1]+inner[1]+radius*delta[1]/length,center[2]+inner[2]+radius*delta[2]/length};
                normals[i]={delta[0]/length,delta[1]/length,delta[2]/length};
            }
            if(side<0){std::swap(positions[1],positions[3]);std::swap(normals[1],normals[3]);}
            uint32_t ids[4];for(int i=0;i<4;++i)ids[i]=m.vertex(positions[i],normals[i],{});
            m.indices.insert(m.indices.end(),{ids[0],ids[1],ids[2],ids[0],ids[2],ids[3]});
        }
    };
    // Revolved ceramics and round metal tubes; flat boxes cannot describe a
    // basin, toilet bowl or chair leg. Twelve facets suffice at room distance.
    auto round=[&](Mesh& m,double x,double z,const std::vector<std::pair<double,double>>& profile,double stretch=1.) {
        for(size_t j=1;j<profile.size();++j)for(int i=0;i<12;++i) {
            double a=i*kPi/6,b=(i+1)*kPi/6;
            auto at=[&](size_t n,double t){return P3{x+profile[n].second*std::cos(t),profile[n].first,z+profile[n].second*std::sin(t)*stretch};};
            m.addQuad(at(j-1,a),at(j,a),at(j,b),at(j-1,b));
        }
    };
    auto leg=[&](double x,double z,double height){round(steel,x,z,{{.02,.025},{height,.025}});};
    auto chair=[&](double x,double z,bool reverse=false) {
        for(double dx:{-.19,.19})for(double dz:{-.19,.19})leg(x+dx,z+dz,.44);
        box(soft,x,z,.47,.46,.46,.07);
        box(soft,x,z+(reverse?-.2:.2),.7,.46,.065,.45);
    };
    auto table=[&](double width,double depth,double height) {
        box(wood,0,0,height,width,depth,.065);
        for(double x:{-width/2+.09,width/2-.09})for(double z:{-depth/2+.09,depth/2-.09})leg(x,z,height-.03);
    };
    const auto& k=f.kind;
    if(k=="rug") {
        box(linen,0,0,.013,w,d,.026);
        for(double x:{-w/2+.06,w/2-.06})box(soft,x,0,.028,.035,d-.10,.006);
    } else if(k=="prayer_mat") {
        box(soft,0,0,.0125,w,d,.025);
    } else if(k=="partition") {
        box(paint,0,0,h/2,w,d,h);
        box(wood,0,0,.055,w+.018,d+.018,.11);
    } else if(k=="sofa"||k=="pew") {
        for(double x:{-w/2+.12,w/2-.12})for(double z:{-d/2+.12,d/2-.12})leg(x,z,.17);
        box(wood,0,0,.25,w,d,.18);
        cushion(soft,0,d/2-.12,.64,w,.24,.55,.08);
        for(double x:{-w/2+.12,w/2-.12})cushion(soft,x,0,.49,.24,d,.43,.08);
        const int cushions=std::max(1,int(w/.65));
        for(int i=0;i<cushions;++i)cushion(soft,-w/2+.24+(w-.48)*(i+.5)/cushions,-.10,.44,(w-.48)/cushions-.025,d-.28,.19,.06);
        if(k=="sofa"&&w>1.5)for(double x:{-w*.28,w*.28})cushion(linen,x,d*.13,.65,.36,.18,.31,.075);
    } else if(k=="bed") {
        for(double x:{-w/2+.1,w/2-.1})for(double z:{-d/2+.1,d/2-.1})leg(x,z,.22);
        box(wood,0,0,.29,w,d,.19);cushion(linen,0,-.04,.49,w-.04,d-.10,.24,.065);
        cushion(soft,0,d/2-.06,.65,w,.14,.9,.06);
        cushion(soft,0,-d*.17,.64,w-.02,d*.62,.10,.035);
        for(double x:{-w*.24,w*.24})cushion(paper,x,d*.29,.65,w*.42,.43,.16,.06);
        box(linen,0,-d*.34,.702,w-.07,.20,.018);
    } else if(k=="bedside") {
        box(wood,0,0,.24,w,d,.46);
        for(double y:{.15,.34}){box(paint,0,-d/2-.012,y,w-.045,.02,.15);box(steel,0,-d/2-.035,y,.12,.025,.014);}
        round(ceramic,0,0,{{.49,.09},{.52,.09},{.68,.035}});
        round(linen,0,0,{{.67,.14},{.84,.10},{.84,0.}});
    } else if(k=="fridge") {
        box(paint,0,0,h/2,w,d,h);
        for(auto [y,fh]:{std::pair{h*.64,h*.67},std::pair{h*.15,h*.28}})box(ceramic,0,-d/2-.015,y,w-.02,.03,fh);
        box(steel,w*.34,-d/2-.045,h*.62,.035,.05,.34);
    } else if(k=="lamp") {
        round(dark,0,0,{{.02,.20},{.04,.20},{.04,0.}});
        round(steel,0,0,{{.04,.022},{h-.3,.022}});
        round(linen,0,0,{{h-.35,.25},{h,.17},{h,0.}});
    } else if(k=="plant") {
        round(ceramic,0,0,{{.02,.12},{.3,.20},{.32,.20},{.32,.17}});
        round(dark,0,0,{{.31,.17},{.315,.17},{.315,0.}});
        for(double a:{0.,2.1,4.2}) {
            const double x=.12*std::cos(a),z=.12*std::sin(a);
            round(wood,x,z,{{.3,.012},{h*.8,.012}});
            cushion(green,x,z,h*.7,.22,.22,h*.48,.09);
        }
    } else if(k=="kitchen") {
        box(wood,0,0,.44,w,d,.86);box(ceramic,0,0,.9,w+.04,d+.04,.06);
        const int doors=std::max(1,int(w/.55));
        for(int i=0;i<doors;++i) {
            double x=-w/2+w*(i+.5)/doors;
            box(ceramic,x,-d/2-.01,.45,w/doors-.025,.02,.76);
            box(steel,x,-d/2-.04,.7,.16,.035,.018);
            box(wood,x,d*.25,1.7,w/doors-.02,d*.48,.55);
        }
        // Recessed basin, rim and faucet to the left; four hob burners right.
        round(ceramic,-w*.22,0,{{.92,.20},{.84,.14},{.84,.0}},.7);
        round(steel,-w*.22,d*.24,{{.92,.018},{1.16,.018}});
        box(steel,-w*.22,d*.17,1.16,.035,.12,.035);
        for(double x:{w*.14,w*.34})for(double z:{-d*.19,d*.19})round(dark,x,z,{{.932,.08},{.938,.08},{.938,0.}});
        box(dark,w*.25,-d/2-.025,.49,w*.38,.015,.39);
        box(steel,w*.25,-d/2-.06,.71,w*.32,.035,.02);
        box(ceramic,w*.25,d*.26,1.43,w*.45,d*.5,.09);
        box(steel,w*.25,d*.4,1.64,.18,.13,.34);
    } else if(k=="toilet") {
        round(ceramic,0,-.06,{{0.,.16},{.15,.12},{.33,.24},{.43,.25},{.43,.19},{.30,.13},{.28,0.}},1.22);
        round(dark,0,-.06,{{.44,.25},{.445,.25},{.445,.19},{.44,.19}},1.22);
        box(ceramic,0,d/2-.12,.58,.43,.22,.35);box(steel,.1,d/2-.245,.7,.08,.016,.025);
    } else if(k=="basin") {
        box(wood,0,0,.42,w,d,.8);
        round(ceramic,0,-.03,{{.87,w*.46},{.82,w*.38},{.75,w*.2},{.75,0.}},.7);
        round(steel,0,d*.3,{{.84,.018},{1.04,.018}});box(steel,0,d*.2,1.04,.03,.14,.03);
        box(steel,0,d/2-.015,1.15,w*.84,.025,.25);
    } else if(k=="shower") {
        box(ceramic,0,0,.035,w,d,.07);
        box(ceramic,0,d/2-.03,h/2,w,.06,h);
        box(ceramic,-w/2+.03,0,h/2,.06,d,h);
        round(steel,w*.2,d*.4,{{.7,.018},{1.88,.018}});
        box(steel,w*.2,d*.28,1.9,.18,.22,.04);
        box(dark,0,0,.074,.09,.09,.009);
    } else if(k=="tv") {
        box(wood,0,0,.3,w,d,.55);box(steel,0,0,.65,.04,.08,.2);
        box(dark,0,0,.92,w,.055,.55);box(steel,0,.03,.92,w+.025,.018,.58);
    } else if(k=="desk"||k=="school_desk") {
        const double top=k=="school_desk"?.72:.76;
        box(wood,0,d*.18,top,w,.65,.06);
        for(double x:{-w/2+.08,w/2-.08})for(double z:{d*.18-.24,d*.18+.24})leg(x,z,top-.03);
        chair(0,-d*.29,true);
        if(k=="desk") {
            box(steel,w*.2,d*.28,1.02,.055,.06,.42);
            box(dark,w*.2,d*.23,1.11,.55,.045,.32);
            box(dark,w*.2,d*.02,.8,.4,.17,.025);
            box(paper,-w*.25,d*.17,.8,.3,.22,.035);
        } else box(paper,-w*.2,d*.15,.76,.25,.19,.025);
    } else if(k=="dining"||k=="meeting"||k=="coffee_table") {
        const double tw=k=="coffee_table"?w:w-.15,td=k=="coffee_table"?d:d-.8;
        table(tw,td,k=="coffee_table"?.4:.75);
        if(k!="coffee_table")for(double x:{-tw*.28,tw*.28})for(double side:{-1.,1.})chair(x,side*(d/2-.22),side<0);
        box(paper,0,0,k=="coffee_table"?.45:.79,.28,.21,.022);
    } else if(k=="blackboard") {
        box(wood,0,0,1.35,w,d,1.1);box(dark,0,-d/2-.008,1.35,w-.1,.015,1.);
        box(steel,0,-d/2-.045,.84,w,.1,.035);
    } else if(k=="reception"||k=="workbench") {
        box(wood,0,0,h/2,w,d,h-.1);box(steel,0,0,h,w+.06,d+.04,.06);
        if(k=="reception")box(dark,w*.2,0,h+.18,.38,.07,.3);
        else for(double x:{-.3,0.,.3})box(steel,x,-d*.12,h+.09,.16,.1,.12);
    } else if(k=="wardrobe") {
        box(wood,0,0,h/2,w,d,h);
        for(double x:{-w*.25,w*.25}) {
            box(paint,x,-d/2-.01,h/2,w/2-.025,.02,h-.04);
            box(steel,x+(x<0?.16:-.16),-d/2-.035,h*.49,.02,.035,.23);
        }
    } else {
        // Open bookshelves and storage, individual books, drawer handles.
        box(wood,-w/2+.03,0,h/2,.06,d,h);box(wood,w/2-.03,0,h/2,.06,d,h);
        box(wood,0,d/2-.025,h/2,w,.05,h);
        for(double y=.12;y<h;y+=.38) {
            box(wood,0,0,y,w,d,.04);
            if(k=="bookcase")for(double x=-w/2+.1;x<w/2-.07;x+=.09)box(paper,x,0,y+.14,.065,d*.72,.25);
            else if(k=="wardrobe"||k=="filing"||k=="tool_cabinet") {
                box(ceramic,0,-d/2,y+.16,w-.08,.025,.3);box(steel,0,-d/2-.04,y+.17,w*.32,.04,.022);
            } else box(paper,0,0,y+.15,w*.7,d*.75,.25);
        }
    }
    const std::array<double,3> fabrics[]={{.12,.16,.19},{.20,.15,.11},{.085,.13,.095},{.18,.09,.065}};
    auto fabric=fabrics[std::clamp(f.variant,0,3)];
    auto metal=surfaceMaterial("Furniture brushed steel",{.14,.15,.16},.3,std::nullopt,true);metal.metallic=.65;
    return {{"Interior timber",std::move(wood),surfaceMaterial("Furniture timber",{.18,.105,.052},.72,"deck",true)},
        {"Interior upholstery",std::move(soft),surfaceMaterial("Woven upholstery",fabric,.95,std::nullopt,true)},
        {"Interior metal fittings",std::move(steel),metal},
        {"Interior ceramic and plaster",std::move(ceramic),surfaceMaterial("Ceramic",{.32,.325,.30},.35,std::nullopt,true)},
        {"Interior screens and boards",std::move(dark),surfaceMaterial("Dark screen",{.018,.025,.024},.45,std::nullopt,true)},
        {"Interior books and bedding",std::move(paper),surfaceMaterial("Paper and linen",{.38,.36,.30},.85,std::nullopt,true)},
        {"Interior painted joinery",std::move(paint),surfaceMaterial("Warm painted plaster",{.43,.40,.34},.9,std::nullopt,true)},
        {"Interior soft linen",std::move(linen),surfaceMaterial("Natural linen",{.34,.29,.22},.98,std::nullopt,true)},
        {"Interior foliage",std::move(green),surfaceMaterial("Houseplant leaves",{.055,.11,.04},.8,std::nullopt,true)}};
}
} // namespace r1
