#include "interiors.hpp"
#include "palette.hpp"
#include <algorithm>

namespace r1 {
std::vector<MeshPart> buildBuildingFixture(const InteriorFixture& f) {
    if(f.kind=="vehicle")return {}; // Runtime reuses the authored road fleet.
    Mesh wood(UvMode::Planar),soft,steel,ceramic,dark,paper;
    const double w=f.size.x,d=f.size.y,h=f.height;
    auto box=[](Mesh& m,double x,double z,double y,double width,double depth,double height){
        if(width>0&&depth>0&&height>0)m.addBox({x,y,z},{width,height,depth});
    };
    // Revolved ceramics and round metal tubes; flat boxes cannot describe a
    // basin, toilet bowl or chair leg. Twelve facets suffice at room distance.
    auto round=[&](Mesh& m,double x,double z,const std::vector<std::pair<double,double>>& profile,double stretch=1.) {
        for(size_t j=1;j<profile.size();++j)for(int i=0;i<12;++i) {
            double a=i*kPi/6,b=(i+1)*kPi/6;
            auto at=[&](size_t n,double t){return P3{x+profile[n].second*std::cos(t),profile[n].first,z+profile[n].second*std::sin(t)*stretch};};
            m.addQuad(at(j-1,a),at(j-1,b),at(j,b),at(j,a));
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
    if(k=="prayer_mat") {
        box(soft,0,0,.0125,w,d,.025);
    } else if(k=="partition") {
        box(ceramic,0,0,h/2,w,d,h);
        box(wood,0,0,.055,w+.018,d+.018,.11);
    } else if(k=="sofa"||k=="pew") {
        for(double x:{-w/2+.12,w/2-.12})for(double z:{-d/2+.12,d/2-.12})leg(x,z,.17);
        box(wood,0,0,.25,w,d,.18);
        box(soft,0,d/2-.1,.61,w,.2,.5);
        for(double x:{-w/2+.1,w/2-.1})box(soft,x,0,.48,.19,d,.4);
        const int cushions=std::max(1,int(w/.65));
        for(int i=0;i<cushions;++i)box(soft,-w/2+.2+(w-.4)*(i+.5)/cushions,-.07,.43,(w-.4)/cushions-.025,d-.22,.19);
        if(k=="sofa"&&w>1.5)for(double x:{-w*.28,w*.28})box(soft,x,d*.12,.66,.36,.22,.25);
    } else if(k=="bed") {
        box(wood,0,0,.22,w,d,.3);box(soft,0,-.04,.45,w-.06,d-.12,.22);
        box(wood,0,d/2-.06,.58,w,.1,.7);
        box(soft,0,-d*.17,.58,w-.04,d*.62,.06);
        for(double x:{-w*.24,w*.24})box(paper,x,d*.29,.6,w*.42,.4,.1);
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
    return {{"Interior timber",std::move(wood),surfaceMaterial("Furniture timber",{.18,.105,.052},.72,"wood",true)},
        {"Interior upholstery",std::move(soft),surfaceMaterial("Woven upholstery",fabric,.95,std::nullopt,true)},
        {"Interior metal fittings",std::move(steel),metal},
        {"Interior ceramic and plaster",std::move(ceramic),surfaceMaterial("Ceramic",{.32,.325,.30},.35,std::nullopt,true)},
        {"Interior screens and boards",std::move(dark),surfaceMaterial("Dark screen",{.018,.025,.024},.45,std::nullopt,true)},
        {"Interior books and bedding",std::move(paper),surfaceMaterial("Paper and linen",{.38,.36,.30},.85,std::nullopt,true)}};
}
} // namespace r1
