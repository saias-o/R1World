// Deterministic, engine-independent interior contract. Recipes supply furniture;
// the footprint, portal, floor and collision contract are shared by all uses.
#pragma once
#include "mesh.hpp"
#include "osm.hpp"
#include "polygons.hpp"
#include <functional>
#include <optional>

namespace r1 {
struct InteriorPlan {
    int64_t id = 0;
    size_t footprint = 0, edge = 0;
    std::string recipe, name, nameSource, entranceSource;
    Ring ring;
    P2 door, along, inward;
    double width = 2.4, floor = 0, ceiling = 3.6, approach = 0;
    // A supermarket mapped inside a mall (Carrefour in Le Fourchêne): its
    // measured position. The floor nearer to it than to the door is its
    // sales floor; the rest stays the gallery.
    std::optional<P2> anchor;
    std::string anchorName;
    bool inAnchor(P2 p) const { return anchor && dist(p, *anchor) < dist(p, door); }
    // Where someone standing at `p` inside is: the anchor store or the room.
    std::string place(P2 p) const { return inAnchor(p) ? anchorName + " \xC2\xB7 " + name : name; }
    P2 point(double u, double v) const;
    P2 local(P2 p) const;
    nlohmann::json json() const;
    static InteriorPlan read(const nlohmann::json& j);
};
struct InteriorFixture { P2 at, size; double height; std::string kind; int variant = 0; };
struct InteriorLayout { std::vector<InteriorFixture> fixtures; };
bool retailUse(const Tags& tags);
std::string retailRecipe(const Tags& tags);
std::string retailName(const Tags& tags);
// Engine-frame points a store may open toward, written by retailBuildings as
// "r1:front" ("x,z;x,z"): every mapped entrance, or one inferred target.
std::vector<P2> retailFronts(const Tags& tags);
std::vector<P2> retailPoints(const std::string& text);
// The portal opens at a target on the outline (a mapped entrance) when there
// is one, choosing among them the door whose central aisle runs deepest into
// the room (up to 12 m): an entrance in a vestibule that faces a wall loses to
// another mapped entrance. With no target on the outline, the nearest door
// with a 6 m aisle wins (the deepest aisle, if none reaches 6 m). `offset` is the door's distance to its target.
bool chooseRetailPortal(InteriorPlan& plan,const std::vector<P2>& targets,
                        const std::function<bool(size_t)>& eligible = {},double* offset = nullptr);
InteriorLayout layoutInterior(const InteriorPlan& plan);
std::vector<MeshPart> buildInteriorShell(const InteriorPlan& plan);
// Furniture is generated once per prototype and instanced by the runtime.
std::string interiorFixtureKey(const InteriorFixture& fixture);
std::vector<MeshPart> buildInteriorFixture(InteriorFixture fixture);
// Door leaves are local to the portal; their scene transforms provide the slide.
std::vector<MeshPart> buildDoorLeaf(double width);
std::vector<MeshPart> buildShopfront(const InteriorPlan& plan);
bool interiorBlocked(const InteriorPlan& plan, const InteriorLayout& layout,
                     P2 point, double opening, double radius = .32);
double slideDoor(double opening, bool near, double dt);
} // namespace r1
