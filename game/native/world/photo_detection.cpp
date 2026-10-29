#include "photo_detection.hpp"
#include "render/FrustumCulling.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace r1::world {
float photoZoom(float base,float fov) {
    return std::tan(glm::radians(base)*.5f)/std::tan(glm::radians(fov)*.5f);
}
float zoomPhotoFov(float base,float fov,float wheel) {
    if(!std::isfinite(wheel)||!std::isfinite(base)||!std::isfinite(fov)||base<1||base>=179||fov<=0)return fov;
    const float zoom=std::clamp(photoZoom(base,fov)*std::pow(1.2f,std::clamp(wheel,-100.f,100.f)),1.f,kPhotoMaximumZoom);
    return glm::degrees(2.f*std::atan(std::tan(glm::radians(base)*.5f)/zoom));
}

std::vector<PhotoAnimal> detectPhotoAnimals(const glm::mat4& clipFromWorld,int width,int height,
                                         const std::vector<PhotoSubject>& subjects) {
    struct Detection {PhotoAnimal animal;float area=0;};
    std::map<std::string,Detection> found;
    if(width<=0||height<=0)return {};
    for(const auto& subject:subjects) {
        if(subject.animal.species.empty()||!subject.visible)continue;
        const auto clipFromLocal=clipFromWorld*subject.world;
        if(!saida::aabbIntersectsClipVolume(subject.bounds,clipFromLocal))continue;
        glm::vec2 low(1),high(-1);
        for(int k=0;k<8;++k) {
            const auto p=glm::vec3(k&1?subject.bounds.max.x:subject.bounds.min.x,
                k&2?subject.bounds.max.y:subject.bounds.min.y,k&4?subject.bounds.max.z:subject.bounds.min.z);
            const auto clip=clipFromLocal*glm::vec4(p,1);
            if(clip.w<=0)continue;
            const auto screen=glm::clamp(glm::vec2(clip)/clip.w,glm::vec2(-1),glm::vec2(1));
            low=glm::min(low,screen);high=glm::max(high,screen);
        }
        const glm::vec2 pixels=(high-low)*glm::vec2(width,height)*.5f;
        // Sub-pixel specks are not a recognizable animal in the saved image.
        if(std::max(pixels.x,pixels.y)<2.f||pixels.x*pixels.y<4.f)continue;
        bool visible=false;
        // Sample across the bounds, so a partly occluded or cropped animal is
        // eligible even when its centre is hidden or outside the photograph.
        for(float x:{.5f,.05f,.95f})for(float y:{.5f,.05f,.95f})for(float z:{.5f,.05f,.95f}) {
            if(visible)continue;
            const auto local=glm::mix(subject.bounds.min,subject.bounds.max,glm::vec3(x,y,z));
            const auto clip=clipFromLocal*glm::vec4(local,1);
            if(clip.w<=0||clip.z<0||clip.z>clip.w||std::abs(clip.x)>clip.w||std::abs(clip.y)>clip.w)continue;
            visible=subject.visible(glm::vec3(subject.world*glm::vec4(local,1)));
        }
        if(!visible)continue;
        const float area=pixels.x*pixels.y;
        auto& detection=found[subject.animal.species];
        if(area>detection.area)detection={subject.animal,area};
    }
    std::vector<Detection> ranked;
    for(auto& [id,detection]:found)ranked.push_back(std::move(detection));
    std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){
        return a.area!=b.area?a.area>b.area:a.animal.species<b.animal.species;
    });
    std::vector<PhotoAnimal> animals;
    for(auto& detection:ranked)animals.push_back(std::move(detection.animal));
    return animals;
}
void classifyPhotograph(Photograph& photo,std::vector<PhotoAnimal> animals) {
    photo.animals=std::move(animals);
    if(photo.animals.empty())return;
    photo.category=PhotoCategory::Animal;
    photo.title=photo.animals.front().name;
    if(photo.animals.size()>1)photo.title+=" + "+std::to_string(photo.animals.size()-1)+" autre(s) espèce(s)";
    if(!photo.place.empty())photo.title+=" · "+photo.place;
    // Location metadata stays complete even when a very long place name does
    // not fit in the title; avoid cutting a UTF-8 codepoint.
    if(photo.title.size()>512)photo.title=photo.animals.front().name;
}
} // namespace r1::world
