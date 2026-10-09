#include "ground.hpp"
#include "polygons.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace r1 {

GroundInference::GroundInference(const OsmData& osm, const Bounds& b,
                                const RegionProfile& profile, std::string country,
                                const std::map<int64_t,double>& predictedHeights)
    : country_(std::move(country)), profile_(profile.key) {
    const auto& config = palette().groundInference;
    auto apply = [&](const nlohmann::json& p) {
        halfWindow = p.value("halfWindowM", halfWindow);
        settledCoverage = p.value("settledCoverage", settledCoverage);
        denseCoverage = p.value("denseCoverage", denseCoverage);
        minimumBuildings = p.value("minimumBuildings", minimumBuildings);
        rural_ = p.value("rural", rural_);
        settled_ = p.value("settled", settled_);
        dense_ = p.value("dense", dense_);
    };
    apply(config.value("default", nlohmann::json::object()));
    for (const auto& [group, key] : {std::pair<std::string,std::string>{"profiles",profile_}, {"countries",country_}})
        if (config.contains(group) && config[group].contains(key)) apply(config[group][key]);
    // Named city palettes used to pave even empty land. Sparse surroundings
    // use the climate's vegetation; mapped parks still take priority everywhere.
    if (rural_ == "profile" && profile.ground.name == "Made ground") rural_ = "grass";
    auto valid = [&](const std::string& name) {
        return name == "profile" || std::any_of(palette().groundClasses.begin(),palette().groundClasses.end(),
            [&](const auto& c) { return c.name == name; });
    };
    if (!(halfWindow >= 20 && halfWindow <= 250 && settledCoverage > 0 &&
          denseCoverage >= settledCoverage && denseCoverage <= 1 && minimumBuildings > 0) ||
        !valid(rural_) || !valid(settled_) || !valid(dense_))
        throw std::runtime_error("invalid ground inference policy for " + profile_ + "/" + country_);
    const double dy = (halfWindow + 12) / metresPerDegree;
    const double dx = dy / std::max(.05, std::cos(radians(std::max(std::abs(b.south),std::abs(b.north)))));
    x0 = int(std::floor((b.west-dx)/step)); y0 = int(std::floor((b.south-dy)/step));
    width = int(std::ceil((b.east+dx)/step))-x0;
    height = int(std::ceil((b.north+dy)/step))-y0;
    const size_t stride = size_t(width+1);
    occupancy_.assign(stride*size_t(height+1),0);
    buildings_.assign(occupancy_.size(),0);
    mass_.assign(occupancy_.size(),0);
    std::set<int64_t> seen;
    std::vector<double> crossings;
    for (const auto& building : osm.buildings) {
        if (!seen.insert(building.id).second || !building.closed() || building.points.size()<4) continue;
        const auto kind = tagOr(building.tags,"building");
        if (kind=="roof" || kind=="carport" || kind=="ruins" || kind=="greenhouse") continue;
        const auto& ring = building.points;
        const auto planned=predictedHeights.find(building.id);
        const double finalHeight=planned==predictedHeights.end()?0:planned->second;
        // Height is the completed prediction, including neighbour inference.
        // Logarithmic weighting keeps real skyscrapers influential without one
        // exceptional tower paving its entire surroundings. No geometry is capped.
        const double weight=std::max(1.,std::log2(1+std::max(0.,finalHeight)/6.));
        double low = ring.front().y, high = low;
        for (auto p:ring) { low=std::min(low,p.y); high=std::max(high,p.y); }
        const int first=std::max(0,int(std::ceil(low/step-.5))-y0);
        const int last=std::min(height-1,int(std::floor(high/step-.5))-y0);
        for (int row=first;row<=last;++row) {
            const double lat=(y0+row+.5)*step;
            crossings.clear();
            for (size_t i=1;i<ring.size();++i) {
                const auto a=ring[i-1], c=ring[i];
                if ((a.y>lat)==(c.y>lat)) continue;
                crossings.push_back(a.x+(lat-a.y)*(c.x-a.x)/(c.y-a.y));
            }
            std::sort(crossings.begin(),crossings.end());
            for (size_t i=1;i<crossings.size();i+=2) {
                const int left=std::max(0,int(std::ceil(crossings[i-1]/step-.5))-x0);
                const int right=std::min(width-1,int(std::floor(crossings[i]/step-.5))-x0);
                for (int col=left;col<=right;++col) {
                    const size_t at=size_t(row+1)*stride+size_t(col+1);
                    occupancy_[at]=1; mass_[at]=std::max(mass_[at],weight);
                }
            }
        }
        const auto centre=centroid(ring);
        const int col=int(std::floor(centre.x/step))-x0, row=int(std::floor(centre.y/step))-y0;
        if (col>=0 && col<width && row>=0 && row<height)
            ++buildings_[size_t(row+1)*stride+size_t(col+1)];
    }
    // Union occupancy prevents overlapping cadastral footprints inflating density.
    for (auto* table : {&occupancy_,&buildings_})
        for (int row=1;row<=height;++row) for (int col=1;col<=width;++col) {
            const size_t at=size_t(row)*stride+size_t(col);
            (*table)[at]+=(*table)[at-1]+(*table)[at-stride]-(*table)[at-stride-1];
        }
    for (int row=1;row<=height;++row) for (int col=1;col<=width;++col) {
        const size_t at=size_t(row)*stride+size_t(col);
        mass_[at]+=mass_[at-1]+mass_[at-stride]-mass_[at-stride-1];
    }
}

uint32_t GroundInference::sum(const std::vector<uint32_t>& table, int left, int bottom, int right, int top) const {
    left=std::clamp(left-x0,0,width);right=std::clamp(right-x0,0,width);
    bottom=std::clamp(bottom-y0,0,height);top=std::clamp(top-y0,0,height);
    const size_t stride=size_t(width+1);
    return table[size_t(top)*stride+right]+table[size_t(bottom)*stride+left]
          -table[size_t(top)*stride+left]-table[size_t(bottom)*stride+right];
}

GroundInference::Density GroundInference::density(double lon, double lat) const {
    const double dy=halfWindow/metresPerDegree;
    const double dx=dy/std::max(.05,std::cos(radians(lat)));
    const int left=int(std::floor((lon-dx)/step)),right=int(std::ceil((lon+dx)/step));
    const int bottom=int(std::floor((lat-dy)/step)),top=int(std::ceil((lat+dy)/step));
    const double pixels=double(right-left)*double(top-bottom);
    const int l=std::clamp(left-x0,0,width),r=std::clamp(right-x0,0,width);
    const int b=std::clamp(bottom-y0,0,height),t=std::clamp(top-y0,0,height);
    const size_t stride=size_t(width+1);
    const double mass=mass_[size_t(t)*stride+r]+mass_[size_t(b)*stride+l]
                     -mass_[size_t(t)*stride+l]-mass_[size_t(b)*stride+r];
    return {sum(occupancy_,left,bottom,right,top)/pixels,mass/pixels,sum(buildings_,left,bottom,right,top)};
}

std::string GroundInference::at(double lon, double lat, const std::string* mapped) const {
    if (mapped) return *mapped;
    const auto d=density(lon,lat);
    const std::string& cls=d.heightWeightedCoverage>=denseCoverage ? dense_ :
        d.buildings>=minimumBuildings && d.heightWeightedCoverage>=settledCoverage ? settled_ : rural_;
    return cls=="profile" ? kInferred : "inferred:"+cls;
}

nlohmann::json GroundInference::report() const {
    return {{"source","inferred:building-density+predicted-height+atlas"},{"country",country_},{"profile",profile_},
        {"windowSideM",2*halfWindow},{"samplingDegrees",step},{"settledCoverage",settledCoverage},
        {"denseCoverage",denseCoverage},{"minimumBuildings",minimumBuildings},
        {"rural",rural_},{"settled",settled_},{"dense",dense_},
        {"heightWeight","max(1,log2(1+finalHeightM/6))"},{"heightSource","completed building prediction"}};
}

}  // namespace r1
