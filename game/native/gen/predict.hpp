// The predictive model of details: what the maps do not say, but the place
// does.
//
// The world is built from what was surveyed. For the details -- which sign
// stands where, which road passes over which -- the maps are silent, but not
// the place: a roundabout's exits onto a départementale carry the priority
// diamond, a hump has its 30 just before it, a motorway that crosses a lane
// without a shared node passes over it. Those regularities are the highway
// code and the way roads are built; each is written once as a **rule**.
//
// The model is four stages, and the order is the contract:
//
//   1. **Facts.** The observations as a graph (gen/roadnet.hpp) and the few
//      questions every rule asks of them: which jurisdiction, is this in town.
//   2. **Rules.** Each rule reads the facts and *proposes* details, with its
//      confidence and the OSM element it reasoned from. A rule never places
//      anything and never looks at what another rule proposed. Rules are
//      grouped in rulebooks by jurisdiction (gen/rules_fr.cpp); the ones that
//      hold everywhere are in the rulebook "*" (gen/rules_structure.cpp).
//   3. **Arbitration.** What is measured beats what is inferred (PLAN §3 I5):
//      a sign OSM carries silences the rule that would have guessed it. Then
//      proposals of the same detail for the same traffic merge, the tile keeps
//      what stands on it, and a budget bounds the rest.
//   4. **Emission.** Scene nodes on shared models (CLAUDE.md §5), and a
//      manifest saying, rule by rule, what was proposed, kept and silenced.
//
// Everything is deterministic (PLAN §3 I3): no draw, the order of proposals
// is the order of the data, and every tie is broken on an OSM id.
#pragma once

#include "roadnet.hpp"
#include "scatter.hpp"

#include <optional>

namespace r1 {

// Bump when a rule, the arbitration or the placement changes what comes out.
constexpr int kPredictRevision = 2;

// Whose highway code applies here: the country OSM's own boundaries put the
// neighbourhood in (measured, question 7 on), else a coarse outline for the
// countries that have one, else nobody's -- and then only the rules that hold
// everywhere run.
struct Jurisdiction {
    std::string code;        // ISO 3166-1 alpha-2, or empty
    bool rightHand = true;   // which side traffic keeps to, and signs stand on
    std::string basis;       // how the country was known
    bool rulebook = false;   // whether a rulebook is written for it
};
Jurisdiction jurisdictionAt(const OsmData& osm, double lon, double lat);

// One detail a rule proposes.
struct Prediction {
    std::string what;        // an OSM `traffic_sign` code ("FR:AB6"), or a structure ("bridge")
    std::string rule;        // the rule that proposed it
    double confidence = 0;   // the rule's prior that the detail is really there
    P2 xz{};                 // engine metres, where it stands
    P2 travel{};             // the traffic it addresses, as a unit direction
    bool urban = false;      // the mount the sign takes
    int64_t evidence = 0;    // the OSM element the rule reasoned from
    nlohmann::json detail;   // what a structure needs to be built
};

// The facts every rule reads, and the placements every rule shares.
class Context {
public:
    Context(const OsmData& osm, const RoadNet& net, Jurisdiction jurisdiction,
            const std::optional<Bounds>& extent = std::nullopt);

    const OsmData& osm;
    const RoadNet& net;
    const Jurisdiction jurisdiction;

    // In town or in the open country, and on what grounds: the way's own
    // tags first (measured), then an urban land use around the point, then
    // the density of buildings (inferred).
    std::pair<bool, const char*> urbanAt(int way, P2 xz) const;

    // The town a point is in: an agglomeration of at least a dozen and a half
    // buildings, no gap wider than 160 m between them; -1 in the open country.
    int townAt(P2 xz) const;
    // What the buildings of `town` around a point call their commune
    // (`addr:city`, the majority); empty when too few of them say.
    std::string townName(P2 xz, int town) const;
    // Built up enough around a point to be a town's inside (350 m square).
    bool dense(P2 xz) const;
    // Whether the observations reach this far: past the box OSM was asked
    // about, a town seems to end where only the data does. Without the box,
    // a stopgap: 250 m past the buildings the answer carries.
    bool observed(P2 xz) const {
        return seen_[0] <= xz.x && xz.x <= seen_[2] && seen_[1] <= xz.y && xz.y <= seen_[3];
    }
    // A sign beside a way at a point, for the traffic running `travel`.
    Prediction signAt(int way, P2 xz, P2 travel) const;

    // A sign for traffic leaving `vertex` along `leave`, `metres` down the
    // road; nearer when a junction comes first, never under `minimum`.
    std::optional<Prediction> signAfter(int vertex, const RoadNet::Branch& leave, double metres,
                                        double minimum) const;
    // A sign `metres` before `vertex` for traffic arriving along `arrival`.
    std::optional<Prediction> signBefore(int vertex, const RoadNet::Branch& arrival, double metres,
                                         double minimum) const;

private:
    struct Area { std::vector<P2> ring; double x0, z0, x1, z1; };
    std::vector<Area> urbanAreas_;
    std::map<std::pair<long, long>, int> buildings_;  // first points on a 50 m grid
    std::map<std::pair<long, long>, int> townCells_;  // 40 m cells -> their town
    std::vector<std::pair<P2, const std::string*>> addresses_;
    double seen_[4] = {1e300, 1e300, -1e300, -1e300};
    Prediction beside(const RoadNet::Walk& w, bool arriving) const;
};

struct Rule {
    const char* id;
    // The regulation or the regularity it encodes, in one line.
    const char* says;
    double confidence;
    void (*propose)(const Context&, const Rule&, std::vector<Prediction>&);
};

// One rulebook per country. Every country gets one in time: adding one is a
// file of rules (gen/rules_<cc>.cpp, on the model of rules_fr.cpp) and a line
// in `rulebooks()` (gen/predict.cpp). Rules shared by a family of codes (the
// Vienna Convention countries) belong in a helper those files call with the
// country's own numbers -- default limits, sign codes -- never in a rule that
// guesses its country.
struct Rulebook {
    const char* country;
    std::vector<Rule> (*rules)();
    // What a surveyed `highway=give_way` and `highway=stop` node is, in this
    // country's own code: the sign OSM saw without naming it.
    const char* giveWay = "";
    const char* stop = "";
    // And a stop surveyed as every approach's (`stop=all`), when the code has one.
    const char* allWay = "";
    // Surveyed codes that silence guesses of other codes: they regulate the
    // same thing (a stop and a give-way at one entry, a town's limit either way).
    std::vector<std::pair<std::string, std::vector<std::string>>> aliases;
};
const std::vector<Rulebook>& rulebooks();
// The rulebook written for a country, or nullptr.
const Rulebook* rulebookFor(const std::string& country);
std::vector<Rule> frenchRules();
std::vector<Rule> unitedStatesRules();
// The rules that hold everywhere: how roads are built, not what a code says.
std::vector<Rule> structureRules();
// The country's rulebook, then the rules that hold everywhere.
std::vector<const Rule*> rulesFor(const Jurisdiction& j);

struct PredictOutput {
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json stats;
    // Every structure kept, this tile's and its neighbours': a bridge whose
    // ramps reach into this tile is built from both sides (gen/bridges.hpp).
    std::vector<Prediction> structures;
};

// The details of one tile, predicted from its neighbourhood's observations.
// `extent`: the box the OSM answer covers, when known.
PredictOutput predictDetails(const OsmData& osm, const Tile& tile, const Anchor& anchor, const GroundAt& ground,
                             const std::optional<Bounds>& extent = std::nullopt);

// Shared font meshes already shipped for street signs, fitted to a fascia.
// Null when this font cannot spell the observed name; never silently respell it.
std::optional<nlohmann::json> facadeLettering(const std::string& name, double width, double height);

}  // namespace r1
