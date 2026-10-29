#pragma once
#include "player_profile.hpp"
#include "graphics/Mesh.hpp"
#include <functional>
#include <glm/glm.hpp>

namespace r1::world {
constexpr float kPhotoMaximumZoom=8.f;
float photoZoom(float baseFov,float fov);
float zoomPhotoFov(float baseFov,float fov,float wheel);

// Inputs are the engine's current rendered mesh bounds, transforms and camera
// projection. Visibility is supplied by the world's physics query, not inferred
// from an animal being somewhere near the player.
struct PhotoSubject {
    PhotoAnimal animal;
    saida::Aabb bounds;
    glm::mat4 world{1};
    std::function<bool(glm::vec3)> visible;
};
std::vector<PhotoAnimal> detectPhotoAnimals(const glm::mat4& clipFromWorld,int width,int height,
    const std::vector<PhotoSubject>& subjects);
void classifyPhotograph(Photograph& photo,std::vector<PhotoAnimal> animals);
} // namespace r1::world
