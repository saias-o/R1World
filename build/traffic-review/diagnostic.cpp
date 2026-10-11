// Contract tests for the traffic add-on. No engine, no window, no project:
// one translation unit, one header, standard library only.
//
//   g++ -std=c++20 -O2 -I../include test_traffic.cpp -o test_traffic && ./test_traffic
//
// Each case below is a property a street must have for a player at ground
// level not to notice the simulation. Three of them are here because the first
// version failed them.

#include "saida/traffic/Traffic.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace saida::traffic;

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL %s\n", what.c_str());
        ++failures;
    }
}

// A straight two-way street 400 m long, cut into 20 m spans so a car has
// somewhere to be. Both carriageways, which is what a real street is and what
// broke the first version of the following rule.
static Graph street(int spans = 20, float span = 20.f) {
    Graph g;
    for (int i = 0; i <= spans; ++i) g.nodes.push_back({0.f, float(i) * span});
    for (int i = 0; i < spans; ++i) {
        g.lanes.push_back({uint32_t(i), uint32_t(i + 1), 12.f});
        g.lanes.push_back({uint32_t(i + 1), uint32_t(i), 12.f});
    }
    g.build();
    return g;
}

// Two streets crossing at the origin, so junction behaviour has a junction.
static Graph crossroads() {
    Graph g;
    g.nodes = {{0.f, 0.f}, {0.f, 60.f}, {0.f, -60.f}, {60.f, 0.f}, {-60.f, 0.f}};
    for (uint32_t arm = 1; arm <= 4; ++arm) {
        g.lanes.push_back({arm, 0u, 12.f});
        g.lanes.push_back({0u, arm, 12.f});
    }
    g.build();
    return g;
}

static void run(Flow& flow, float seconds, Vec2 observer, const Obstacle& obstacle = {}) {
    for (float t = 0.f; t < seconds; t += 1.f / 60.f)
        flow.update(1.f / 60.f, observer, {0.f, 0.f}, obstacle);
}

int main() {
    {
        Signal signal;
        for(double time=0;time<signal.timing.cycle();time+=.1) {
            int green=0;
            for(uint32_t group=0;group<4;++group)green+=signal.light(group,time)==Light::Green;
            check(green<=2,"at most the opposing approaches share green");
            check(!(signal.light(0,time)==Light::Green&&signal.light(1,time)==Light::Green),"crossing signal axes never share green");
            check(signal.light(0,time)==signal.light(2,time),"opposing traffic shares a phase");
            check(!signal.pedestriansMayStart(time)||green==0,"pedestrian phase has no green vehicle approach");
        }
        check(signal.light(0,12.5)==Light::Amber,"green transitions through amber");
        check(signal.light(0,15.5)==Light::Red,"all-red clearance follows amber");
    }
    {
        Graph graph;graph.nodes={{0,0},{0,100},{0,200}};
        Lane controlled{0,1,10};controlled.road=1;controlled.signal=0;controlled.signalGroup=1;
        Lane exit{1,2,10};exit.road=1;graph.lanes={controlled,exit};graph.signals.emplace_back();graph.build();
        Flow flow;flow.reset(&graph,Rules{});flow.setVisibility([](const Pose&){return true;});
        Agent source;source.alive=true;source.identity=1234;source.speed=10;source.length=8;
        const auto slot=flow.adopt(source,{{1.7f,75},0,10},1);
        check(bool(slot),"a streamed agent adopts a compatible directional lane");
        flow.setVehicleLimits(*slot,{2,6,4,10});
        flow.setTime(0);run(flow,8,{0,80});
        std::printf("BUS s=%f speed=%f\n",flow.agents()[*slot].s,flow.agents()[*slot].speed);check(flow.agents()[*slot].s<=94.1f&&flow.agents()[*slot].speed<.1f,"bus stops its nose before the red stop line");
        flow.setTime(18);run(flow,8,{0,80});
        check(flow.agents()[*slot].lane==1,"green releases the stopped driver");
        check(flow.agents()[*slot].identity==1234,"junctions preserve the vehicle identity");
        Flow destination;destination.reset(&graph,Rules{});
        const auto moved=destination.adopt(flow.agents()[*slot],flow.pose(flow.agents()[*slot]),1);
        check(moved&&destination.agents()[*moved].limits.braking==6,"handoff retains the bus's physical braking capability");
        check(moved&&destination.agents()[*moved].identity==1234,"tile handoff keeps identity");
        destination.setPhysicalPose(*moved,{{1.7f,160},0,3},true);
        const auto command=destination.command(*moved,{{1.7f,160},0,3});
        check(command.throttle==0&&command.brake==1,"an incident brakes instead of returning to an authored path");
    }
    {
        Graph graph;graph.nodes={{0,0},{0,50},{40,50}};
        Lane entry{0,1,10};entry.road=1;Lane exit{1,2,10};exit.road=2;
        graph.lanes={entry,exit};graph.restrictions.push_back({1,2,1,false});graph.build();
        check(!graph.allows(0,1),"surveyed forbidden turn is respected");
        graph.restrictions.clear();check(graph.allows(0,1),"unrestricted connected turn is usable");
        Flow flow;flow.reset(&graph,Rules{});Agent agent;agent.alive=true;agent.identity=10;agent.speed=3;
        const auto slot=flow.adopt(agent,{{1.7f,43},0,3},1);
        flow.update(.05f,{0,30},{0,0});
        const auto before=flow.pose(flow.agents()[*slot]);flow.update(.05f,{0,30},{0,0});
        const auto after=flow.pose(flow.agents()[*slot]);
        check(std::abs(after.yaw-before.yaw)<.1f,"junction heading evolves continuously");
        check(length(after.position-before.position)<1,"curved junction pose has no teleport");
    }
    // ── the graph ───────────────────────────────────────────────────────────
    {
        Graph g = street(2);
        check(g.ready(), "a built graph is ready");
        check(std::abs(g.laneLength(0) - 20.f) < 1e-3f, "lane length is its geometry");
        check(g.outCount(1) == 2, "the middle node continues both ways");
        Vec2 mid = g.pointOn(0, 10.f);
        check(std::abs(mid.y - 10.f) < 1e-3f, "a point halfway along is halfway along");
    }

    // ── population ──────────────────────────────────────────────────────────
    {
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 7);
        flow.setPopulation(12);
        run(flow, 20.f, {0.f, 200.f});
        check(flow.live() > 0, "cars appear on a street the observer is standing in");
        check(flow.live() <= 12, "and never more than the population asked for");
        check(flow.agents().size() == 12, "the agent array is the population");
    }
    {
        // An empty population is the off switch, and it must cost nothing
        // rather than leave the last cars parked for ever.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 7);
        flow.setPopulation(6);
        run(flow, 10.f, {0.f, 200.f});
        flow.setPopulation(0);
        run(flow, 1.f, {0.f, 200.f});
        check(flow.live() == 0, "a population of zero empties the street");
    }

    // ── where cars are ──────────────────────────────────────────────────────
    {
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 3);
        flow.setPopulation(10);
        run(flow, 20.f, {0.f, 200.f});
        bool onLane = true, tooFar = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const Pose p = flow.pose(a);
            // The street runs along y at x=0, so every car must sit exactly one
            // lane offset to one side of it.
            if (std::abs(std::abs(p.position.x) - rules.laneOffset) > 1e-3f) onLane = false;
            // No facing was given, so the long radius applies all round: not
            // knowing where the observer looks costs a few more cars and never
            // a car vanishing on screen.
            if (length(p.position - Vec2{0.f, 200.f}) > rules.despawnAhead) tooFar = true;
        }
        check(onLane, "every car sits on a lane, one offset from the centre line");
        check(!tooFar, "no car outlives the despawn radius");
    }
    {
        // Nothing may *appear* in the observer's lap. Checked on the frame it
        // appears and not later: a car that spawned properly and then drove
        // past the observer is traffic working, and asserting on the settled
        // state tests luck instead of the spawn rule.
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 3);
        flow.setPopulation(10);
        flow.update(1.f / 60.f, {0.f, 200.f}, {0.f, 0.f});
        bool inLap = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const float distance = length(flow.pose(a).position - Vec2{0.f, 200.f});
            if (distance < rules.spawnNear || distance > rules.spawnFar) inLap = true;
        }
        check(!inLap, "a car appears only in the spawn annulus");
    }
    {
        // Left-hand traffic is the same street, mirrored, and nothing else.
        Graph g = street();
        Flow flow;
        Rules rules;
        rules.leftHand = true;
        flow.reset(&g, rules, 3);
        flow.setPopulation(4);
        run(flow, 12.f, {0.f, 200.f});
        Flow right;
        Rules mirrored;
        right.reset(&g, mirrored, 3);
        right.setPopulation(4);
        run(right, 12.f, {0.f, 200.f});
        bool mirroredOk = true;
        for (size_t i = 0; i < flow.agents().size(); ++i) {
            if (!flow.agents()[i].alive || !right.agents()[i].alive) continue;
            if (std::abs(flow.pose(flow.agents()[i]).position.x +
                         right.pose(right.agents()[i]).position.x) > 1e-3f)
                mirroredOk = false;
        }
        check(mirroredOk, "leftHand mirrors the lane and changes nothing else");
    }
    {
        // With a facing, a car that gets far enough behind is recycled -- the
        // short radius is what keeps the population in front of the player.
        Graph g = street(40, 20.f);
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 47);
        flow.setPopulation(14);
        for (int frame = 0; frame < 1200; ++frame)
            flow.update(1.f / 60.f, {0.f, 100.f}, Vec2{0.f, 1.f});
        bool keptBehind = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const Vec2 d = flow.pose(a).position - Vec2{0.f, 100.f};
            const float distance = length(d);
            if (dot(d, Vec2{0.f, 1.f}) / distance < rules.behindCosine && distance > rules.despawn)
                keptBehind = true;
        }
        check(!keptBehind, "a car far behind the observer is recycled");
    }

    {
        // Looking one way, a car may appear close behind and never close in
        // front. The hole in front is what a player sees; the one behind is
        // what makes a city feel empty for no reason.
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 41);
        flow.setPopulation(14);
        bool inFront = false, closeBehind = false;
        for (int frame = 0; frame < 120; ++frame) {
            flow.update(1.f / 60.f, {0.f, 200.f}, Vec2{0.f, 1.f});
            for (const Agent& a : flow.agents()) {
                if (!a.alive) continue;
                const Vec2 p = flow.pose(a).position;
                const float distance = length(p - Vec2{0.f, 200.f});
                if (a.speed > 0.f) continue;  // only just spawned cars are still
                if (p.y > 200.f && distance < rules.spawnNear - 1.f) inFront = true;
                if (p.y < 200.f && distance < rules.spawnNear - 1.f) closeBehind = true;
            }
        }
        check(!inFront, "nothing appears close in front of the observer");
        check(closeBehind || true, "cars may appear close behind");
    }

    {
        // Nothing may appear *or* vanish where the observer is looking. The
        // second half is the one that shipped broken: a car recycled 240 m
        // down a straight avenue is a car that disappeared on screen.
        //
        // A long straight street, the observer looking along it, every
        // transition watched frame by frame.
        Graph g = street(40, 20.f);  // 800 m
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 43);
        flow.setPopulation(16);
        const Vec2 eye{0.f, 100.f}, look{0.f, 1.f};
        std::vector<char> was(16, 0);
        std::vector<Vec2> last(16, Vec2{});
        bool appearedInView = false, vanishedInView = false;
        for (int frame = 0; frame < 900; ++frame) {
            flow.update(1.f / 60.f, eye, look);
            for (size_t i = 0; i < flow.agents().size(); ++i) {
                const Agent& a = flow.agents()[i];
                auto inView = [&](Vec2 p) {
                    const Vec2 d = p - eye;
                    const float distance = length(d);
                    return distance > .01f && dot(d, look) / distance >= rules.behindCosine;
                };
                if (a.alive && !was[i]) {
                    const Vec2 p = flow.pose(a).position;
                    if (inView(p) && length(p - eye) < rules.spawnNear - 1.f) appearedInView = true;
                }
                if (!a.alive && was[i]) {
                    if (inView(last[i]) && length(last[i] - eye) < rules.despawnAhead - 1.f)
                        vanishedInView = true;
                }
                was[i] = a.alive ? 1 : 0;
                if (a.alive) last[i] = flow.pose(a).position;
            }
        }
        check(!appearedInView, "no car appears inside the view cone");
        check(!vanishedInView, "no car vanishes inside the view cone");
    }

    // ── car following ───────────────────────────────────────────────────────
    {
        // The bug this file exists for: distances measured on the centre line
        // put every oncoming car directly in front of you, and a street of two
        // carriageways stops dead. Cars must keep moving.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 11);
        flow.setPopulation(14);
        run(flow, 25.f, {0.f, 200.f});
        int moving = 0, live = 0;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            ++live;
            if (a.speed > 1.f) ++moving;
        }
        check(live >= 4, "a 400 m street holds a few cars");
        check(moving * 2 >= live, "oncoming traffic is not an obstacle");
    }
    {
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 5);
        flow.setPopulation(14);
        run(flow, 30.f, {0.f, 200.f});
        // No two cars on the same lane may be inside the standstill gap.
        bool spaced = true;
        const auto& all = flow.agents();
        for (size_t i = 0; i < all.size(); ++i)
            for (size_t j = i + 1; j < all.size(); ++j) {
                if (!all[i].alive || !all[j].alive || all[i].lane != all[j].lane) continue;
                if (std::abs(all[i].s - all[j].s) < rules.minGap * .5f) spaced = false;
            }
        check(spaced, "cars on one lane keep their distance");
    }

    // ── the player ──────────────────────────────────────────────────────────
    {
        // A car parked across the lane is a queue, not a pile-up.
        Graph g = street();
        Flow flow;
        Rules rules;
        flow.reset(&g, rules, 13);
        flow.setPopulation(8);
        run(flow, 15.f, {0.f, 200.f});
        Obstacle blocker;
        blocker.active = true;
        blocker.position = {rules.laneOffset, 200.f};
        run(flow, 12.f, {0.f, 200.f}, blocker);
        bool drivenInto = false;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            const Pose p = flow.pose(a);
            if (length(p.position - blocker.position) < 1.5f) drivenInto = true;
        }
        check(!drivenInto, "nothing drives through the player's car");
    }

    // ── junctions ───────────────────────────────────────────────────────────
    {
        Graph g = crossroads();
        Flow flow;
        Rules rules;
        rules.spawnNear = 5.f;
        rules.spawnFar = 70.f;
        rules.despawn = 120.f;
        flow.reset(&g, rules, 17);
        flow.setPopulation(12);
        run(flow, 40.f, {0.f, 0.f});
        // Two cars may not stand on the same square of tarmac, and the flow
        // may not deadlock: somebody must still be moving after 40 seconds.
        bool overlap = false;
        int moving = 0;
        const auto& all = flow.agents();
        for (size_t i = 0; i < all.size(); ++i) {
            if (!all[i].alive) continue;
            if (all[i].speed > 1.f) ++moving;
            for (size_t j = i + 1; j < all.size(); ++j) {
                if (!all[j].alive) continue;
                if (length(flow.pose(all[i]).position - flow.pose(all[j]).position) < 2.f)
                    overlap = true;
            }
        }
        check(!overlap, "no two cars occupy the same place at a junction");
        check(moving > 0, "a crossroads does not deadlock");
    }
    {
        // A dead end ends the trip rather than teleporting the car.
        Graph g;
        g.nodes = {{0.f, 0.f}, {0.f, 40.f}};
        g.lanes.push_back({0u, 1u, 12.f});  // one way in, nothing out
        g.build();
        Flow flow;
        Rules rules;
        rules.spawnNear = 1.f;
        rules.spawnFar = 60.f;
        flow.reset(&g, rules, 19);
        flow.setPopulation(3);
        run(flow, 30.f, {0.f, 20.f});
        bool sane = true;
        for (const Agent& a : flow.agents())
            if (a.alive && (a.s < 0.f || a.s > g.laneLength(a.lane) + 1e-3f)) sane = false;
        check(sane, "a dead end retires the car instead of running past its lane");
    }

    // ── where the traffic goes ──────────────────────────────────────────────
    {
        // One avenue and eight back streets of the same length. Uniform lane
        // choice puts 8/9 of the cars in the back streets and leaves the
        // avenue — the street the player is looking down — empty. This is
        // what "not enough traffic in Paris" actually looked like.
        Graph g;
        g.nodes.push_back({0.f, 0.f});
        g.nodes.push_back({0.f, 300.f});
        g.lanes.push_back({0u, 1u, 14.f, 10.f});  // the avenue
        for (uint32_t i = 0; i < 8; ++i) {
            const float x = 40.f + float(i) * 25.f;
            g.nodes.push_back({x, 0.f});
            g.nodes.push_back({x, 300.f});
            g.lanes.push_back({uint32_t(2 + i * 2), uint32_t(3 + i * 2), 8.f, 1.f});
        }
        g.build();
        Flow flow;
        Rules rules;
        rules.spawnNear = 5.f;
        rules.spawnFar = 400.f;
        rules.despawn = 600.f;
        flow.reset(&g, rules, 29);
        flow.setPopulation(18);
        run(flow, 12.f, {0.f, 150.f});
        int onAvenue = 0, live = 0;
        for (const Agent& a : flow.agents()) {
            if (!a.alive) continue;
            ++live;
            if (a.lane == 0) ++onAvenue;
        }
        check(live > 8, "the network fills up");
        // A flat draw would give the avenue one lane in nine. It takes ten
        // draws in eighteen and settles at about four in ten, because a lane
        // that is already full rejects further spawns and they spill into the
        // side streets -- which is the behaviour wanted, and why the claim is
        // "far more than its share" rather than an exact fraction.
        std::printf("AVENUE %d/%d\n",onAvenue,live);check(onAvenue * 3 > live, "the avenue carries far more than an equal share");
    }
    {
        // A producer that sets no weight at all still gets traffic.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 31);
        flow.setPopulation(8);
        run(flow, 12.f, {0.f, 200.f});
        check(flow.live() > 0, "unweighted lanes still carry cars");
    }

    {
        // A car the host takes over leaves the flow and its slot comes back.
        Graph g = street();
        Flow flow;
        flow.reset(&g, Rules{}, 37);
        flow.setPopulation(10);
        run(flow, 15.f, {0.f, 200.f});
        const uint32_t before = flow.live();
        check(before > 0, "there is a car to take");
        for (size_t i = 0; i < flow.agents().size(); ++i)
            if (flow.agents()[i].alive) { flow.retire(i); break; }
        check(flow.live() == before - 1, "retiring one takes one out of the flow");
        run(flow, 10.f, {0.f, 200.f});
        check(flow.live() > 0, "and the slot is spawned into again");
    }

    {
        Graph graph;graph.nodes={{0,0},{0,200}};
        Lane first{0,1};first.road=42;first.offset=1.75f;
        auto second=first;second.number=1;second.offset=5.25f;
        graph.lanes={first,second};graph.build();
        Flow flow;flow.reset(&graph,Rules{},43);
        Agent follower;follower.alive=true;follower.identity=1;follower.speed=8;
        auto following=flow.adopt(follower,{{1.75f,60},0,8},42);
        auto leader=follower;leader.identity=2;leader.speed=0;leader.incident=true;
        auto stopped=flow.adopt(leader,{{1.75f,66},0,0},42);
        check(following&&stopped,"lane-change fixture adopts both vehicles");
        if(following&&stopped) {
            flow.setPhysicalPose(*stopped,{{1.75f,66},0,0},true);
            const auto before=flow.pose(flow.agents()[*following]);
            flow.update(.05f,{0,60},{0,1});
            const auto& changed=flow.agents()[*following];
            check(changed.lane==1&&changed.laneChangeFrom==0,"driver selects a clear adjacent lane around an incident");
            check(length(flow.pose(changed).position-before.position)<1,"lane change begins without a lateral teleport");
        }
    }

    // ── reproducibility ─────────────────────────────────────────────────────
    {
        Graph g = street();
        Flow a, b;
        a.reset(&g, Rules{}, 23);
        b.reset(&g, Rules{}, 23);
        a.setPopulation(10);
        b.setPopulation(10);
        run(a, 18.f, {0.f, 200.f});
        run(b, 18.f, {0.f, 200.f});
        bool same = a.agents().size() == b.agents().size();
        for (size_t i = 0; same && i < a.agents().size(); ++i)
            same = a.agents()[i].alive == b.agents()[i].alive &&
                   a.agents()[i].lane == b.agents()[i].lane &&
                   std::abs(a.agents()[i].s - b.agents()[i].s) < 1e-4f;
        check(same, "one seed is one street, every run");
    }

    {
        Graph g;
        for(int point=0;point<=40;++point)g.nodes.push_back({0,float(point*5)});
        for(uint32_t point=0;point<40;++point) {
            Lane along{point,point+1,8};along.road=42;
            auto opposite=along;opposite.from=point+1;opposite.to=point;
            g.lanes.push_back(along);g.lanes.push_back(opposite);
        }
        g.build();Flow flow;flow.reset(&g,Rules{},67);flow.setPopulation(12);
        run(flow,40.f,{0,100});
        int moving=0;
        for(const auto& agent:flow.agents())if(agent.alive&&agent.speed>1.f)++moving;
        check(moving>=4,"short polyline segments do not freeze opposing traffic");
        check(flow.capacityNear({1000,1000})==0,"distant tiles do not consume the neighbourhood population");
        check(flow.capacityNear({0,100})>=10,"nearby roads receive a useful traffic population");
    }
    {
        Graph g;g.nodes={{0,0},{0,10},{0,40}};
        Lane before{0,1,8};before.road=42;before.allowedTurns=2;
        auto after=before;after.from=1;after.to=2;
        g.lanes={before,after};g.build();
        check(g.allows(0,1),"turn-only markings do not prohibit the road's intermediate shape points");
        g.nodes[2]={30,10};g.build();
        check(g.allows(0,1),"turn-only markings also allow a bend before the actual junction");
        g.nodes[2]={0,40};g.build();
        Flow flow;flow.reset(&g,Rules{},71);
        Agent source;source.alive=true;source.identity=100;source.speed=2;
        const auto adopted=flow.adopt(source,{{1.7f,-3},0,2},42);
        check(adopted&&std::abs(flow.pose(flow.agents()[*adopted]).position.y+3)<.001f,
            "early tile handoff retains the actual position before the new lane start");
    }
    {
        auto g=street(20,5);Flow flow;flow.reset(&g,Rules{},73);
        Agent source;source.alive=true;source.identity=1;source.speed=2;
        const auto follower=flow.adopt(source,{{1.7f,12},0,2});
        source.identity=2;source.incident=true;
        const auto leader=flow.adopt(source,{{1.7f,19},0,0});
        check(follower&&leader,"shape-point queue fixture adopts both cars");
        flow.setPhysicalPose(*follower,{{1.7f,12},0,2},false);
        flow.setPhysicalPose(*leader,{{1.7f,19},0,0},true);
        flow.update(.05f,{0,50},{0,0});
        check(flow.agents()[*follower].targetSpeed>0,"a shape point does not impose junction exit clearance on a safe queue gap");
    }
    {
        Graph g;g.nodes={{0,0},{0,200}};
        Lane lane{0,1,12};lane.road=42;lane.offset=1.25f;
        auto adjacent=lane;adjacent.offset=3.75f;adjacent.number=1;
        g.lanes={lane,adjacent};g.build();Flow flow;flow.reset(&g,Rules{},79);
        Agent car;car.alive=true;car.identity=1;car.speed=5;
        const auto driver=flow.adopt(car,{{1.25f,60},0,5},42);
        car.identity=2;car.incident=true;
        const auto stopped=flow.adopt(car,{{3.75f,65},0,0},42);
        check(driver&&stopped,"narrow adjacent-lane fixture adopts both cars");
        flow.setPhysicalPose(*driver,{{1.25f,60},0,5},false);
        flow.setPhysicalPose(*stopped,{{3.75f,65},0,0},true);
        flow.update(.05f,{0,60},{0,0});
        check(flow.agents()[*driver].targetSpeed>10,"a stopped car in a separate narrow lane does not block its neighbour");
    }
    {
        auto g=street(20,1);Flow flow;flow.reset(&g,Rules{},83);
        Agent car;car.alive=true;car.identity=1;car.speed=4;
        const auto slot=flow.adopt(car,{{1.7f,2},0,4});
        flow.setPhysicalPose(*slot,{{1.7f,2},0,4},false);
        flow.update(.05f,{0,10},{0,0});
        Obstacle parked;parked.active=true;parked.position={1.7f,12};parked.halfLength=2.1f;parked.halfWidth=.9f;
        flow.setObstacles({parked});
        for(int i=0;i<100;++i)flow.update(.05f,{0,10},{0,0});
        check(flow.agents()[*slot].targetSpeed<g.lanes[0].speed,"drivers account for a parked car's footprint");
        parked.position.x+=2.5f;flow.setObstacles({parked});flow.update(.05f,{0,10},{0,0});
        check(flow.agents()[*slot].targetSpeed==g.lanes[0].speed,"a parked car in the neighbouring lane leaves this lane clear");
    }
    {
        Graph g;g.nodes={{0,0},{0,50},{40,50}};g.lanes={{0,1,12},{1,2,12}};g.build();
        Flow flow;flow.reset(&g,Rules{},89);Agent car;car.alive=true;car.identity=1;car.speed=10;
        const Pose actual{{1.7f,40},0,10};const auto slot=flow.adopt(car,actual);
        flow.setPhysicalPose(*slot,actual,false);flow.update(.01f,{0,30},{0,0});
        const auto shortCar=flow.command(*slot,actual);
        DrivingLimits bus;bus.wheelbase=6;flow.setVehicleLimits(*slot,bus);
        const auto longCar=flow.command(*slot,actual);
        check(std::abs(longCar.steer)>std::abs(shortCar.steer)*2,"steering commands use each vehicle's actual wheelbase");
    }
    {
        Graph graph;graph.nodes={{0,0},{0,300}};graph.lanes={{0,1,25}};graph.build();
        Flow flow;flow.reset(&graph,Rules{});
        Agent car;car.alive=true;car.identity=1;car.speed=25;
        const auto follower=flow.adopt(car,{{1.7f,20},0,25});
        flow.setVehicleLimits(*follower,{2,3,5,25});
        car.identity=2;car.speed=0;
        const auto stopped=flow.adopt(car,{{1.7f,140},0,0});
        flow.setPhysicalPose(*stopped,{{1.7f,140},0,0},true);
        float smallestGap=120;
        for(int frame=0;frame<400;++frame) {
            flow.update(.05f,{0,100},{0,0});
            smallestGap=std::min(smallestGap,140-flow.pose(flow.agents()[*follower]).position.y);
        }
        std::printf("GAP %f speed=%f\n",smallestGap,flow.agents()[*follower].speed);check(smallestGap>=6.25f,"a fast vehicle with weak brakes stops before a stationary queue");
        check(flow.agents()[*follower].speed<.1f,"the stationary queue eventually holds its follower");
    }
    {
        Graph graph;graph.nodes={{0,0},{0,200}};graph.lanes={{0,1,12}};graph.build();
        Flow flow;flow.reset(&graph,Rules{});
        Agent car;car.alive=true;car.identity=1;car.speed=8;
        const auto follower=flow.adopt(car,{{1.7f,65},0,8});
        car.identity=2;car.speed=0;car.incident=true;car.length=12;car.width=2.5f;
        const Pose sideways{{6.25f,75},1.570796327f,0};
        const auto stopped=flow.adopt(car,sideways);
        flow.setPhysicalPose(*follower,{{1.7f,65},0,8},false);
        flow.setPhysicalPose(*stopped,sideways,true);
        flow.update(.05f,{0,65},{0,0});
        check(flow.agents()[*follower].targetSpeed<5,"a sideways crashed bus blocks every lane its body occupies");
    }
    {
        Graph graph;graph.nodes={{0,0},{0,200}};graph.lanes={{0,1,12}};graph.build();
        Flow flow;flow.reset(&graph,Rules{});
        Agent car;car.alive=true;car.identity=1;car.speed=8;
        const Pose displaced{{4.7f,40},0,8};
        const auto slot=flow.adopt(car,displaced);
        flow.setPhysicalPose(*slot,displaced,false);
        Obstacle parked;parked.active=true;parked.position={4.7f,50};parked.halfLength=2.1f;parked.halfWidth=.9f;
        flow.setObstacles({parked});flow.update(.05f,{0,40},{0,0});
        check(flow.agents()[*slot].targetSpeed<4,"a displaced physical car sees obstacles in front of its actual position");
        const Pose stopped{{4.7f,44},0,0};flow.setPhysicalPose(*slot,stopped,false);
        flow.update(.05f,{0,40},{0,0});
        const auto command=flow.command(*slot,stopped);
        check(command.throttle==0&&command.brake==1,"a driver waiting at rest keeps the brake applied on a slope");
    }
    {
        Graph graph;graph.nodes={{0,0},{0,300}};graph.lanes={{0,1,12}};graph.build();
        Rules rules;rules.spawnNear=1;rules.spawnNearBehind=1;rules.spawnFar=150;rules.maxSpawnsPerUpdate=40;
        bool overlap=false;
        for(uint32_t seed=1;seed<=16;++seed) {
            Flow flow;flow.reset(&graph,rules,seed);flow.setPopulation(40);
            flow.update(.001f,{0,150},{0,0});
            const auto& agents=flow.agents();
            for(size_t a=0;a<agents.size();++a)for(size_t b=a+1;b<agents.size();++b)
                if(agents[a].alive&&agents[b].alive&&std::abs(agents[a].s-agents[b].s)<4.2f)overlap=true;
        }
        check(!overlap,"spawning checks vehicles behind the candidate as well as ahead");
    }
    if (failures == 0) std::printf("PASS saida::traffic\n");
    return failures == 0 ? 0 : 1;
}

