#include "session.hpp"

namespace r1::world {
void World::Impl::addBirdPerches(Loaded& tile,const saida::Node& tree) {
    const saida::Mesh* cacheKey=nullptr;
    std::function<void(const saida::Node&)> findKey=[&](const saida::Node& n) {
        if(cacheKey||n.name()=="Mid"||n.name()=="Far")return;
        if(n.mesh()&&n.material()&&n.material()->desc().alphaCutoff==0.f&&n.material()->desc().foliage.x==0.f)cacheKey=n.mesh();
        for(const auto& c:n.children())findKey(*c);
    };
    findKey(tree);if(!cacheKey)return;
    auto cached=birdBranchCache.find(cacheKey);
    if(cached==birdBranchCache.end()) {
        std::vector<glm::vec3> candidates;
        const glm::vec3 trunk(0);
        std::function<void(const saida::Node&,glm::mat4)> visit=[&](const saida::Node& n,glm::mat4 parent) {
            if(n.name()=="Mid"||n.name()=="Far")return;
            const auto matrix=&n==&tree?parent:parent*n.localMatrix();
            if(n.mesh()&&n.material()&&n.material()->desc().alphaCutoff==0.f&&n.material()->desc().foliage.x==0.f) {
                const auto& vertices=n.mesh()->collisionVertices();const auto& indices=n.mesh()->collisionIndices();
                for(size_t i=0;i+2<indices.size();i+=3) {
                    if(indices[i]>=vertices.size()||indices[i+1]>=vertices.size()||indices[i+2]>=vertices.size())continue;
                    const auto a=glm::vec3(matrix*glm::vec4(vertices[indices[i]],1));
                    const auto b=glm::vec3(matrix*glm::vec4(vertices[indices[i+1]],1));
                    const auto c=glm::vec3(matrix*glm::vec4(vertices[indices[i+2]],1));
                    if(auto p=r1::birdBranchPoint(a,b,c,trunk,.03f,.03f))candidates.push_back(*p);
                }
            }
            for(const auto& child:n.children())visit(*child,matrix);
        };
        visit(tree,glm::mat4(1));
        std::sort(candidates.begin(),candidates.end(),[](auto a,auto b){return a.y>b.y;});
        std::vector<glm::vec3> selected;
        // Retain low as well as high branches: songbirds and heavy ground birds
        // cannot use a tall raptor's crown. Shared asset geometry is scanned once.
        for(size_t i=0;i<8&&!candidates.empty();++i) {
            auto p=candidates[(candidates.size()-1)*i/7];
            if(std::any_of(selected.begin(),selected.end(),[&](auto q){return glm::distance(p,q)<.1f;}))continue;
            selected.push_back(p);
        }
        cached=birdBranchCache.emplace(cacheKey,std::move(selected)).first;
    }
    const auto matrix=tree.localMatrix();
    for(auto local:cached->second) {
        const auto p=glm::vec3(matrix*glm::vec4(local,1));
        const auto root=glm::vec3(matrix[3]);
        if(p.y-root.y<.8f||std::hypot(p.x-root.x,p.z-root.z)<.5f)continue;
        tile.birdPerches.push_back({&tree,{{p.x,p.z},p.y,uint64_t(tile.birdPerches.size()+1)}});
    }
    tile.birdPerchesDirty=true;
}

void World::Impl::refreshBirdPerches(Loaded& tile) {
    if(!tile.birdPerchesDirty&&tile.birdTreesEnabled==debugTrees)return;
    tile.birdPerchesDirty=false;tile.birdTreesEnabled=debugTrees;
    std::vector<saida::wildlife::Perch> perches;
    if(debugTrees)for(const auto& p:tile.birdPerches)if(p.tree&&p.tree->enabled())perches.push_back(p.perch);
    tile.herds.setPerches(std::move(perches));
}

bool World::Impl::birdFlightClear(const Loaded& tile,saida::wildlife::Vec2 from,float fy,saida::wildlife::Vec2 to,float ty) const {
    const double length=saida::wildlife::distance(from,to);
    const int samples=std::max(1,int(std::ceil(length/1.5)));
    for(int i=1;i<=samples;++i) {
        const double u=double(i)/samples,x=from.x+(to.x-from.x)*u,z=from.y+(to.y-from.y)*u,y=fy+(ty-fy)*u;
        for(const auto& f:tile.footprints) {
            if(y>f.top+.7||x<f.low.x-.4||x>f.high.x+.4||z<f.low.y-.4||z>f.high.y+.4)continue;
            bool inside=false;
            for(size_t a=0,b=f.points.size()-1;a<f.points.size();b=a++) {
                const auto& p=f.points[a];const auto& q=f.points[b];
                if((p.y>z)!=(q.y>z)&&x<(q.x-p.x)*(z-p.y)/(q.y-p.y)+p.x)inside=!inside;
            }
            if(inside)return false;
        }
        // Real trunks, unlike leaf volume, cannot be crossed in flight.
        if(debugTrees)for(const auto& p:tile.vegetation)if(p.tree&&p.node) {
            const auto at=p.node->transform().position;
            if(y>at.y+.15&&y<at.y+4.f&&std::hypot(x-at.x,z-at.z)<.45)return false;
        }
    }
    return true;
}

} // namespace r1::world
