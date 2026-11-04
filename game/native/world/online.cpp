#include "session.hpp"
#include "online_rules.hpp"
#include "saida/netcode/Clock.hpp"
#include "saida/netcode/Replica.hpp"
#include "saida/p2p/Host.hpp"
#include "saida/p2p/NetworkThread.hpp"
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
    // The network runs on its own thread (engine plugins/p2p): datagrams are
    // read and stamped as they land and snapshots leave at an even rate
    // whatever the frame does. Everything below is touched under hold().
    std::unique_ptr<p2p::NetworkThread> thread;
    p2p::NetworkThread::Lock hold() { return thread->lock(); }
    std::map<o::Cell, p2p::Topic> cells;
    double now = 0, nextCells = 0, nextNodes = 600, lastOwn = -1, nextTiers = 0, nextSummary = 60;
    bool fresh = false;  // his own avatar is current: the thread sends it
    // The minute's frames, for the summary: what playing online costs here.
    std::vector<float> frames;
    double lastFrame = -1;
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
        double distance = 1e18;    // metres from us, as drawn this frame
        float poseRate = -1;       // animation LOD applied to his body
        // What he hears of us (o::tierFor): how often, and when next.
        o::Tier tier = o::Tier::Near;
        double nextSend = 0, lastContact = -1e9;
    };
    std::map<p2p::PeerHandle, Remote> remotes;
    std::map<p2p::PeerHandle, double> arriving;          // linked, no snapshot of his yet: when we send next
    uint64_t sentByTier[size_t(o::Tier::Count)] = {};    // snapshots sent, for the log and the smoke
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
    // Whether each linked player speaks our protocol (o::kProtocol, in his
    // hello). One who does not stays linked -- he is a neighbour like any
    // other to the engine -- but nothing is sent to him or read from him.
    std::string protocol = o::kProtocol;
    std::map<p2p::PeerHandle, bool> sameVersion;
    bool speaks(p2p::PeerHandle peer) {
        const auto known = sameVersion.find(peer);
        if (known != sameVersion.end()) return known->second;
        const p2p::PeerInfo* info = host->peer(peer);
        if (!info) return false;
        const std::string his = o::protocolOf(info->hello);
        const bool same = his == protocol;
        sameVersion[peer] = same;
        if (!same)
            saida::Log::info("[World online] ", o::playerName(info->id), " is on another version (",
                             his.empty() ? "unnamed" : his, ", ours ", protocol, "): linked, not drawn");
        return same;
    }
    size_t foreign() const {
        size_t n = 0;
        for (const auto& [peer, same] : sameVersion) n += !same;
        return n;
    }
    // Joining a friend (online_rules.hpp): our own code's topic, always
    // joined, and the friend's while we look for him. He is the member of
    // that topic who welcomed us.
    p2p::Topic invite, joinTopic;
    std::string joinCode;  // the friend's code whose topic we are in, "" for none
    double joinSince = 0;
    std::set<p2p::PeerHandle> welcomedBy;
    p2p::PeerHandle friendPeer = p2p::kNoPeer;
    bool went = false;
    double nextGo = 0;     // a Go refused is tried again this late
    std::string joinSaid;  // what the panel says of the join, "" when none is under way
    size_t welcomed = 0;   // players who joined us with our code
    double joinedAt = -1;
    std::string passed;    // the smoke's verdict, said once the other has seen it too
    // The cars other players own (o::Vehicle), drawn from what they say.
    struct SharedCar {
        uint64_t owner = 0;
        p2p::PeerHandle peer = p2p::kNoPeer;
        uint16_t claim = 0;
        uint32_t seed = 0;
        uint8_t model = 0;
        o::Car last;
        double heard = 0;
        nc::Replica<o::Car> replica;
        saida::Node* node = nullptr;
    };
    std::map<uint64_t, SharedCar> sharedCars;
    void carHeard(p2p::PeerHandle from, uint64_t owner, const o::Message& m) {
        const o::Vehicle& v = m.vehicle;
        auto found = sharedCars.find(v.car);
        if (found != sharedCars.end() && found->second.owner != owner) {
            // Two players claim it: the later take keeps it (o::outranks).
            if (!o::outranks(v.claim, owner, found->second.claim, found->second.owner)) return;
            saida::Node* node = found->second.node;
            sharedCars.erase(found);
            found = sharedCars.emplace(v.car, SharedCar{}).first;
            found->second.node = node;
        }
        if (found == sharedCars.end()) {
            if (sharedCars.size() >= 512) return refuse("a car of " + o::playerName(owner), "already following the most cars");
            found = sharedCars.emplace(v.car, SharedCar{}).first;
        }
        SharedCar& c = found->second;
        c.owner = owner, c.peer = from, c.claim = v.claim, c.seed = v.seed, c.model = v.model, c.last = v.state;
        c.heard = thread->now();
        c.replica.push(m.sent, c.heard, v.state);
    }
    // What this machine saw each player do (online_rules.hpp, Witness).
    o::Witness witness;
    std::map<std::string, double> refusedSaid;
    std::string hud;
    double hudAt = -1;
    bool dhtSaid = false;
    fs::path nodesFile;
    // The two-instance smoke (R1WORLD_ONLINE_SMOKE).
    int hitsTaken = 0, hitsGiven = 0, hitsDenied = 0, rounds = 0;
    double sawAt = -1, smokeFired = 0, passedAt = -1;

    // Our snapshot to each player near, as often as his tier says. A player
    // linked but not heard from yet gets every snapshot: he is arriving.
    void sendState() {
        if (now >= nextTiers) {
            nextTiers = now + .25;
            std::vector<std::pair<double, Remote*>> byDistance;
            for (auto& [peer, r] : remotes) byDistance.push_back({r.replica.newest() ? r.distance : -1, &r});
            std::sort(byDistance.begin(), byDistance.end(), [](auto& a, auto& b) { return a.first < b.first; });
            size_t near = 0, rank = 0;
            for (auto& [d, r] : byDistance) {
                const o::Avatar* him = r->replica.newest();
                o::Tier t = him ? o::tierFor(own, *him, now - r->lastContact, rank++) : o::Tier::Near;
                if (t == o::Tier::Near && ++near > o::kMaxNear) t = o::Tier::Middle;
                if (t < r->tier) r->nextSend = now;  // promoted: at once
                r->tier = t;
            }
        }
        // Stamped when it was true: a frame that stalls sends nothing new,
        // and the others carry him on, then hold him, as they would a loss.
        if (now - lastOwn > .25) return;
        o::Message m;
        m.kind = o::Kind::State, m.sent = lastOwn, m.avatar = own;
        const auto bytes = o::encode(m);
        const auto heard = audience();
        for (p2p::PeerHandle p : heard) {
            const auto found = remotes.find(p);
            const double every = found == remotes.end() ? o::kSendSeconds : o::kTierSeconds[size_t(found->second.tier)];
            double& next = found == remotes.end() ? arriving[p] : found->second.nextSend;
            if (now < next) continue;
            next = std::max(next + every, now - every);
            host->send(p, bytes, p2p::Channel::Unreliable);
            ++sentByTier[found == remotes.end() ? 0 : size_t(found->second.tier)];
        }
        for (auto it = arriving.begin(); it != arriving.end();)
            it = remotes.count(it->first) || !heard.count(it->first) ? arriving.erase(it) : std::next(it);
    }
    void contact(uint64_t id) {
        if (Remote* r = byId(id)) r->lastContact = now, r->nextSend = std::min(r->nextSend, now);
    }
    // Every minute, and once more at the end: the links, the crowd, what
    // was sent, and what playing online cost this machine's frames.
    void summary(const char* when) {
        const auto status = meet->status();
        const auto network = thread->stats();
        size_t relayed = 0;
        for (p2p::PeerHandle p : host->peers())
            if (const auto* info = host->peer(p); info && info->via != p2p::kNoPeer) ++relayed;
        auto& f = frames;
        auto at = [&](double q) {
            if (f.empty()) return 0;
            auto it = f.begin() + std::ptrdiff_t(std::min(f.size() - 1, size_t(q * double(f.size()))));
            std::nth_element(f.begin(), it, f.end());
            return int(std::lround(*it));
    };
    const std::string frameLine = std::to_string(f.size()) + " frames, median/p95/p99/worst " + std::to_string(at(.5)) +
                                  "/" + std::to_string(at(.95)) + "/" + std::to_string(at(.99)) + "/" + std::to_string(at(1)) + " ms";
    f.clear();
    saida::Log::info("[World online] ", when, host->peers().size(), " linked (", relayed, " relayed), ", remotes.size(),
                     " followed, ", foreign(), " on another version, ", status.candidates,
                     " heard of, crowd about ", int(std::lround(status.population)), "; snapshots sent near/middle/far ", sentByTier[0], "/",
                     sentByTier[1], "/", sentByTier[2], "; ", status.introductionsAsked, " introductions asked, ",
                     status.pruned, " links dropped; announcing in ", status.announcing, " of ", topics().size(),
                     " topics (", cells.size(), " cells); network thread worst poll ", int(network.worstStep * 1000), " ms, worst lateness ",
                     int(network.worstLate * 1000), " ms; ", frameLine);
    }
    // His body goes back to the spares: built once, reused by the next one drawn.
    void releaseBody(Remote& r) {
        if (!r.root) return;
        r.root->setEnabled(false);
        if (r.fall.ragdoll()) r.fall.release();
        spare.push_back({r.root, r.body, r.bodyRest, r.animator, {r.arms[0], r.arms[1]}, r.pistol, r.launcher, r.capsule,
                         r.fall.ragdoll()});
        r.root = r.body = nullptr, r.animator = nullptr, r.arms[0] = r.arms[1] = nullptr;
        r.pistol = r.launcher = nullptr, r.capsule = nullptr;
        r.fall = r1::Knockdown{};
        r.visible = r.placed = false;
    }
    // The topics we hear and are heard in: the cells, our code's, the friend's.
    std::vector<p2p::Topic> topics() const {
        std::vector<p2p::Topic> out{invite};
        for (const auto& [cell, topic] : cells) out.push_back(topic);
        if (!joinCode.empty()) out.push_back(joinTopic);
        return out;
    }
    std::set<p2p::PeerHandle> audience() {
        std::set<p2p::PeerHandle> out;
        for (const auto& topic : topics())
            for (p2p::PeerHandle p : meet->peers(topic))
                if (speaks(p)) out.insert(p);
        return out;
    }
    bool shares(p2p::PeerHandle peer, bool cellsOnly = false) const {
        if (cellsOnly) {
            for (const auto& [cell, topic] : cells) {
                const auto peers = meet->peers(topic);
                if (std::find(peers.begin(), peers.end(), peer) != peers.end()) return true;
            }
            return false;
        }
        for (const auto& topic : topics()) {
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
    if (online->thread) online->thread->stop();
    if (online->host && online->meet && online->thread) online->summary("at the end: ");
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
    if (onlineSmoke && !std::set<std::string>{"target", "shooter", "versions", "host", "joiner", "driver", "watcher"}.count(onlineSmoke))
        throw std::runtime_error("R1WORLD_ONLINE_SMOKE must be target, shooter, versions, host, joiner, driver or watcher");
    std::unique_ptr<Online, OnlineDeleter> n(new Online);
    // The versions smoke plays one of its two instances as another build.
    if (const char* other = std::getenv("R1WORLD_ONLINE_PROTOCOL"); other && smoke) n->protocol = other;
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
    hc.hello.assign(n->protocol.begin(), n->protocol.end());
    // Room above the neighbours kept (a quarter more, and those who keep
    // us) so a newcomer can always be let in and placed.
    hc.maxPeers = o::kNeighbours * 3;  // 48 kept at most (keepRatio), the rest for newcomers and those who keep us
    n->host = std::make_unique<p2p::Host>(n->socket, identity, hc);
    p2p::RendezvousConfig rc;
    n->nodesFile = game / "cache/online-nodes.txt";
    if (std::ifstream nodes{n->nodesFile}) {
        std::string line;
        p2p::Address a;
        while (std::getline(nodes, line))
            if (p2p::Address::parse(line, a) && a.valid()) rc.knownNodes.push_back(a);
    }
    rc.neighbours = o::kNeighbours;
    // A crowd that walks reshuffles its nearest every few seconds: keep half
    // as many again, and longer, so links are not torn down and punched anew
    // as people pass (a link costs a handshake, sometimes a relay).
    rc.keepRatio = 1.5;
    rc.pruneSeconds = 8;
    n->meet = std::make_unique<p2p::Rendezvous>(*n->host, rc);
    n->invite = p2p::arenaTopic(o::kApplication, invite());
    n->meet->join(n->invite);
    if (p2p::openLanSocket(n->lan, rc)) n->meet->setLanSocket(&n->lan);
    else saida::Log::info("[World online] no LAN beacons (", n->lan.error(), "): players behind this router meet through the DHT only");
    Online* self = n.get();
    n->host->setReceiver([self](p2p::PeerHandle from, const uint8_t* data, size_t size, p2p::Channel) {
        o::Message m;
        const p2p::PeerInfo* info = self->host->peer(from);
        if (!info || !self->speaks(from)) return;  // another version: said once, then ignored
        if (!o::decode(data, size, m)) return self->refuse("a message from " + o::playerName(info->id), "malformed");
        if (m.kind == o::Kind::Welcome) {
            self->welcomedBy.insert(from);
            return;
        }
        if (m.kind == o::Kind::Vehicle) return self->carHeard(from, info->id, m);
        if (m.kind != o::Kind::State) {
            if (self->inbox.size() < 512) self->inbox.push_back({from, m});
            else self->refuse("a message from " + o::playerName(info->id), "too many at once");
            return;
        }
        auto found = self->remotes.find(from);
        if (found == self->remotes.end()) {
            // Every linked player is followed (a replica is a few hundred
            // bytes); only the nearest are drawn (showOnline).
            if (self->remotes.size() >= o::kMaxTracked)
                return self->refuse("the avatar of " + o::playerName(info->id), "already following the most players");
            Online::Remote r;
            r.peer = from, r.id = info->id, r.name = o::playerName(info->id);
            found = self->remotes.emplace(from, std::move(r)).first;
            const p2p::PeerInfo* relay = info->via != p2p::kNoPeer ? self->host->peer(info->via) : nullptr;
            saida::Log::info("[World online] ", found->second.name, " is here, ",
                             relay ? "relayed by " + o::playerName(relay->id) : "from " + info->address.toString(),
                             ", round trip ", int(info->rtt * 1000), " ms");
        }
        found->second.replica.push(m.sent, self->thread->now(), m.avatar);
        self->witness.snapshot(info->id, m.sent, m.avatar, self->thread->now());
    });
    n->thread = std::make_unique<p2p::NetworkThread>(*n->host, n->meet.get(), n->socket);
    n->thread->start([self](double now) {
        self->now = now;
        if (self->fresh) self->sendState();
    });
    saida::Log::info("[World online] ", o::playerName(identity.id()), " on UDP ", n->socket.local().toString(),
                     ", ", rc.knownNodes.size(), " DHT nodes remembered, invite code ", invite());
    online = std::move(n);
}

void World::Impl::pumpOnline() {
    SAIDA_PROFILE_SCOPE("World/Online");
    // Go starts the meeting: the globe's ten seconds of flight are spent
    // finding whoever stands where he will land.
    const bool arriving = drop.phase != Drop::None;
    // --join, said like a typed code; the smoke's joiner plays where it
    // spawned first, as a player already in the world does.
    if (!joinArgument.empty() && (!smoke || smokeStarted)) joinFriend(std::exchange(joinArgument, std::string()));
    if (!online) {
        // A friend to join starts it from the menu: he is found before Go.
        if ((!playing && !arriving && friendCode.empty()) || onlineStarted) return;
        onlineStarted = true;
        startOnline();
        if (!online) return;
    }
    Online& n = *online;
    const auto held = n.hold();
    n.now = n.thread->now();
    if (n.lastFrame >= 0 && n.frames.size() < 100000) n.frames.push_back(float((n.now - n.lastFrame) * 1000));
    n.lastFrame = n.now;
    auto forget = [&](p2p::PeerHandle peer, const char* why) {
        auto found = n.remotes.find(peer);
        if (found == n.remotes.end()) return;
        Online::Remote& r = found->second;
        saida::Log::info("[World online] ", r.name, " left: ", why);
        n.releaseBody(r);
        n.witness.forget(r.id);
        if (punchRemote == r.id) punchRemote = 0;
        n.remotes.erase(found);
    };
    for (const auto& e : n.host->takeEvents()) {
        if (e.type == p2p::HostEventType::Connected) n.speaks(e.peer);  // another version is said at once
        if (e.type == p2p::HostEventType::Disconnected) {
            forget(e.peer, p2p::reasonName(e.reason));
            n.sameVersion.erase(e.peer), n.welcomedBy.erase(e.peer);
            if (n.friendPeer == e.peer) n.friendPeer = p2p::kNoPeer, n.went = false;
        }
    }
    for (const auto& e : n.meet->takeEvents()) {
        if (e.type == p2p::RendezvousEventType::PeerLeft && !n.shares(e.peer)) forget(e.peer, "no longer near");
        // A friend with our code: we tell him it is ours, so he knows whom to join.
        if (e.type == p2p::RendezvousEventType::PeerJoined && e.topic == n.invite && n.speaks(e.peer)) {
            o::Message m;
            m.kind = o::Kind::Welcome, m.sent = n.now;
            n.send(m, p2p::Channel::Reliable, e.peer);
            ++n.welcomed;
            const p2p::PeerInfo* info = n.host->peer(e.peer);
            saida::Log::info("[World online] ", info ? o::playerName(info->id) : "a player", " joins us with our invite code");
        }
    }
    seekFriend();
    // The cells around him: joined within kMeetRadius, left beyond kLeaveRadius.
    if (n.now >= n.nextCells) {
        n.nextCells = n.now + .5;
        std::set<o::Cell> keep;
        const double atLon = arriving ? drop.goalLon : lon, atLat = arriving ? drop.goalLat : lat;
        if (playing || arriving) {
            for (const auto& c : o::cellsWithin(atLon, atLat, o::kLeaveRadius)) keep.insert(c);
            for (const auto& c : o::cellsWithin(atLon, atLat, o::kMeetRadius))
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
    n.fresh = playing && player && feet && drop.phase != Drop::Zoom;
    if (arriving && drop.phase == Drop::Zoom) {
        const auto goal = ecef(drop.goalLon, drop.goalLat, 0.);
        n.meet->setPosition(goal.x, goal.y, goal.z);
    }
    if (n.fresh) {
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
        a.busy = menu || playerMenu;
        a.invincible = health.invincible();
        a.health = uint8_t(std::clamp(std::ceil(health.hp), 0., 100.));
        // Back from the dead is a new life, even where he fell.
        if (n.ownKnown && n.own.health == 0 && a.health > 0) ++n.life;
        a.life = n.life;
        n.own = a, n.ownKnown = true, n.lastOwn = n.now;
        n.history.record(n.now, a);
        const auto feetAt = o::feetEcef(a);
        n.meet->setPosition(feetAt[0], feetAt[1], feetAt[2]);
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
    if (n.now >= n.nextSummary) {
        n.nextSummary = n.now + 60;
        n.summary("");
    }
    onlineHud();
}

void World::Impl::onlineHud() {
    Online& n = *online;
    if (n.now - n.hudAt < .5) return;
    n.hudAt = n.now;
    size_t near = 0;
    for (const auto& [peer, r] : n.remotes) near += r.visible;
    const auto status = n.meet->status();
    const std::string line = n.joinSaid.empty() ? o::panelLine(near, n.foreign(), status.population, status.dhtReady, status.problem)
                                                : "Online · " + n.joinSaid;
    if (line == n.hud) return;
    n.hud = line;
    text("online", line);
}

void World::Impl::showOnline(double dt) {
    if (!online) return;
    SAIDA_PROFILE_SCOPE("World/OnlinePlayers");
    Online& n = *online;
    const auto held = n.hold();
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
        // A suspect's blows are not believed here, nor shown.
        if (m.kind != o::Kind::Hit && n.witness.suspected(r.id)) {
            n.refuse("a blow from " + r.name, "he is suspected: " + n.witness.reason(r.id));
            continue;
        }
        if ((m.kind == o::Kind::Shot && m.shot.victim == me) || (m.kind == o::Kind::Punch && m.punch.victim == me) ||
            (m.kind == o::Kind::Hit && m.hit.shooter == me))
            r.lastContact = n.now;
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
            if (m.shot.victim && m.shot.victim != me) n.witness.shot(r.id, m.shot, m.sent, playerHeight, n.now);
            if (m.shot.victim != me) {
                if (!m.shot.victim) effects.burst(r1::CombatEffects::Dust, end);
                else effects.burst(r1::CombatEffects::Blood, end);
                break;
            }
            o::Avatar then;
            if (!judged(m.shot.seen, then)) break;
            const auto verdict = o::judgeShot(m.shot, shooter, then, playerHeight, n.now - r.lastShot);
            o::Message reply;
            reply.kind = o::Kind::Hit, reply.sent = n.now, reply.hit.victim = me, reply.hit.shooter = r.id;
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
            n.send(reply, p2p::Channel::Reliable);  // to everyone near: the witnesses check our word
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
            reply.kind = o::Kind::Hit, reply.sent = n.now, reply.hit.victim = me, reply.hit.shooter = r.id;
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
            n.send(reply, p2p::Channel::Reliable);
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
            // His word on another's claim: the witnesses' to weigh.
            if (m.hit.victim != r.id) break;
            if (m.hit.shooter != me) {
                n.witness.verdict(r.id, m.hit, n.now);
                break;
            }
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
    // What the witness made of it: every strike said, a new suspect shown.
    n.witness.tick(n.now);
    for (const auto& s : n.witness.takeStrikes()) {
        const std::string who = o::playerName(s.id);
        saida::Log::info("[World online] witness: ", who, " ", s.why, " (suspicion ", number(s.suspicion, 1), ")");
        if (s.suspectedNow) {
            saida::Log::info("[World online] ", who, " is suspected of cheating and no longer drawn here: ", s.why);
            text("stream-status", who + " hidden: " + s.why + ".");
        }
    }
    // Their bodies, drawn where their owners were a moment ago: the nearest
    // kMaxDrawn, the first kMaxPlayers of them in full, the others as the
    // crowd is drawn (a pose at a lower rate, held between samples, no arms
    // aimed). Those already drawn keep their place against a newcomer
    // barely nearer, so the edges do not flicker.
    const glm::vec3 up(0, 1, 0);
    struct Drawn {
        o::Avatar a;
        bool show = false, full = false;
    };
    std::map<p2p::PeerHandle, Drawn> plan;
    std::vector<std::pair<double, p2p::PeerHandle>> nearest;
    for (auto& [peer, r] : n.remotes) {
        Drawn& d = plan[peer];
        const auto kind = r.replica.sample(n.now, d.a, o::lerp);
        ++r.samples[size_t(kind)];
        const bool away = kind == nc::ReplicaSample::Empty || n.now - r.replica.lastArrival() > o::kAwaySeconds;
        d.show = !away && d.a.pose != o::Pose::Vehicle && playing && !n.witness.suspected(r.id);
        r.distance = d.show ? metresBetween(lon, lat, d.a.lon, d.a.lat) : 1e18;
        if (r.distance > o::kDrawnMetres) d.show = false, r.distance = 1e18;  // a friend far away, before we go to him
        if (d.show) nearest.push_back({r.distance * (r.visible ? .85 : 1.), peer});
    }
    std::sort(nearest.begin(), nearest.end());
    for (size_t i = 0; i < nearest.size(); ++i) {
        plan[nearest[i].second].full = i < o::kMaxPlayers;
        if (i >= o::kMaxDrawn) plan[nearest[i].second].show = false;
    }
    for (auto& [peer, r] : n.remotes) {
        const o::Avatar& a = plan[peer].a;
        const bool show = plan[peer].show;
        if (!show && r.root) n.releaseBody(r);
        if (show && !r.root) {
            if (!n.spare.empty()) {
                const auto b = n.spare.back();
                n.spare.pop_back();
                r.root = b.root, r.body = b.body, r.bodyRest = b.bodyRest, r.animator = b.animator;
                r.arms[0] = b.arms[0], r.arms[1] = b.arms[1], r.pistol = b.pistol, r.launcher = b.launcher, r.capsule = b.capsule;
                r.fall.bind(*b.ragdoll, *r.capsule, r1::combat::kLieSeconds);
                r.clip.clear();
                r.poseRate = -1;
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
        // Animation LOD, as for the crowd (people.cpp): every frame close by,
        // 15, 8 and 4 Hz further out; a body drawn as the crowd never closer
        // than 8 Hz, its arms left to the clip.
        const bool full = plan[peer].full;
        const float rate = r.distance < 12 && full ? 0.f : r.distance < 30 && full ? 15.f : r.distance < 60 ? 8.f : 4.f;
        if (rate != r.poseRate) r.animator->setPoseRate(rate, saida::Animator::PoseRateMode::Hold), r.poseRate = rate;
        if (!full) {
            for (auto* arm : r.arms)
                if (arm && arm->weight() > 0) arm->release(), arm->clearTipRotation();
            r.punch = -1;
            r.shown = a;
            continue;
        }
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
    showSharedCars();
}

void World::Impl::showSharedCars() {
    Online& n = *online;
    const auto held = n.hold();
    const uint64_t me = n.host->identity().id();
    // Ours first: one another player took from us, he keeps (o::outranks);
    // the rest we say where they are.
    for (auto it = ownedCars.begin(); it != ownedCars.end();) {
        saida::Node* node = it->first;
        OwnedCar& own = it->second;
        const auto other = n.sharedCars.find(own.id);
        if (other != n.sharedCars.end() && other->second.owner != me &&
            o::outranks(other->second.claim, other->second.owner, own.claim, me)) {
            const std::string who = o::playerName(other->second.owner);
            saida::Log::info("[World online] ", who, " took the car ", own.id, " (take ", other->second.claim, " against ours, ",
                             own.claim, "): it is his");
            if (node == car && driving()) leaveCar();
            text("stream-status", who + " has taken this car.");
            if (node == car) car = nullptr, carParked = false;
            for (auto p = parked.begin(); p != parked.end(); ++p)
                if (p->node == node) {
                    parked.erase(p);
                    break;
                }
            node->queueFree();
            it = ownedCars.erase(it);
            continue;
        }
        if (other != n.sharedCars.end()) {  // an older claim than ours: ours now
            if (other->second.node) other->second.node->queueFree();
            n.sharedCars.erase(other);
        }
        const bool driven = node == car && driving();
        if (n.now >= own.nextSay && node->enabled()) {
            own.nextSay = n.now + (driven ? o::kCarSendSeconds : o::kCarParkedSeconds);
            o::Message m;
            m.kind = o::Kind::Vehicle, m.sent = n.now;
            m.vehicle = {own.id, own.seed, own.model, own.claim, carState(*node)};
            n.send(m, p2p::Channel::Unreliable);
        }
        ++it;
    }
    // Theirs: drawn where their owners say, and their places left empty.
    for (auto it = n.sharedCars.begin(); it != n.sharedCars.end();) {
        const uint64_t id = it->first;
        Online::SharedCar& c = it->second;
        takenCars.insert(id);
        const double silent = n.now - c.heard;
        if (c.last.driven && silent > o::kCarGoneSeconds) {
            saida::Log::info("[World online] the car ", id, " of ", o::playerName(c.owner), " is gone, not heard of for ", int(silent), " s");
            if (c.node) c.node->queueFree();
            it = n.sharedCars.erase(it);
            continue;
        }
        o::Car at = c.last;
        if (c.last.driven) c.replica.sample(n.now, at, o::lerpCar);
        const bool show = playing && !(c.last.driven && silent > o::kAwaySeconds) && c.model < fleet.size() &&
                          metresBetween(lon, lat, at.lon, at.lat) < kParkedRadius * 3;
        if (!show) {
            if (c.node) c.node->setEnabled(false);
            ++it;
            continue;
        }
        if (!c.node) {
            auto made = clonePlant(*fleet[c.model].prototype);
            vehicleCollider(*made, fleet[c.model]);
            paintCar(*made, paintFor(c.seed));
            c.node = engine.sceneTree().world().addChild(std::move(made));
        }
        c.node->setEnabled(true);
        if (!r1::VehicleRuntime::physical(*c.node)) {
            c.node->transform().position = glm::vec3(origin.local(ecef(at.lon, at.lat, at.alt + r1::VehicleRuntime::kBodyClearance)));
            c.node->transform().rotation = glm::angleAxis(float(-at.yaw * rad), glm::vec3(0, 1, 0)) *
                                           glm::angleAxis(float(at.pitch * rad), glm::vec3(1, 0, 0)) *
                                           glm::angleAxis(float(-at.roll * rad), glm::vec3(0, 0, 1));
        }
        ++it;
    }
}

bool World::Impl::nearestSharedCar(Reach& out, double& distance) {
    if (!online) return false;
    const auto held = online->hold();
    bool found = false;
    const glm::dvec3 me = ecef(lon, lat, alt);
    for (auto& [id, c] : online->sharedCars) {
        if (!c.node || !c.node->enabled() || c.last.driven) continue;  // nobody takes a car someone drives
        const double d = glm::length(me - ecef(c.last.lon, c.last.lat, c.last.alt));
        if (d < distance) distance = d, out = {c.node, nullptr, 0, 4}, found = true;
    }
    return found;
}

bool World::Impl::takeSharedCar(const Reach& target) {
    if (!online) return false;
    Online& n = *online;
    const auto held = n.hold();
    for (auto it = n.sharedCars.begin(); it != n.sharedCars.end(); ++it) {
        if (it->second.node != target.node) continue;
        const uint64_t id = it->first;
        const uint16_t claim = uint16_t(it->second.claim + 1);
        const uint64_t from = it->second.owner;
        const uint32_t seed = it->second.seed;
        const uint8_t model = it->second.model;
        const float heading = it->second.last.yaw;
        saida::Node* node = it->second.node;
        n.sharedCars.erase(it);
        abandonCurrentCar();
        car = node;
        const auto point = origin.origin + origin.basis * glm::dvec3(car->transform().position);
        const auto geo = r1::ecefToGeodetic(point.x, point.y, point.z);
        carParked = true, carLon = geo.x, carLat = geo.y, carAlt = groundAt(carLon, carLat, alt), carYaw = heading;
        carSpeed = 0, wheelSpin = 0, steerShown = 0;
        collectWheels(*car);
        ownedCars[car] = {id, seed, model, claim, 0};
        saida::Log::info("[World online] took the car ", id, " that ", o::playerName(from), " left (take ", claim, ")");
        return true;
    }
    return false;
}

uint64_t World::Impl::remoteByBody(const saida::RaycastHit& hit, double& seen) {
    if (!online || !hit.hit) return 0;
    const auto held = online->hold();
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
    const auto held = online->hold();
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
    const auto held = online->hold();
    for (const auto& [peer, r] : online->remotes)
        if (r.id == id && r.root) return r.root->transform().position;
    return player->transform().position;
}

void World::Impl::sendShot(const glm::vec3& muzzle, const glm::vec3& direction, float length, uint64_t victim, double seen) {
    if (!online) return;
    const auto held = online->hold();
    o::Message m;
    m.kind = o::Kind::Shot, m.sent = online->now;
    const glm::dvec3 at = origin.origin + origin.basis * glm::dvec3(muzzle);
    const glm::dvec3 d = glm::normalize(origin.basis * glm::dvec3(direction));
    m.shot.muzzle = {at.x, at.y, at.z};
    m.shot.direction = {float(d.x), float(d.y), float(d.z)};
    m.shot.length = std::clamp(length, .01f, float(r1::combat::kPistol.range));
    m.shot.victim = victim, m.shot.seen = seen;
    if (victim) online->contact(victim);
    online->send(m, p2p::Channel::Reliable);
}

void World::Impl::sendPunch(uint64_t victim, double seen, bool hook) {
    if (!online) return;
    const auto held = online->hold();
    o::Message m;
    m.kind = o::Kind::Punch, m.sent = online->now;
    m.punch.victim = victim, m.punch.seen = seen, m.punch.hook = hook;
    if (victim) online->contact(victim);
    online->send(m, p2p::Channel::Reliable);
}

void World::Impl::sendRocket(const glm::vec3& muzzle, const glm::vec3& target) {
    if (!online) return;
    const auto held = online->hold();
    o::Message m;
    m.kind = o::Kind::Rocket, m.sent = online->now;
    const glm::dvec3 a = origin.origin + origin.basis * glm::dvec3(muzzle), b = origin.origin + origin.basis * glm::dvec3(target);
    m.rocket.muzzle = {a.x, a.y, a.z}, m.rocket.target = {b.x, b.y, b.z};
    online->send(m, p2p::Channel::Reliable);
}

// Two instances at one spot (tools/online_smoke.py): each must draw the other;
// the shooter fires at the body he draws until the target's machine says it
// was hit; the target passes once hit by the other player.
void World::Impl::stopOnline() {
    if (!online) return;
    {
        const auto held = online->hold();
        for (auto& [peer, r] : online->remotes) online->releaseBody(r);
        for (auto& body : online->spare) body.root->queueFree();
        online->spare.clear();
        for (auto& [id, c] : online->sharedCars)
            if (c.node) c.node->queueFree();
        online->sharedCars.clear();
    }
    online.reset();  // the deleter stops the thread and says the last summary
    onlineStarted = false;
    punchRemote = 0;
}

void World::Impl::setOnlinePlay(bool on) {
    onlinePreference = on;
    if (on != onlineWanted) {
        onlineWanted = on;
        saida::Log::info("[World options] multiplayer ", on ? "on" : "off");
        if (!on) stopOnline(), friendCode.clear();
        else onlineStarted = false;  // pumpOnline opens it, in play or for a friend to join
    }
    sayOnlinePlay();
    sayInvite();
    saveOptions();
}

const std::string& World::Impl::invite() {
    if (inviteCode.empty()) {
        // The smoke's host is given his, so that its joiner can be given it too.
        const char* given = smoke ? std::getenv("R1WORLD_INVITE_CODE") : nullptr;
        if (given && !p2p::normalizeArenaCode(given, inviteCode))
            throw std::runtime_error(std::string("R1WORLD_INVITE_CODE is not an invite code: ") + given);
        if (!given) inviteCode = p2p::newArenaCode();
    }
    return inviteCode;
}

void World::Impl::sayInvite() {
    text("invite-code", onlineWanted ? invite() : "—");
    if (!onlineWanted) text("join-status", "Multiplayer is off: turn it on above to get a code and join a friend.");
    else if (online && !online->joinSaid.empty()) text("join-status", online->joinSaid);
}

void World::Impl::joinFriend(const std::string& typed) {
    std::string code, refusal;
    if (!onlineWanted) refusal = "Multiplayer is off: turn it on above, then join.";
    else if (!p2p::normalizeArenaCode(typed, code))
        refusal = "\"" + typed + "\" is not an invite code: eight letters and digits, like K7QM-2XPA.";
    else if (code == invite()) refusal = "That is your own code: give it to a friend, who types it here.";
    if (!refusal.empty()) {
        saida::Log::info("[World online] join refused: ", refusal);
        text("join-status", refusal);
        return;
    }
    friendCode = code;
    saida::Log::info("[World online] joining the player with code ", code);
    text("join-status", o::joinLine(o::Join::Looking, code, ""));
}

void World::Impl::seekFriend() {
    Online& n = *online;
    if (n.joinCode != friendCode) {  // a new code, or none any more
        if (!n.joinCode.empty()) n.meet->leave(n.joinTopic);
        n.joinCode = friendCode, n.joinSince = n.now, n.friendPeer = p2p::kNoPeer, n.went = false;
        if (!friendCode.empty()) n.joinTopic = p2p::arenaTopic(o::kApplication, friendCode), n.meet->join(n.joinTopic);
    }
    if (friendCode.empty()) return;
    const auto members = n.meet->peers(n.joinTopic);
    bool foreign = false;
    for (p2p::PeerHandle p : members) {
        if (!n.speaks(p)) foreign = true;
        else if (n.friendPeer == p2p::kNoPeer && n.welcomedBy.count(p)) n.friendPeer = p;
    }
    const p2p::PeerInfo* info = n.friendPeer != p2p::kNoPeer ? n.host->peer(n.friendPeer) : nullptr;
    const std::string name = info ? o::playerName(info->id) : "";
    const auto found = n.remotes.find(n.friendPeer);
    const o::Avatar* him = info && found != n.remotes.end() ? found->second.replica.newest() : nullptr;
    o::Join state = o::Join::Looking;
    if (!info) state = foreign ? o::Join::Foreign : n.now - n.joinSince > o::kJoinQuietSeconds ? o::Join::Quiet : o::Join::Looking;
    else if (!him || (!n.went && (o::notYet(*him) || n.now < n.nextGo))) state = o::Join::Waiting;
    else if (!n.went) {
        // Go, as the Destination tab goes: the typed place is beside him.
        const auto at = o::besideFriend(*him);
        field("latitude", number(at[1], 7)), field("longitude", number(at[0], 7));
        field("city-query", name), cityInput = citySubmitted = name;
        saida::Log::info("[World online] going to ", name, " at ", at[0], ", ", at[1],
                         ", ", int(metresBetween(lon, lat, him->lon, him->lat)), " m away");
        n.went = go();  // a refusal is said by go(), and tried again
        if (n.went) showOptions(false);
        else n.nextGo = n.now + 5;
        state = n.went ? o::Join::Going : o::Join::Waiting;
    } else if (playing && drop.phase == Drop::None && metresBetween(lon, lat, him->lon, him->lat) < o::kMeetRadius &&
               n.shares(n.friendPeer, true)) {
        // Arrived, and in a cell with him: the cells hold us together from
        // now on (leaving his code's topic sooner would drop him a moment).
        saida::Log::info("[World online] joined ", name, ": ", number(metresBetween(lon, lat, him->lon, him->lat), 1), " m from him");
        text("stream-status", o::joinLine(o::Join::Joined, friendCode, name) + ".");
        text("join-status", o::joinLine(o::Join::Joined, friendCode, name) + ".");
        n.joinedAt = n.now, n.joinSaid.clear(), n.hudAt = -1;
        friendCode.clear();
        return;
    } else state = o::Join::Going;
    const std::string line = o::joinLine(state, friendCode, name);
    if (line == n.joinSaid) return;
    if (state == o::Join::Waiting && him && o::notYet(*him)) saida::Log::info("[World online] ", name, " is ", o::notYet(*him), ": waiting");
    else saida::Log::info("[World online] join: ", line);
    n.joinSaid = line, n.hudAt = -1;
    text("join-status", line);
}

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
    const auto held = n.hold();  // recursive: fire() below sends under it too
    const bool shooter = std::string(onlineSmoke) == "shooter";
    Online::Remote* other = nullptr;
    for (auto& [peer, r] : n.remotes)
        if (r.visible && r.root) other = &r;
    // Two builds that differ: each must link the other, say it on the panel,
    // and never draw him.
    if (std::string(onlineSmoke) == "versions") {
        if (other) return fail("drew " + other->name + ", who is on another version");
        if (n.passedAt >= 1e17) return;
        if (n.passedAt >= 0) {
            if (n.now - n.passedAt > 3) {  // lingers, so the other one says it too
                saida::Log::info("[World E2E] PASS online versions: \"", n.hud, "\", nothing drawn, refusals ", n.refused.size());
                n.passedAt = 1e18;
                engine.sceneTree().quit();
            }
            return;
        }
        if (n.hud.find("on another version") != std::string::npos) n.passedAt = n.now;
        else if (n.now > 200) fail("never said the other version (panel: \"" + n.hud + "\", " + std::to_string(n.host->peers().size()) + " linked)");
        return;
    }
    // A shared car: the driver takes the parked car nearest him and drives
    // it away; the watcher must see it move, its place left empty. Both
    // check the shared world first: no traffic, nobody, parked cars.
    if (std::string(onlineSmoke) == "driver" || std::string(onlineSmoke) == "watcher") {
        const bool driver = std::string(onlineSmoke) == "driver";
        if (n.passedAt >= 1e17) return;
        if (n.passedAt >= 0) {
            // Meanwhile he looks at the parked car nearest him, for the picture.
            if (driver) {
                double nearest = 1e9;
                for (auto& [key, tile] : loaded)
                    for (const auto& p : tile.kerbCars)
                        if (p.node) {
                            const auto here = geodeticOf(tile, p.node->transform().position);
                            const glm::dvec3 to = origin.local(ecef(here.x, here.y, alt)) - origin.local(ecef(lon, lat, alt));
                            const double d = glm::length(to);
                            if (d > 6 && d < nearest) nearest = d, yaw = std::atan2(to.x, -to.z) / rad;
                        }
            }
            if (n.now - n.passedAt > (driver ? 6 : 2)) {  // the driver lingers, so the watcher hears where the car stands
                saida::Log::info("[World E2E] PASS online ", onlineSmoke, ": ", n.passed);
                n.passedAt = 1e18;
                // The driver's view of the street he parked in, to look at.
                if (const char* png = std::getenv("R1WORLD_ONLINE_SHOT"); png && driver) {
                    saida::CaptureRequest shot;
                    shot.pngPath = png, shot.frame = 1, shot.waitForAssets = false;
                    engine.captureFrameThenExit(shot);
                } else engine.sceneTree().quit();
            }
            return;
        }
        size_t drawn = 0, places = 0;
        int people = 0;
        double worstGap = 0;  // a parked car's body above or below the ground under it
        for (auto& [key, tile] : loaded) {
            places += tile.kerbCars.size();
            people += tile.crowd.live();
            for (const auto& p : tile.kerbCars)
                if (p.node) {
                    ++drawn;
                    const auto at = p.node->transform().position;
                    const auto geo = geodeticOf(tile, at);
                    const double ground = groundUnder(tile, at.x, at.z, p.up);
                    const double gap = std::abs(at.y - r1::VehicleRuntime::kBodyClearance - ground);
                    if (gap > worstGap && gap > .6) {
                        saida::Log::info("[World parking] car ", p.spot.id, " at ", geo.x, ", ", geo.y, ": road ", p.up, " m, ground ", ground,
                                         " m, body ", at.y, " m, lane ", p.spot.lane, " speed ", tile.graph.lanes[p.spot.lane].speed,
                                         ", physical ", r1::VehicleRuntime::physical(*p.node));
                    }
                    worstGap = std::max(worstGap, gap);
                }
        }
        if (worstGap > .6) return fail("a parked car stands " + number(worstGap, 2) + " m off the ground under it");
        if (trafficLive() > 0) return fail(std::to_string(trafficLive()) + " traffic cars in a shared world");
        if (people > 0) return fail(std::to_string(people) + " passers-by in a shared world");
        if (n.now > 240)
            return fail(driver ? "never drove a parked car away (" + std::to_string(drawn) + " parked cars drawn of " + std::to_string(places) + ")"
                               : "never saw the other player's car move (" + std::to_string(n.sharedCars.size()) + " cars heard of)");
        auto placeOf = [&](uint64_t id, glm::dvec2& at) {
            for (auto& [key, tile] : loaded)
                for (const auto& p : tile.kerbCars)
                    if (p.spot.id == id) {
                        const auto point = tile.frame.origin + tile.frame.basis * glm::dvec3(p.spot.x, p.up, p.spot.y);
                        const auto geo = r1::ecefToGeodetic(point.x, point.y, point.z);
                        at = {geo.x, geo.y};
                        return p.node == nullptr;  // its place empty
                    }
            return false;
        };
        if (!driver) {
            for (auto& [id, c] : n.sharedCars) {
                glm::dvec2 at;
                if (!c.node || !c.node->enabled() || !placeOf(id, at)) continue;
                const double moved = metresBetween(at.x, at.y, c.last.lon, c.last.lat);
                if (moved < 8) continue;
                n.passedAt = n.now;
                n.passed = "saw " + o::playerName(c.owner) + "'s car " + number(moved, 1) + " m from its place, the place empty, " +
                           std::to_string(drawn) + " parked cars drawn of " + std::to_string(places) + ", worst " + number(worstGap, 2) +
                           " m off the ground, no traffic, nobody";
            }
            return;
        }
        // His picture is taken in daylight (a test run saves no option).
        if (driver && std::getenv("R1WORLD_ONLINE_SHOT") && !forcedMinutes) forceTime(14 * 60);
        // The driver: once the other is drawn, to the nearest parked car.
        bool other = false;
        for (auto& [peer, r] : n.remotes) other |= r.visible;
        if (!other && !driving() && smokeDriveTime == 0) return;
        if (!driving() && smokeDriveTime == 0) {
            Reach target;
            double distance = 1e9;
            const glm::dvec3 me = ecef(lon, lat, alt);
            for (auto& [key, tile] : loaded)
                for (size_t i = 0; i < tile.kerbCars.size(); ++i)
                    if (auto* node = tile.kerbCars[i].node) {
                        const auto here = geodeticOf(tile, node->transform().position);
                        const double d = glm::length(me - ecef(here.x, here.y, alt));
                        if (d < distance) distance = d, target = {node, &tile, i, 3};
                    }
            if (!target.node) return;
            if (distance > 3) {
                const auto here = geodeticOf(*target.tile, target.node->transform().position);
                const glm::dvec3 to = origin.local(ecef(here.x, here.y, alt)) - origin.local(me);
                yaw = std::atan2(to.x, -to.z) / rad;
                smokeToCar = true;
                return;
            }
            smokeToCar = false;
            if (!enterCar() || !ownedCars.count(car)) return fail("could not take the parked car " + number(distance, 1) + " m away");
            smokeDriving = true, smokeBrake = false, smokeDriveTime = 1e-6;
            smokeDriveStart = ecef(lon, lat, alt);
            return;
        }
        smokeDriveTime += dt;
        if (smokeDriveTime > 4) smokeBrake = true;
        if (smokeDriveTime > 7 && driving()) {
            smokeDriving = false;
            leaveCar();
            const double moved = glm::length(ecef(carLon, carLat, carAlt) - smokeDriveStart);
            if (moved < 8) return fail("the parked car moved only " + number(moved, 1) + " m");
            n.passedAt = n.now;
            n.passed = "took a parked car (" + std::to_string(ownedCars.size()) + " owned) and drove it " + number(moved, 1) + " m, " +
                       std::to_string(drawn) + " parked cars drawn of " + std::to_string(places) + ", worst " + number(worstGap, 2) +
                       " m off the ground, no traffic, nobody";
        }
        return;
    }
    // A friend joined by his code: the joiner starts kilometres away with
    // --join, the host gives him his code; each must end up drawing the
    // other a few metres away, the joiner having been taken there.
    if (std::string(onlineSmoke) == "host" || std::string(onlineSmoke) == "joiner") {
        const bool host = std::string(onlineSmoke) == "host";
        if (n.passedAt >= 1e17) return;
        Online::Remote* beside = nullptr;
        for (auto& [peer, r] : n.remotes)
            if (r.visible && r.root && glm::distance(r.root->transform().position, player->transform().position) < 30) beside = &r;
        if (n.passedAt >= 0) {
            if (n.now - n.passedAt > 3) {  // lingers, so the other one sees him too
                saida::Log::info("[World E2E] PASS online ", onlineSmoke, ": ", n.passed);
                n.passedAt = 1e18;
                engine.sceneTree().quit();
            }
            return;
        }
        if (beside && (host ? n.welcomed > 0 : n.joinedAt >= 0)) {
            n.passedAt = n.now;
            n.passed = (host ? std::to_string(n.welcomed) + " welcomed" : "joined after " + number(n.joinedAt, 1) + " s") +
                       ", drawing " + beside->name + " at " +
                       number(glm::distance(beside->root->transform().position, player->transform().position), 1) + " m";
        }
        else if (n.now > 240)
            fail(host ? "the joiner never came (" + std::to_string(n.welcomed) + " welcomed, " + std::to_string(n.host->peers().size()) + " linked)"
                      : "never joined (" + (n.joinSaid.empty() ? std::string("nothing said") : n.joinSaid) + ")");
        return;
    }
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
                         ", refusals ", n.refused.size(), drawing, ", snapshots sent near/middle/far ",
                         n.sentByTier[0], "/", n.sentByTier[1], "/", n.sentByTier[2]);
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
