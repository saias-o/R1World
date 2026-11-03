#include "session.hpp"
#include "online_rules.hpp"
#include "saida/netcode/Clock.hpp"
#include "saida/netcode/Replica.hpp"
#include "saida/p2p/Host.hpp"
#include "saida/p2p/Rendezvous.hpp"
#include "saida/p2p/Socket.hpp"

namespace r1::world {
namespace o = r1::online;
namespace p2p = saida::p2p;
namespace nc = saida::netcode;

struct World::Impl::Online {
    p2p::UdpSocket socket, lan;
    std::unique_ptr<p2p::Host> host;
    std::unique_ptr<p2p::Rendezvous> meet;
    std::map<o::Cell, p2p::Topic> cells;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    double now = 0, nextSend = 0, nextCells = 0, nextNodes = 600, lastOwn = -1;
    nc::History<o::Avatar> history{o::kHistorySeconds};
    o::Avatar own;
    bool ownKnown = false;
    uint8_t life = 0;

    struct Remote {
        p2p::PeerHandle peer = p2p::kNoPeer;
        uint64_t id = 0;
        std::string name;
        nc::Replica<o::Avatar> replica;
        o::Avatar shown;
        bool visible = false;
        saida::Node* root = nullptr;
        saida::Node* body = nullptr;
        saida::Transform bodyRest;
        saida::Animator* animator = nullptr;
        saida::TwoBoneIKModifier* arms[2]{nullptr, nullptr};
        saida::Node* pistol = nullptr;
        saida::Node* launcher = nullptr;
        saida::RigidBodyNode* capsule = nullptr;
        r1::Knockdown fall;
        // A body that got up where its ragdoll lay glides onto its owner's path.
        nc::Smoother<3> settle{.15f, 6.f};
        std::string clip;
        double punch = -1;
        int hand = 0;
        double lastShot = -1e9, lastPunch = -1e9, lastRocket = -1e9;
        bool met = false;
        uint64_t samples[4] = {};  // by ReplicaSample: how he was drawn
        double travelled = 0;      // metres drawn, for the smoke's measure
        bool placed = false;
    };
    std::map<p2p::PeerHandle, Remote> remotes;
    // Bodies of players gone, kept for the next one: built once, reused.
    struct Body {
        saida::Node* root;
        saida::Node* body;
        saida::Transform bodyRest;
        saida::Animator* animator;
        saida::TwoBoneIKModifier* arms[2];
        saida::Node *pistol, *launcher;
        saida::RigidBodyNode* capsule;
        saida::RagdollBehaviour* ragdoll;
    };
    std::vector<Body> spare;
    struct Pending {
        p2p::PeerHandle from;
        o::Message message;
    };
    std::vector<Pending> inbox;
    std::map<std::string, uint64_t> refused;
    std::map<std::string, double> refusedSaid;
    std::string hud;
    double hudAt = -1;
    bool dhtSaid = false;
    fs::path nodesFile;
    // The two-instance smoke (R1WORLD_ONLINE_SMOKE).
    int hitsTaken = 0, hitsGiven = 0, hitsDenied = 0, rounds = 0;
    double sawAt = -1, smokeFired = 0, passedAt = -1;

    double seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); }
    std::set<p2p::PeerHandle> audience() const {
        std::set<p2p::PeerHandle> out;
        for (const auto& [cell, topic] : cells)
            for (p2p::PeerHandle p : meet->peers(topic)) out.insert(p);
        return out;
    }
    bool shares(p2p::PeerHandle peer) const {
        for (const auto& [cell, topic] : cells) {
            const auto peers = meet->peers(topic);
            if (std::find(peers.begin(), peers.end(), peer) != peers.end()) return true;
        }
        return false;
    }
    Remote* byId(uint64_t id) {
        for (auto& [peer, r] : remotes)
            if (r.id == id) return &r;
        return nullptr;
    }
    // Every refusal is counted and said, once a second per reason (CLAUDE.md §3).
    void refuse(const std::string& what, const std::string& why) {
        ++refused[why];
        double& said = refusedSaid[why];
        if (now - said < 1) return;
        said = now;
        saida::Log::info("[World online] refused ", what, ": ", why, " (", refused[why], " so far)");
    }
    void send(const o::Message& m, p2p::Channel channel, p2p::PeerHandle only = p2p::kNoPeer) {
        const auto bytes = o::encode(m);
        if (only != p2p::kNoPeer) {
            host->send(only, bytes, channel);
            return;
        }
        for (p2p::PeerHandle p : audience()) host->send(p, bytes, channel);
    }
};

void World::Impl::OnlineDeleter::operator()(Online* online) const {
    if (!online) return;
    if (online->meet && !online->nodesFile.empty()) {
        if (auto* dht = online->meet->dht()) {
            std::ofstream out(online->nodesFile, std::ios::trunc);
            for (const auto& a : dht->nodes(64)) out << a.toString() << "\n";
        }
    }
    delete online;
}

namespace {
std::string hex(const p2p::Key& key) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : key) s += digits[b >> 4], s += digits[b & 15];
    return s;
}
bool unhex(const std::string& s, p2p::Key& key) {
    if (s.size() != 64) return false;
    auto value = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < 32; ++i) {
        const int hi = value(s[2 * i]), lo = value(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        key[i] = uint8_t(hi << 4 | lo);
    }
    return true;
}
const char* clipOf(o::Motion m) {
    return m == o::Motion::Jump ? "jump" : m == o::Motion::Sprint ? "sprint" : m == o::Motion::Run ? "run" : "idle";
}
} // namespace

void World::Impl::startOnline() {
    const bool wanted = smoke ? onlineSmoke != nullptr : onlineWanted;
    if (!wanted) return;
    if (onlineSmoke && std::string(onlineSmoke) != "target" && std::string(onlineSmoke) != "shooter")
        throw std::runtime_error("R1WORLD_ONLINE_SMOKE must be target or shooter");
    std::unique_ptr<Online, OnlineDeleter> n(new Online);
    if (!n->socket.open()) {
        saida::Log::error("[World online] no UDP socket (", n->socket.error(), "): playing alone");
        text("online", "Online unavailable: no network socket.");
        return;
    }
    // A stable identity across runs (saves/, beside the profile); a test
    // run makes a fresh one, so two instances on one machine are two players.
    p2p::Identity identity = p2p::Identity::generate();
    const fs::path keyFile = game / "saves/online-identity.key";
    if (!smoke) {
        std::ifstream in(keyFile);
        std::string line;
        p2p::Key secret{};
        if (in && std::getline(in, line) && unhex(line, secret)) identity.keys = p2p::KeyPair::fromSecret(secret);
        else {
            std::error_code ec;
            fs::create_directories(keyFile.parent_path(), ec);
            std::ofstream out(keyFile, std::ios::trunc);
            out << hex(identity.keys.secret) << "\n";
            if (!out) saida::Log::error("[World online] cannot keep the identity in ", keyFile.string(), ": a new one each run");
        }
    }
    p2p::HostConfig hc;
    hc.application = o::kApplication;
    hc.maxPeers = 48;
    n->host = std::make_unique<p2p::Host>(n->socket, identity, hc);
    p2p::RendezvousConfig rc;
    n->nodesFile = game / "cache/online-nodes.txt";
    if (std::ifstream nodes{n->nodesFile}) {
        std::string line;
        p2p::Address a;
        while (std::getline(nodes, line))
            if (p2p::Address::parse(line, a) && a.valid()) rc.knownNodes.push_back(a);
    }
    rc.maxPeersPerTopic = 32;
    n->meet = std::make_unique<p2p::Rendezvous>(*n->host, rc);
    if (p2p::openLanSocket(n->lan, rc)) n->meet->setLanSocket(&n->lan);
    else saida::Log::info("[World online] no LAN beacons (", n->lan.error(), "): players behind this router meet through the DHT only");
    Online* self = n.get();
    n->host->setReceiver([self](p2p::PeerHandle from, const uint8_t* data, size_t size, p2p::Channel) {
        o::Message m;
        const p2p::PeerInfo* info = self->host->peer(from);
        if (!info) return;
        if (!o::decode(data, size, m)) return self->refuse("a message from " + o::playerName(info->id), "malformed");
        if (m.kind != o::Kind::State) {
            if (self->inbox.size() < 512) self->inbox.push_back({from, m});
            else self->refuse("a message from " + o::playerName(info->id), "too many at once");
            return;
        }
        auto found = self->remotes.find(from);
        if (found == self->remotes.end()) {
            if (self->remotes.size() >= o::kMaxPlayers)
                return self->refuse("the avatar of " + o::playerName(info->id), "already drawing the most players");
            Online::Remote r;
            r.peer = from, r.id = info->id, r.name = o::playerName(info->id);
            found = self->remotes.emplace(from, std::move(r)).first;
            saida::Log::info("[World online] ", found->second.name, " is here, from ", info->address.toString(),
                             ", round trip ", int(info->rtt * 1000), " ms");
        }
        found->second.replica.push(m.sent, self->now, m.avatar);
    });
    saida::Log::info("[World online] ", o::playerName(identity.id()), " on UDP ", n->socket.local().toString(),
                     ", ", rc.knownNodes.size(), " DHT nodes remembered");
    online = std::move(n);
}

void World::Impl::pumpOnline() {
    SAIDA_PROFILE_SCOPE("World/Online");
    if (!online) {
        if (!playing || onlineStarted) return;
        onlineStarted = true;
        startOnline();
        if (!online) return;
    }
    Online& n = *online;
    n.now = n.seconds();
    n.host->poll(n.now);
    n.meet->poll(n.now);
    auto forget = [&](p2p::PeerHandle peer, const char* why) {
        auto found = n.remotes.find(peer);
        if (found == n.remotes.end()) return;
        Online::Remote& r = found->second;
        saida::Log::info("[World online] ", r.name, " left: ", why);
        if (r.root) {
            r.root->setEnabled(false);
            if (r.fall.ragdoll()) r.fall.release();
            n.spare.push_back({r.root, r.body, r.bodyRest, r.animator, {r.arms[0], r.arms[1]}, r.pistol, r.launcher,
                               r.capsule, r.fall.ragdoll()});
        }
        if (punchRemote == r.id) punchRemote = 0;
        n.remotes.erase(found);
    };
    for (const auto& e : n.host->takeEvents())
        if (e.type == p2p::HostEventType::Disconnected) forget(e.peer, p2p::reasonName(e.reason));
    for (const auto& e : n.meet->takeEvents())
        if (e.type == p2p::RendezvousEventType::PeerLeft && !n.shares(e.peer)) forget(e.peer, "no longer near");
    // The cells around him: joined within kMeetRadius, left beyond kLeaveRadius.
    if (n.now >= n.nextCells) {
        n.nextCells = n.now + .5;
        std::set<o::Cell> keep;
        if (playing) {
            for (const auto& c : o::cellsWithin(lon, lat, o::kLeaveRadius)) keep.insert(c);
            for (const auto& c : o::cellsWithin(lon, lat, o::kMeetRadius))
                if (!n.cells.count(c)) {
                    const auto topic = p2p::Topic::named(o::kApplication, c.name());
                    n.meet->join(topic);
                    n.cells.emplace(c, topic);
                }
        }
        for (auto it = n.cells.begin(); it != n.cells.end();)
            if (keep.count(it->first)) ++it;
            else n.meet->leave(it->second), it = n.cells.erase(it);
    }
    // His own avatar: recorded every frame (the judge's history), sent at 20 Hz.
    if (playing && player && feet) {
        o::Avatar a;
        glm::vec3 at = player->transform().position;
        if (driving() || sailing() || piloting()) a.pose = o::Pose::Vehicle;
        else if (playerDown) a.pose = wasted ? o::Pose::Dead : o::Pose::Down, at = downAnchor();
        else if (dropFlying()) a.pose = o::Pose::Air;
        else if (swimming()) a.pose = o::Pose::Swim;
        if (a.pose == o::Pose::Vehicle) a.lon = lon, a.lat = lat, a.alt = float(alt);
        else {
            const auto geo = sceneToGeo(at);
            a.lon = geo.x, a.lat = geo.y, a.alt = float(geo.z);
        }
        const glm::vec3 front = player->transform().rotation * glm::vec3(0, 0, -1);
        a.heading = float(wrap(std::atan2(front.x, -front.z) / rad));
        const glm::vec3 look = camera->transform().rotation * glm::vec3(0, 0, -1);
        a.aimYaw = float(wrap(std::atan2(look.x, -look.z) / rad));
        a.aimPitch = float(std::asin(std::clamp(look.y, -1.f, 1.f)) / rad);
        if (n.ownKnown && n.now > n.lastOwn) {
            const double dt = n.now - n.lastOwn;
            const auto moved = Frame(n.own.lon, n.own.lat, n.own.alt).local(ecef(a.lon, a.lat, a.alt));
            // A step no body takes in a frame is a respawn or a teleport:
            // the others jump with him instead of watching him fly there.
            if (glm::length(moved) > 30 + 80 * dt) ++n.life;
            else a.east = float(moved.x / dt), a.north = float(-moved.z / dt), a.up = float(moved.y / dt);
        }
        const std::string clip = animators.empty() ? "" : animators.front()->currentClip();
        a.motion = clip == "jump" ? o::Motion::Jump : clip == "sprint" ? o::Motion::Sprint : clip == "run" ? o::Motion::Run : o::Motion::Idle;
        a.weapon = arsenal.held();
        a.aiming = aiming;
        a.health = uint8_t(std::clamp(std::ceil(health.hp), 0., 100.));
        a.life = n.life;
        n.own = a, n.ownKnown = true, n.lastOwn = n.now;
        n.history.record(n.now, a);
        if (n.now >= n.nextSend) {
            n.nextSend = std::max(n.nextSend + o::kSendSeconds, n.now - o::kSendSeconds);
            o::Message m;
            m.kind = o::Kind::State, m.sent = n.now, m.avatar = a;
            n.send(m, p2p::Channel::Unreliable);
        }
    }
    if (n.now >= n.nextNodes) {
        n.nextNodes = n.now + 600;
        if (auto* dht = n.meet->dht()) {
            std::ofstream out(n.nodesFile, std::ios::trunc);
            for (const auto& a : dht->nodes(64)) out << a.toString() << "\n";
        }
    }
    if (!n.dhtSaid) {
        const auto status = n.meet->status();
        if (status.dhtReady) {
            n.dhtSaid = true;
            saida::Log::info("[World online] DHT ready after ", n.now, " s: ", status.dhtGoodNodes, " good nodes of ",
                             status.dhtNodes, ", public address ", status.publicAddress.valid() ? status.publicAddress.toString() : "not known yet");
        }
    }
    n.host->flush();
    onlineHud();
}

void World::Impl::onlineHud() {
    Online& n = *online;
    if (n.now - n.hudAt < .5) return;
    n.hudAt = n.now;
    size_t near = 0;
    for (const auto& [peer, r] : n.remotes) near += r.visible;
    const auto status = n.meet->status();
    std::string line;
    if (!status.problem.empty() && n.remotes.empty()) line = "Online: " + status.problem;
    else if (near) line = "Online · " + std::to_string(near) + (near == 1 ? " player nearby" : " players nearby");
    else line = status.dhtReady ? "Online · nobody else here yet" : "Online · looking for players…";
    if (line == n.hud) return;
    n.hud = line;
    text("online", line);
}

void World::Impl::showOnline(double dt) {
    if (!online) return;
    SAIDA_PROFILE_SCOPE("World/OnlinePlayers");
    Online& n = *online;
    const uint64_t me = n.host->identity().id();
    auto local = [&](const std::array<double, 3>& p) { return glm::vec3(origin.local(glm::dvec3(p[0], p[1], p[2]))); };
    auto direction = [&](const std::array<float, 3>& d) {
        return glm::vec3(glm::transpose(origin.basis) * glm::dvec3(d[0], d[1], d[2]));
    };
    // In a menu he is not in the fight; at the wheel his history says so.
    const bool live = !menu && !playerMenu;
    // What the others did since the last frame.
    for (auto& [from, m] : std::exchange(n.inbox, {})) {
        auto found = n.remotes.find(from);
        if (found == n.remotes.end() || !found->second.replica.newest()) {
            n.refuse("an action", "from a player not drawn yet");
            continue;
        }
        Online::Remote& r = found->second;
        const o::Avatar& shooter = *r.replica.newest();
        auto judged = [&](double seen, o::Avatar& then) {
            if (!live) return n.refuse("a blow from " + r.name, "he is in a menu"), false;
            if (!n.history.at(seen, then, o::lerp)) return n.refuse("a blow from " + r.name, "too old to judge"), false;
            return true;
        };
        switch (m.kind) {
        case o::Kind::Shot: {
            const glm::vec3 muzzle = local(m.shot.muzzle), dir = direction(m.shot.direction);
            const float heard = glm::distance(camera->transform().position, muzzle);
            effects.muzzle(muzzle);
            sounds.play(r1::CombatSounds::Shot, heard);
            alarm(muzzle, r1::combat::kGunshotHeard, 10.);
            const glm::vec3 end = muzzle + dir * m.shot.length;
            if (m.shot.victim != me) {
                if (!m.shot.victim) effects.burst(r1::CombatEffects::Dust, end);
                else effects.burst(r1::CombatEffects::Blood, end);
                break;
            }
            o::Avatar then;
            if (!judged(m.shot.seen, then)) break;
            const auto verdict = o::judgeShot(m.shot, shooter, then, playerHeight, n.now - r.lastShot);
            o::Message reply;
            reply.kind = o::Kind::Hit, reply.sent = n.now, reply.hit.victim = me;
            if (verdict.refusal) n.refuse("a shot from " + r.name, verdict.refusal), ++n.hitsDenied;
            else {
                r.lastShot = n.now;
                const bool killed = health.hp <= verdict.damage && !health.invincible();
                onlineBlame = r.name + (verdict.head ? "'s pistol, in the head" : "'s pistol");
                effects.burst(r1::CombatEffects::Blood, end);
                hurtPlayer(verdict.damage, glm::normalize(glm::vec3(dir.x, 0.f, dir.z) + glm::vec3(0, .001f, 0)),
                           onlineBlame.c_str(), false);
                text("stream-status", "Shot by " + r.name + ".");
                reply.hit.damage = float(verdict.damage), reply.hit.killed = killed;
                ++n.hitsTaken;
            }
            n.send(reply, p2p::Channel::Reliable, from);
            break;
        }
        case o::Kind::Punch: {
            r.punch = 0, r.hand ^= 1;
            if (r.root) sounds.play(r1::CombatSounds::Swing, glm::distance(camera->transform().position, r.root->transform().position), .6f);
            if (m.punch.victim != me) break;
            o::Avatar then;
            if (!judged(m.punch.seen, then)) break;
            const auto verdict = o::judgePunch(m.punch, shooter, then, n.now - r.lastPunch);
            o::Message reply;
            reply.kind = o::Kind::Hit, reply.sent = n.now, reply.hit.victim = me;
            if (verdict.refusal) n.refuse("a punch from " + r.name, verdict.refusal), ++n.hitsDenied;
            else {
                r.lastPunch = n.now;
                glm::vec3 push = player->transform().position - (r.root ? r.root->transform().position : player->transform().position);
                push.y = 0;
                push = glm::length(push) > 1e-3f ? glm::normalize(push) : glm::vec3(0, 0, 1);
                const bool killed = health.hp <= verdict.damage && !health.invincible();
                onlineBlame = r.name + "'s fists";
                sounds.play(r1::CombatSounds::Punch, followDistance);
                hurtPlayer(verdict.damage, push, onlineBlame.c_str(), m.punch.hook);
                reply.hit.damage = float(verdict.damage), reply.hit.killed = killed;
                ++n.hitsTaken;
            }
            n.send(reply, p2p::Channel::Reliable, from);
            break;
        }
        case o::Kind::Rocket: {
            if (const char* why = o::judgeRocket(m.rocket, shooter, n.now - r.lastRocket)) {
                n.refuse("a rocket from " + r.name, why);
                break;
            }
            r.lastRocket = n.now;
            // Flown here as well: its blast is judged by everyone it reaches.
            saida::QueryFilter filter;
            if (r.capsule) filter.ignore = r.capsule->bodyId();
            if (!launchRocket(local(m.rocket.muzzle), local(m.rocket.target), filter, true, r.id))
                n.refuse("a rocket from " + r.name, "too many rockets in flight");
            break;
        }
        case o::Kind::Hit:
            if (m.hit.damage <= 0) {
                text("stream-status", "No hit on " + r.name + ": his screen disagrees.");
                saida::Log::info("[World online] ", r.name, " refused our hit");
            } else {
                ++n.hitsGiven;
                text("stream-status", m.hit.killed ? r.name + " is down." : "Hit " + r.name + " (" + std::to_string(int(m.hit.damage)) + ").");
                saida::Log::info("[World online] hit ", r.name, ": ", m.hit.damage, m.hit.killed ? ", killed" : "");
            }
            break;
        default: break;
        }
    }
    // Their bodies, drawn where their owners were a moment ago.
    const glm::vec3 up(0, 1, 0);
    for (auto& [peer, r] : n.remotes) {
        o::Avatar a;
        const auto kind = r.replica.sample(n.now, a, o::lerp);
        ++r.samples[size_t(kind)];
        const bool away = kind == nc::ReplicaSample::Empty || n.now - r.replica.lastArrival() > o::kAwaySeconds;
        const bool show = !away && a.pose != o::Pose::Vehicle && playing;
        if (show && !r.root) {
            if (!n.spare.empty()) {
                const auto b = n.spare.back();
                n.spare.pop_back();
                r.root = b.root, r.body = b.body, r.bodyRest = b.bodyRest, r.animator = b.animator;
                r.arms[0] = b.arms[0], r.arms[1] = b.arms[1], r.pistol = b.pistol, r.launcher = b.launcher, r.capsule = b.capsule;
                r.fall.bind(*b.ragdoll, *r.capsule, r1::combat::kLieSeconds);
                r.clip.clear();
            } else {
                // His body is the player's own scan, rig and clips, shared.
                auto root = std::make_unique<saida::Node>("online-" + r.name);
                auto body = std::make_unique<saida::Node>("online-body");
                body->transform() = playerBodyRest;
                auto* animator = body->addBehaviour<saida::Animator>();
                const saida::Animator* source = animators.front();
                animator->setRig(const_cast<saida::Rig*>(source->rig()));
                for (const auto& [name, clip] : source->clips()) animator->addClip(name, clip);
                r.arms[0] = animator->addModifier<saida::TwoBoneIKModifier>(*source->rig(), r1::rocketboxArm(false));
                r.arms[1] = animator->addModifier<saida::TwoBoneIKModifier>(*source->rig(), r1::rocketboxArm(true));
                for (auto& c : playerBody->children()) {
                    auto* mesh = dynamic_cast<saida::MeshNode*>(c.get());
                    if (!mesh || c.get() == pistol || c.get() == launcher) continue;
                    auto copy = std::make_unique<saida::MeshNode>(mesh->name(), mesh->mesh(), mesh->material());
                    copy->transform() = mesh->transform();
                    copy->castShadows() = mesh->castShadows();
                    body->addChild(std::move(copy));
                }
                if (auto gun = r1::buildPistol(engine.resources())) {
                    gun->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 R Hand", pistolGrip);
                    r.pistol = body->addChild(std::move(gun));
                }
                auto tube = r1::buildRocketModel(engine.resources(), true);
                tube->addBehaviour<saida::BoneAttachmentBehaviour>("Bip01 R Hand", pistolGrip);
                r.launcher = body->addChild(std::move(tube));
                auto* ragdoll = body->addBehaviour<saida::RagdollBehaviour>(ragdollProfile);
                // A kinematic capsule like a passer-by's: his feet meet it,
                // a round or a rocket stops on it.
                auto capsule = std::make_unique<saida::RigidBodyNode>();
                capsule->kinematic = true;
                capsule->transform().position = glm::vec3(0.f, float(playerHeight * .5), 0.f);
                auto shape = std::make_unique<saida::CollisionShapeNode>();
                shape->shapeType = saida::CollisionShapeType::Capsule;
                shape->radius = float(r1::Crowd::kBodyRadius);
                shape->height = float(playerHeight);
                shape->axis = 1;
                capsule->addChild(std::move(shape));
                r.capsule = static_cast<saida::RigidBodyNode*>(root->addChild(std::move(capsule)));
                r.animator = animator;
                r.body = root->addChild(std::move(body));
                r.bodyRest = r.body->transform();
                r.fall.bind(*ragdoll, *r.capsule, r1::combat::kLieSeconds);
                r.root = engine.sceneTree().world().addChild(std::move(root));
            }
        }
        r.visible = show;
        if (!r.root) continue;
        if (r.root->enabled() != show) r.root->setEnabled(show);
        if (!show) {
            r.placed = false;
            continue;
        }
        if (!r.met) {
            r.met = true;
            saida::Log::info("[World online] drawing ", r.name, " ", int(metresBetween(lon, lat, a.lon, a.lat)), " m away, ",
                             int(r.replica.targetDelay() * 1000), " ms behind his clock");
        }
        auto* ragdoll = r.fall.ragdoll();
        const auto state = ragdoll ? ragdoll->state() : saida::RagdollBehaviour::State::Animated;
        const bool down = a.pose == o::Pose::Down || a.pose == o::Pose::Dead;
        const glm::vec3 velocity(glm::transpose(origin.basis) * Frame(a.lon, a.lat).basis * glm::dvec3(a.east, a.up, -a.north));
        if (down && state == saida::RagdollBehaviour::State::Animated) {
            for (auto* arm : r.arms) if (arm) arm->release();
            beginFall(r.fall, velocity + glm::vec3(0, 1.f, 0), glm::cross(up, velocity) * .5f, "another player's");
        } else if (!down && state != saida::RagdollBehaviour::State::Animated && state != saida::RagdollBehaviour::State::Blending) {
            if (state == saida::RagdollBehaviour::State::Simulated) ragdoll->freeze();
            const glm::vec3 lying = ragdoll->ragdoll() ? ragdoll->ragdoll()->rootPosition() : r.root->transform().position;
            ragdoll->blendOutToParent(float(r1::combat::kStandSeconds), r.bodyRest, sceneGround(lying));
            const glm::vec3 then = r.root->transform().position;
            const glm::vec3 there(origin.local(ecef(a.lon, a.lat, a.alt + .06)));
            const float drawn[3] = {then.x, then.y, then.z}, path[3] = {there.x, there.y, there.z};
            r.settle.correct(drawn, path);
        } else if (state == saida::RagdollBehaviour::State::Simulated && ragdoll->simulatedSeconds() >= r1::combat::kLongestTumble) {
            ragdoll->freeze();
        }
        if (state == saida::RagdollBehaviour::State::Simulated || state == saida::RagdollBehaviour::State::Frozen) {
            r.shown = a;
            continue;  // the ragdoll has his body; its bodies follow the root
        }
        // Placed: the owner's position, a correction gliding off it.
        const glm::vec3 there(origin.local(ecef(a.lon, a.lat, a.alt + .06)));
        r.settle.decay(float(dt));
        const float path[3] = {there.x, there.y, there.z};
        float drawn[3];
        r.settle.apply(path, drawn);
        const glm::vec3 placed(drawn[0], drawn[1], drawn[2]);
        if (r.placed && r.shown.life == a.life) r.travelled += glm::distance(placed, r.root->transform().position);
        r.placed = true;
        r.root->transform().position = placed;
        glm::quat facing = glm::angleAxis(float(-a.heading * rad), up);
        if (a.pose == o::Pose::Swim && a.motion != o::Motion::Idle) facing = facing * glm::angleAxis(float(-35. * rad), glm::vec3(1, 0, 0));
        r.root->transform().rotation = facing;
        const std::string clip = clipOf(a.pose == o::Pose::Air ? o::Motion::Jump : a.pose == o::Pose::Swim && a.motion != o::Motion::Idle ? o::Motion::Run : a.motion);
        if (clip != r.clip) {
            r.clip = clip;
            if (clip == "run" || clip == "sprint") {
                saida::ClipView view;
                view.name = clip, view.source = clip, view.speed = float(1. / kPeopleVehicleResize);
                r.animator->playView(view);
            } else r.animator->play(clip);
        }
        if (r.pistol) r.pistol->setVisible(a.weapon == r1::combat::Weapon::Pistol);
        if (r.launcher) r.launcher->setVisible(a.weapon == r1::combat::Weapon::RocketLauncher);
        // The arms: a weapon held along his aim, or a punch thrown.
        const auto& pose = r.animator->globalPose().globalMatrices;
        const saida::Rig& rig = *r.animator->rig();
        const int bones[2] = {rig.findBoneIndex("Bip01 L UpperArm"), rig.findBoneIndex("Bip01 R UpperArm")};
        if (bones[0] < 0 || bones[1] < 0 || pose.size() < rig.boneCount() || !r.arms[0] || !r.arms[1]) continue;
        const glm::vec3 shoulder[2] = {glm::vec3(pose[size_t(bones[0])][3]), glm::vec3(pose[size_t(bones[1])][3])};
        const glm::mat4 toObject = glm::inverse(r.body->worldTransform());
        if (r.punch >= 0) {
            r.punch += dt;
            const float w = float(r1::combat::Punch::extensionAt(r.punch));
            const int h = r.hand;
            r.arms[h]->clearTipRotation();
            r.arms[h]->reach(shoulder[h] + glm::vec3(h ? -.08f : .08f, .05f, playerReach * .95f), w);
            r.arms[h]->setWeightNow(w);
            r.arms[h]->setPole(shoulder[h] + glm::vec3(h ? -.45f : .45f, -.35f, -.25f));
            if (r.punch >= r1::combat::Punch::kRecover) r.punch = -1, r.arms[h]->release();
        } else if ((a.aiming || a.weapon == r1::combat::Weapon::RocketLauncher) && a.weapon != r1::combat::Weapon::Fists) {
            const double yawR = a.aimYaw * rad, pitchR = a.aimPitch * rad;
            const glm::dvec3 enu(std::sin(yawR) * std::cos(pitchR), std::sin(pitchR), -std::cos(yawR) * std::cos(pitchR));
            const glm::vec3 look(glm::transpose(origin.basis) * Frame(a.lon, a.lat).basis * enu);
            holdWeapon(r.arms, shoulder, glm::normalize(glm::vec3(toObject * glm::vec4(look, 0.f))), a.weapon, playerReach, false);
        } else {
            for (auto* arm : r.arms) if (arm->weight() > 0) arm->release(), arm->clearTipRotation();
        }
        r.shown = a;
    }
}

uint64_t World::Impl::remoteByBody(const saida::RaycastHit& hit, double& seen) {
    if (!online || !hit.hit) return 0;
    for (auto& [peer, r] : online->remotes) {
        if (!r.visible || !r.capsule) continue;
        const auto* ragdoll = r.fall.ragdoll();
        if (hit.body == r.capsule->bodyId() ||
            (ragdoll && ragdoll->ragdoll() && ragdoll->ragdoll()->partOf(hit.body) >= 0)) {
            seen = r.replica.playback();
            return r.id;
        }
    }
    return 0;
}

uint64_t World::Impl::remoteInFront(const glm::vec3& from, const glm::vec3& ahead, double reach, double& distance, double& seen) {
    if (!online) return 0;
    uint64_t best = 0;
    for (auto& [peer, r] : online->remotes) {
        if (!r.visible || !r.root || (r.shown.pose != o::Pose::Stand && r.shown.pose != o::Pose::Air)) continue;
        glm::vec3 d = r.root->transform().position - from;
        d.y = 0;
        const float flat = glm::length(d);
        if (flat >= reach || flat < 1e-3f || glm::dot(d / flat, ahead) < .45f) continue;
        reach = flat, distance = flat, seen = r.replica.playback(), best = r.id;
    }
    return best;
}

glm::vec3 World::Impl::remotePosition(uint64_t id) const {
    if (!online) return player->transform().position;
    for (const auto& [peer, r] : online->remotes)
        if (r.id == id && r.root) return r.root->transform().position;
    return player->transform().position;
}

void World::Impl::sendShot(const glm::vec3& muzzle, const glm::vec3& direction, float length, uint64_t victim, double seen) {
    if (!online) return;
    o::Message m;
    m.kind = o::Kind::Shot, m.sent = online->now;
    const glm::dvec3 at = origin.origin + origin.basis * glm::dvec3(muzzle);
    const glm::dvec3 d = glm::normalize(origin.basis * glm::dvec3(direction));
    m.shot.muzzle = {at.x, at.y, at.z};
    m.shot.direction = {float(d.x), float(d.y), float(d.z)};
    m.shot.length = std::clamp(length, .01f, float(r1::combat::kPistol.range));
    m.shot.victim = victim, m.shot.seen = seen;
    online->send(m, p2p::Channel::Reliable);
}

void World::Impl::sendPunch(uint64_t victim, double seen, bool hook) {
    if (!online) return;
    o::Message m;
    m.kind = o::Kind::Punch, m.sent = online->now;
    m.punch.victim = victim, m.punch.seen = seen, m.punch.hook = hook;
    online->send(m, p2p::Channel::Reliable);
}

void World::Impl::sendRocket(const glm::vec3& muzzle, const glm::vec3& target) {
    if (!online) return;
    o::Message m;
    m.kind = o::Kind::Rocket, m.sent = online->now;
    const glm::dvec3 a = origin.origin + origin.basis * glm::dvec3(muzzle), b = origin.origin + origin.basis * glm::dvec3(target);
    m.rocket.muzzle = {a.x, a.y, a.z}, m.rocket.target = {b.x, b.y, b.z};
    online->send(m, p2p::Channel::Reliable);
}

// Two instances at one spot (tools/online_smoke.py): each must draw the other;
// the shooter fires at the body he draws until the target's machine says it
// was hit; the target passes once hit by the other player.
void World::Impl::runOnlineSmoke(double dt) {
    auto fail = [&](const std::string& why) {
        saida::Log::error("[World E2E] FAIL online ", onlineSmoke, ": ", why);
        testFailed = true;
        engine.sceneTree().quit();
    };
    if (!online) {
        if (onlineStarted) fail("the online session could not start (see the [World online] lines above)");
        return;  // it opens on the next frame of play
    }
    Online& n = *online;
    const bool shooter = std::string(onlineSmoke) == "shooter";
    Online::Remote* other = nullptr;
    for (auto& [peer, r] : n.remotes)
        if (r.visible && r.root) other = &r;
    if (other && n.sawAt < 0) {
        n.sawAt = n.now;
        saida::Log::info("[World online] smoke: drawing ", other->name, " at ",
                         glm::distance(other->root->transform().position, player->transform().position), " m after ", smokeWalk, " s of play");
    }
    auto pass = [&] {
        std::string drawing;
        if (other) {
            const uint64_t* s = other->samples;
            const double all = double(s[0] + s[1] + s[2] + s[3]);
            drawing = ", drawn " + number(100. * s[size_t(nc::ReplicaSample::Interpolated)] / std::max(1., all), 1) +
                      "% between snapshots, " + number(100. * s[size_t(nc::ReplicaSample::Extrapolated)] / std::max(1., all), 1) +
                      "% extrapolated, " + number(other->travelled, 1) + " m travelled, delay " +
                      std::to_string(int(other->replica.targetDelay() * 1000)) + " ms";
        }
        saida::Log::info("[World E2E] PASS online ", onlineSmoke, ": drew the other player, ",
                         shooter ? "hits confirmed " + std::to_string(n.hitsGiven) : "hits taken " + std::to_string(n.hitsTaken),
                         ", refusals ", n.refused.size(), drawing);
        n.passedAt = 1e18;
        // The shooter's picture, taken while the other player still stands there.
        if (const char* png = std::getenv("R1WORLD_ONLINE_SHOT"); png && shooter) {
            saida::CaptureRequest shot;
            shot.pngPath = png, shot.frame = 1, shot.waitForAssets = false;
            engine.captureFrameThenExit(shot);
        } else engine.sceneTree().quit();
    };
    if (n.passedAt >= 1e17) return;
    if (n.passedAt >= 0) {
        // The target lingers, so its verdict on the shot reaches the shooter.
        if (n.now - n.passedAt > 3) pass();
        return;
    }
    if (n.now > 200) {
        const auto status = n.meet->status();
        return fail(n.sawAt < 0 ? "never drew the other player (DHT nodes " + std::to_string(status.dhtNodes) +
                                      ", LAN beacons heard " + std::to_string(status.lanBeaconsHeard) +
                                      (status.problem.empty() ? "" : ", " + status.problem) + ")"
                                : shooter ? "no hit confirmed (" + std::to_string(n.rounds) + " rounds fired, " + std::to_string(n.hitsDenied) + " refused)"
                                          : "never hit by the other player");
    }
    if (!shooter) {
        if (n.hitsTaken > 0 && n.sawAt >= 0) n.passedAt = n.now;
        return;
    }
    if (n.hitsGiven > 0) return pass();
    if (!other || n.now - n.sawAt < 2) return;
    // Look at his chest through the camera, as a player would, then fire.
    selectWeapon(r1::combat::Weapon::Pistol);
    const glm::vec3 chest = other->root->transform().position + glm::vec3(0, float(playerHeight * .6), 0);
    const glm::vec3 d = chest - camera->transform().position;
    yaw = wrap(std::atan2(d.x, -d.z) / rad);
    pitch = std::clamp(std::asin(d.y / std::max(1e-3f, glm::length(d))) / rad, -65., 25.);
    n.smokeFired += dt;
    if (n.smokeFired >= .7) {
        n.smokeFired = 0;
        ++n.rounds;
        fire();
    }
}

} // namespace r1::world
