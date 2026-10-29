#include "world/photo_detection.hpp"
#include "core/Camera.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <cmath>
#include <iostream>

using namespace r1::world;
glm::mat4 projection(float fov=60,int width=1000,int height=1000) {
    saida::Camera camera;camera.position={0,0,0};camera.lookAt({0,0,-1});
    camera.setPerspective(glm::radians(fov),float(width)/height,.1f,100.f);
    return camera.projection()*camera.view();
}
PhotoSubject subject(std::string id,glm::vec3 position,float radius=.5f) {
    return {{id,"Name "+id},{glm::vec3(-radius),glm::vec3(radius)},glm::translate(glm::mat4(1),position),[](glm::vec3){return true;}};
}
int main() {
    const auto detect=[](const std::vector<PhotoSubject>& candidates){return detectPhotoAnimals(projection(),1000,1000,candidates);};
    auto cat=subject("cat",{0,0,-10});
    assert(detect({cat}).size()==1);
    assert(detect({subject("behind",{0,0,10}),subject("offscreen",{20,0,-10}),subject("far",{0,0,-110})}).empty());
    auto blocked=cat;blocked.visible=[](glm::vec3){return false;};assert(detect({blocked}).empty());
    auto partial=cat;partial.visible=[](glm::vec3 p){return p.x>.3f;};assert(detect({partial}).size()==1);
    assert(detect({subject("edge",{5.9f,0,-10},.7f)}).size()==1); // centre outside, part of the animal in frame
    assert(detect({subject("tiny",{0,0,-10},.001f)}).empty());
    assert(detect({subject("near",{0,0,-.025f},.005f)}).empty());
    const auto multiple=detect({cat,cat,subject("dog",{2,0,-10},.3f),blocked});
    assert(multiple.size()==2&&multiple[0].species=="cat"&&multiple[1].species=="dog");
    auto side=subject("side",{3,0,-10});assert(detect({side}).size()==1);
    const float zoomed=zoomPhotoFov(60,60,8);
    assert(detectPhotoAnimals(projection(zoomed),1000,1000,{side}).empty());
    assert(detectPhotoAnimals(projection(zoomed),1000,1000,{cat}).size()==1);
    auto wide=subject("wide",{6,0,-10});
    assert(detectPhotoAnimals(projection(60,1600,900),1600,900,{wide}).size()==1);
    assert(detectPhotoAnimals(projection(60,900,1600),900,1600,{wide}).empty());
    assert(detectPhotoAnimals(projection(),0,1000,{cat}).empty());
    float lens=60;
    lens=zoomPhotoFov(60,lens,1);assert(std::abs(photoZoom(60,lens)-1.2f)<.0001f);
    lens=zoomPhotoFov(60,lens,1000);assert(std::abs(photoZoom(60,lens)-8)<.0001f);
    lens=zoomPhotoFov(60,lens,-1000);assert(std::abs(lens-60)<.0001f);
    const auto fractional=zoomPhotoFov(60,60,.5f);assert(fractional<60&&photoZoom(60,fractional)<1.2f);
    Photograph photo;photo.category=PhotoCategory::Event;photo.place="Paris";photo.title="Insolite";
    classifyPhotograph(photo,multiple);assert(photo.category==PhotoCategory::Animal&&photo.animals.size()==2&&photo.title.find("Name cat")==0);
    Photograph empty;empty.category=PhotoCategory::Event;empty.title="Keep me";
    classifyPhotograph(empty,{});assert(empty.category==PhotoCategory::Event&&empty.title=="Keep me");
    std::cout<<"PASS photo recognition: in-frame, clipped, behind, near/far, wall, partial visibility, species deduplication, multi-species, zoom and aspect ratio\n";
}
