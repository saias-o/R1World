#pragma once

#include "graphics/Mesh.hpp"
#include <cmath>
#include <array>

namespace r1::traffic_signal {
inline constexpr float kPoleHeight=3.3f;
inline constexpr float kHeadHeight=2.75f;
inline constexpr float kRepeaterHeight=1.4f;
inline constexpr float kRepeaterScale=.55f;
inline constexpr float kHeadOffset=-.13f;
inline constexpr float kLensPitch=.3f;
inline constexpr float kLensRadius=.105f;
inline constexpr float kLensFront=-.135f;
inline constexpr int kRadialSegments=20;
inline constexpr float kPi=3.14159265358979323846f;

class Builder {
public:
    saida::MeshData data;
    void triangle(glm::vec3 a,glm::vec3 b,glm::vec3 c,glm::vec3 normal) {
        if(glm::dot(glm::cross(b-a,c-a),normal)<0)std::swap(b,c);
        for(const auto p:{a,b,c}) {
            saida::Vertex vertex{};vertex.pos=p;vertex.normal=normal;vertex.color=glm::vec3(1);
            vertex.tangent={1,0,0,1};
            data.indices.push_back(uint32_t(data.vertices.size()));data.vertices.push_back(vertex);
        }
    }
    void quad(glm::vec3 a,glm::vec3 b,glm::vec3 c,glm::vec3 d,glm::vec3 normal) {
        triangle(a,b,c,normal);triangle(a,c,d,normal);
    }
    void cylinder(float radius,float bottom,float top) {
        for(int segment=0;segment<kRadialSegments;++segment) {
            const float a=2*kPi*segment/kRadialSegments,b=2*kPi*(segment+1)/kRadialSegments;
            const glm::vec3 lowA{radius*std::cos(a),bottom,radius*std::sin(a)};
            const glm::vec3 lowB{radius*std::cos(b),bottom,radius*std::sin(b)};
            const glm::vec3 highA{lowA.x,top,lowA.z},highB{lowB.x,top,lowB.z};
            quad(lowA,lowB,highB,highA,glm::normalize(glm::vec3(lowA.x+lowB.x,0,lowA.z+lowB.z)));
            triangle({0,bottom,0},lowB,lowA,{0,-1,0});triangle({0,top,0},highA,highB,{0,1,0});
        }
    }
    void box(glm::vec3 centre,glm::vec3 half) {
        for(int axis=0;axis<3;++axis)for(float sign:{-1.f,1.f}) {
            glm::vec3 normal(0);normal[axis]=sign;
            const int x=(axis+1)%3,y=(axis+2)%3;
            auto corner=[&](float u,float v){auto p=centre;p[axis]+=sign*half[axis];p[x]+=u*half[x];p[y]+=v*half[y];return p;};
            quad(corner(-1,-1),corner(1,-1),corner(1,1),corner(-1,1),normal);
        }
    }
};

inline saida::MeshData pole() {
    Builder mesh;
    constexpr float kShaftRadius=.055f,kFootRadius=.09f,kFootHeight=.16f;
    mesh.cylinder(kShaftRadius,0,kPoleHeight);mesh.cylinder(kFootRadius,0,kFootHeight);
    for(float height:{kHeadHeight,kRepeaterHeight})for(float offset:{-.2f,.2f})
        mesh.box({0,height+offset,-.075f},{.065f,.025f,.1f});
    return std::move(mesh.data);
}
inline saida::MeshData housing() {
    Builder mesh;
    constexpr float kHalfWidth=.16f,kHalfHeight=.49f,kCorner=.04f,kHalfDepth=.12f;
    constexpr int kCornerSegments=5,kOutlinePoints=4*(kCornerSegments+1);
    std::array<glm::vec2,kOutlinePoints> outline{};
    for(int corner=0;corner<4;++corner)for(int step=0;step<=kCornerSegments;++step) {
        const float angle=(corner+float(step)/kCornerSegments)*kPi*.5f;
        const glm::vec2 centre{corner==0||corner==3?kHalfWidth-kCorner:-kHalfWidth+kCorner,
                               corner<2?kHalfHeight-kCorner:-kHalfHeight+kCorner};
        outline[corner*(kCornerSegments+1)+step]=centre+glm::vec2(std::cos(angle),std::sin(angle))*kCorner;
    }
    for(int point=0;point<kOutlinePoints;++point) {
        const auto a=outline[point],b=outline[(point+1)%kOutlinePoints];
        const glm::vec3 normal=glm::normalize(glm::vec3(b.y-a.y,a.x-b.x,0));
        mesh.quad({a.x,a.y,-kHalfDepth},{b.x,b.y,-kHalfDepth},{b.x,b.y,kHalfDepth},{a.x,a.y,kHalfDepth},normal);
        mesh.triangle({0,0,-kHalfDepth},{a.x,a.y,-kHalfDepth},{b.x,b.y,-kHalfDepth},{0,0,-1});
        mesh.triangle({0,0,kHalfDepth},{a.x,a.y,kHalfDepth},{b.x,b.y,kHalfDepth},{0,0,1});
    }
    constexpr float kVisorOuter=.126f,kVisorInner=.116f,kVisorFront=-.34f;
    for(int light=0;light<3;++light)for(int segment=0;segment<kRadialSegments;++segment) {
        const float a=(-.15f+1.3f*segment/kRadialSegments)*kPi,b=(-.15f+1.3f*(segment+1)/kRadialSegments)*kPi;
        const float centre=(1-light)*kLensPitch;
        auto p=[&](float angle,float radius,float depth){return glm::vec3(radius*std::cos(angle),centre+radius*std::sin(angle),depth);};
        const glm::vec3 outward{std::cos((a+b)*.5f),std::sin((a+b)*.5f),0};
        mesh.quad(p(a,kVisorOuter,-kHalfDepth),p(b,kVisorOuter,-kHalfDepth),p(b,kVisorOuter,kVisorFront),p(a,kVisorOuter,kVisorFront),outward);
        mesh.quad(p(a,kVisorInner,-kHalfDepth),p(b,kVisorInner,-kHalfDepth),p(b,kVisorInner,kVisorFront),p(a,kVisorInner,kVisorFront),-outward);
        mesh.quad(p(a,kVisorInner,kVisorFront),p(b,kVisorInner,kVisorFront),p(b,kVisorOuter,kVisorFront),p(a,kVisorOuter,kVisorFront),{0,0,-1});
        if(segment==0||segment==kRadialSegments-1) {
            const float end=segment==0?a:b;
            const glm::vec3 side{std::sin(end),-std::cos(end),0};
            mesh.quad(p(end,kVisorInner,-kHalfDepth),p(end,kVisorOuter,-kHalfDepth),p(end,kVisorOuter,kVisorFront),p(end,kVisorInner,kVisorFront),segment==0?side:-side);
        }
    }
    return std::move(mesh.data);
}
inline saida::MeshData lens() {
    Builder mesh;
    constexpr int kRings=4;
    constexpr float kDomeDepth=.016f;
    auto p=[&](int ring,float angle){const float t=float(ring)/kRings;return glm::vec3(kLensRadius*t*std::cos(angle),kLensRadius*t*std::sin(angle),kLensFront-kDomeDepth*(1-t*t));};
    for(int ring=0;ring<kRings;++ring)for(int segment=0;segment<kRadialSegments;++segment) {
        const float a=2*kPi*segment/kRadialSegments,b=2*kPi*(segment+1)/kRadialSegments;
        const auto first=p(ring,a),second=p(ring+1,a),third=p(ring+1,b),fourth=p(ring,b);
        const auto normal=glm::normalize(glm::cross(first-second,third-second));
        if(ring==0)mesh.triangle(first,second,third,normal);
        else mesh.quad(first,second,third,fourth,normal);
    }
    return std::move(mesh.data);
}
} // namespace r1::traffic_signal
