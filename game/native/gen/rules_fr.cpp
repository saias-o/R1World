// The French rulebook: the Code de la route and the Instruction
// interministérielle sur la signalisation routière (IISR), as regularities
// of the road graph. Each rule proposes; none places (gen/predict.hpp).
#include "predict.hpp"

#include "palette.hpp"

namespace r1 {

namespace {
using Branch = RoadNet::Branch;

const RoadNet::Way& wayOf(const Context& c, const Branch& b) { return c.net.ways()[size_t(b.way)]; }
bool onRing(const Context& c, int vertex) {
    for (const auto& [w, i] : c.net.vertices()[size_t(vertex)].uses)
        if (c.net.ways()[size_t(w)].roundabout) return true;
    return false;
}
// The same stretch of road, driven the other way.
Branch reversed(const Branch& b) { return {b.way, b.index, -b.step, b.legal}; }

// The limit a French road has without any sign (Code de la route R413-2,
// R413-3): 130 on a motorway, 110 on a dual carriageway, 50 in town, 80 on
// every other road of the open country.
double defaultLimit(const RoadNet::Way& w, bool urban) {
    if (w.highway == "motorway") return 130;
    if (urban) return 50;
    if (w.highway == "trunk" && w.direction != 0) return 110;
    return 80;
}

// What OSM says about where a limit comes from: "sign", "FR:urban", ...
std::string limitSource(const Tags& t) {
    for (const char* key : {"maxspeed:type", "source:maxspeed"}) {
        const std::string v = tagOr(t, key);
        if (!v.empty()) return v;
    }
    return "";
}

bool zone30(const RoadNet::Way& w, bool urban) {
    const Tags& t = w.osm->tags;
    const std::string source = limitSource(t), zone = tagOr(t, "zone:maxspeed");
    if (source == "FR:zone30" || source == "FR:30" || zone == "FR:30") return true;
    // An untyped 30 on a street of the town is, in France, a zone's street.
    const bool street = w.highway == "residential" || w.highway == "living_street" || w.highway == "unclassified";
    return urban && street && source.empty() && speedKmh(tag(t, "maxspeed")) == 30;
}

// The limit a sign must announce on a branch, or 0. A limit OSM says is
// signed is; an implicit one (FR:urban, FR:rural, a zone) never is; an
// untyped one is when it differs from the default -- except a 30, which in
// town is far more often a zone's than a sign's.
int signedLimit(const Context& c, const Branch& b, bool urban) {
    const RoadNet::Way& w = wayOf(c, b);
    const double v = speedKmh(c.net.maxspeed(b));
    if (v <= 0) return 0;
    const std::string source = limitSource(w.osm->tags);
    if (source == "sign") return int(v);
    if (source.rfind("FR:", 0) == 0) return 0;
    // The country's own 80 is never posted untyped: past a town's last house
    // it is what the EB20 already said.
    if (v == defaultLimit(w, urban) || v <= 30 || (v == 80 && w.direction == 0)) return 0;
    return int(v);
}

// The speed traffic arrives at along a branch that leads into a vertex.
double arrivingSpeed(const Context& c, const Branch& arrival, bool urban) {
    const double tagged = speedKmh(c.net.maxspeed(reversed(arrival)));
    return tagged > 0 ? tagged : defaultLimit(wayOf(c, arrival), urban);
}

bool priorityRoad(const RoadNet::Way& w) {
    if (w.highway == "primary" || w.highway == "secondary") return true;
    if (w.highway != "tertiary") return false;
    const std::string ref = tagOr(w.osm->tags, "ref");
    return !ref.empty() && (ref[0] == 'D' || ref[0] == 'N' || ref[0] == 'M');
}

bool hasSign(const std::string& code) { return palette().signs.count(code) > 0; }

void add(std::vector<Prediction>& out, std::optional<Prediction> p, const std::string& what) {
    if (!p) return;
    p->what = what;
    out.push_back(std::move(*p));
}

// ── the rules ───────────────────────────────────────────────────────────────

// Every entry onto a roundabout gives way to the ring (R415-10): the
// inverted triangle stands at the entry, for the traffic arriving.
void roundaboutGiveWay(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        if (!onRing(c, v)) continue;
        for (const Branch& a : c.net.arrivals(v)) {
            const RoadNet::Way& w = wayOf(c, a);
            if (!a.legal || w.roundabout || w.highway == "service") continue;
            add(out, c.signBefore(v, a, 4.0, 2.0), "FR:AB3a");
        }
    }
}

// Leaving a roundabout onto a départementale in the open country, the road
// is a priority road and says so a little after the exit (AB6). Its limit is
// the default 80, which no sign repeats (see the speed rule).
void roundaboutExitPriority(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        if (!onRing(c, v)) continue;
        for (const Branch& b : c.net.branches(v)) {
            const RoadNet::Way& w = wayOf(c, b);
            if (!b.legal || w.roundabout || !priorityRoad(w)) continue;
            if (c.urbanAt(b.way, c.net.vertices()[size_t(v)].xz).first) continue;
            // A road held to 50 or less is a street, not a country road.
            const double limit = speedKmh(c.net.maxspeed(b));
            if (limit > 0 && limit <= 50) continue;
            // Where the sign stands must be the country too: the walk may
            // have reached the town's own avenue.
            auto p = c.signAfter(v, b, 60.0, 25.0);
            if (p && p->urban) continue;
            add(out, p, "FR:AB6");
        }
    }
}

// A limit stops at the next intersection (R413-1 / IISR art. 63): where one
// is signed, it is repeated after every junction and posted where it changes.
// One that equals the default is never posted: a départementale leaving a
// roundabout at 80 has no 80 sign.
void speedLimits(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        const bool junction = c.net.isJunction(v);
        const std::vector<Branch> arrivals = c.net.arrivals(v);
        for (const Branch& b : c.net.branches(v)) {
            const RoadNet::Way& w = wayOf(c, b);
            if (!b.legal || w.roundabout || w.link || w.highway == "motorway" || w.highway == "service") continue;
            const bool urban = c.urbanAt(b.way, c.net.vertices()[size_t(v)].xz).first;
            if (zone30(w, urban)) continue;
            const int limit = signedLimit(c, b, urban);
            if (!limit) continue;
            bool changes = false, anyArrival = false;
            for (const Branch& a : arrivals) {
                if (!a.legal || (a.way == b.way && a.index == b.index && a.step == b.step)) continue;
                anyArrival = true;
                const double before = speedKmh(c.net.maxspeed(reversed(a)));
                changes |= before > 0 && int(before) != limit;
            }
            if (!anyArrival || !(junction || changes)) continue;
            // A stub between two junctions carries no sign of its own.
            if (junction && c.net.reach(v, b, 40.0).second < 40.0) continue;
            const std::string code = "FR:B14[" + std::to_string(limit) + "]";
            if (!hasSign(code)) continue;
            add(out, c.signAfter(v, b, junction ? 20.0 : 10.0, 8.0), code);
        }
    }
}

// Entering a zone 30 from a faster road, the zone's panel (B30) stands at its
// gate. Inside, no 30 is repeated.
void zone30Entries(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (int v = 0; v < int(c.net.vertices().size()); ++v) {
        const std::vector<Branch> arrivals = c.net.arrivals(v);
        for (const Branch& b : c.net.branches(v)) {
            const RoadNet::Way& w = wayOf(c, b);
            if (!b.legal || w.roundabout) continue;
            const bool urban = c.urbanAt(b.way, c.net.vertices()[size_t(v)].xz).first;
            if (!zone30(w, urban)) continue;
            bool gate = false;
            for (const Branch& a : arrivals) {
                if (!a.legal || a.way == b.way) continue;
                const RoadNet::Way& from = wayOf(c, a);
                const bool fromUrban = c.urbanAt(a.way, c.net.vertices()[size_t(v)].xz).first;
                // Faster by what OSM says, or by being a through road: an
                // untagged side street is as likely to be in the zone as not.
                const double tagged = speedKmh(c.net.maxspeed(reversed(a)));
                const bool faster = tagged > 30 || (tagged <= 0 && roadRank(from.highway) >= 2);
                gate |= !zone30(from, fromUrban) && from.highway != "service" && faster;
            }
            if (gate) add(out, c.signAfter(v, b, 6.0, 3.0), "FR:B30");
        }
    }
}

// A hump (dos-d'âne, plateau) is announced by A2b, with the 30 it is built
// for just under it, a little before it -- in each direction it is driven.
// On a road already at 30 or less, nothing is added.
void humps(const Context& c, const Rule&, std::vector<Prediction>& out) {
    for (const OsmNode& f : c.osm.features) {
        const std::string kind = tagOr(f.tags, "traffic_calming");
        if (kind != "hump" && kind != "bump" && kind != "table" && kind != "cushion") continue;
        const int v = c.net.vertexAt({f.lon, f.lat});
        if (v < 0) continue;
        for (const Branch& a : c.net.arrivals(v)) {
            if (!a.legal || wayOf(c, a).highway == "service") continue;
            const bool urban = c.urbanAt(a.way, c.net.vertices()[size_t(v)].xz).first;
            if (arrivingSpeed(c, a, urban) <= 30) continue;
            auto p = c.signBefore(v, a, urban ? 30.0 : 60.0, 12.0);
            if (p) p->evidence = f.id;
            add(out, p, "FR:A2b,FR:B14[30]");
        }
    }
}
// A town's name at its gates (R110-2: the agglomeration is the space between
// its entry and exit signs): EB10 for whoever drives in, EB20, its name
// struck through, for whoever drives out. The name is the commune the town's
// own buildings give as their address; a town whose buildings do not say
// gets no sign rather than a guessed one.
void townGates(const Context& c, const Rule&, std::vector<Prediction>& out) {
    constexpr double kStep = 10.0, kSure = 60.0;
    const auto& ways = c.net.ways();
    const auto& verts = c.net.vertices();
    for (int wi = 0; wi < int(ways.size()); ++wi) {
        const RoadNet::Way& w = ways[size_t(wi)];
        const std::string& h = w.highway;
        if (w.roundabout || w.link || w.bridge || w.tunnel) continue;
        if (h != "primary" && h != "secondary" && h != "tertiary" && h != "unclassified" && h != "residential") continue;
        std::vector<P2> samples;
        for (size_t i = 0; i + 1 < w.vertices.size(); ++i) {
            const P2 a = verts[size_t(w.vertices[i])].xz, b = verts[size_t(w.vertices[i + 1])].xz;
            const int n = std::max(1, int(std::ceil(dist(a, b) / kStep)));
            for (int k = 0; k < n; ++k) samples.push_back({a.x + (b.x - a.x) * k / n, a.y + (b.y - a.y) * k / n});
        }
        samples.push_back(verts[size_t(w.vertices.back())].xz);
        for (size_t k = 0; k + 1 < samples.size(); ++k) {
            const bool here = c.townAt(samples[k]) >= 0, next = c.townAt(samples[k + 1]) >= 0;
            if (here == next) continue;
            const P2 a = samples[k], b = samples[k + 1];
            const double l = dist(a, b);
            if (l < 1e-6) continue;
            P2 d{(b.x - a.x) / l, (b.y - a.y) / l};
            if (here) d = {-d.x, -d.y};  // d points into the town
            const P2 gate{(a.x + b.x) / 2, (a.y + b.y) / 2};
            auto along = [&](double s) { return P2{gate.x + d.x * s, gate.y + d.y * s}; };
            // A real edge: open country behind, town ahead, both for a while.
            const int town = c.townAt(along(kSure));
            if (town < 0 || c.townAt(along(kSure / 2)) != town) continue;
            if (c.townAt(along(-kSure / 2)) >= 0 || c.townAt(along(-kSure)) >= 0) continue;
            // Not a park inside the town: the same town does not resume just
            // behind. Not the edge of the data: it reaches past the gate.
            if (c.townAt(along(-300)) == town || c.townAt(along(-450)) == town) continue;
            if (!c.observed(along(-2 * kSure)) || !c.observed(along(kSure))) continue;
            const std::string name = c.townName(along(kSure), town);
            if (name.empty()) continue;
            // Which way along the way is inward, and may traffic drive it.
            const P2 forward{b.x - a.x, b.y - a.y};
            const bool inwardIsForward = forward.x * d.x + forward.y * d.y > 0;
            const bool canForward = w.direction >= 0, canBackward = w.direction <= 0;
            for (const bool entering : {true, false}) {
                const bool drivesForward = entering == inwardIsForward;
                if (drivesForward ? !canForward : !canBackward) continue;
                const P2 travel = entering ? d : P2{-d.x, -d.y};
                Prediction p = c.signAt(wi, gate, travel);
                p.what = entering ? "FR:EB10" : "FR:EB20";
                p.urban = false;
                p.detail = {{"name", name}};
                out.push_back(std::move(p));
            }
        }
    }
}
}  // namespace

std::vector<Rule> frenchRules() {
    return {
        {"fr.roundabout.give_way", "every roundabout entry gives way to the ring (R415-10): AB3a at the entry",
         0.97, roundaboutGiveWay},
        {"fr.roundabout.exit_priority",
         "a départementale leaving a roundabout in the open country is a priority road: AB6 after the exit",
         0.80, roundaboutExitPriority},
        {"fr.speed.limit",
         "a signed limit ends at the next intersection: repeated after junctions, posted where it changes, "
         "never when it is the default (80 in the country, 50 in town)",
         0.75, speedLimits},
        {"fr.zone30.gate", "a zone 30 entered from a faster road has its B30 panel at the gate", 0.85, zone30Entries},
        {"fr.town.gates", "a town's name at its gates: EB10 driving in, EB20 driving out (R110-2)", 0.85, townGates},
        {"fr.hump.warning", "a hump is announced by A2b with its 30 beneath, before it in each direction",
         0.80, humps},
    };
}

}  // namespace r1
